#!/usr/bin/env python3
"""AFS survey: look inside the game's big archives (PZS3US1.AFS, PZS3US2.AFS) WITHOUT uploading them.

Run it on your PC (Windows: `py afs_survey.py PZS3US1.AFS PZS3US2.AFS`), then send the small `afs_survey.zip` it makes (a few MB to ~100 MB).
It writes, per archive:
  <name>_listing.txt   every entry: index, offset, size, name, the first 16 bytes
  <name>_types.txt     how many entries of each kind (by first bytes) and their sizes
  <name>_toc.bin       the archive's table of contents only (header, offset/size table, name table)
  samples/<name>/...   a few small examples of every kind of entry (the whole entry), up to the limits below
Pull out chosen entries (whole, for me to look at):  --dump 12,15,2500-2511   or   --dump-name Map_35,WorldTou   (copied to <out>/dump/<archive>/)
Options:  --per-type N (default 4)  --max-file-mb N (default 6)  --max-total-mb N (default 120)  --out DIR (default afs_survey)
Nothing is changed or sent anywhere; the archives are only read."""
import argparse, collections, os, struct, sys, zipfile


def kind(head):
    if head[:4] == b"AFS\0":
        return "AFS"
    if head[:4] == b"TIM2":
        return "TIM2"
    if head[:2] == b"\x80\x00":
        return "ADX"
    if head[:4] in (b"RIFF", b"FORM", b"MThd", b"OggS", b"IECS", b"SShd", b"SSbd"):
        return head[:4].decode("ascii", "replace")
    if head[:3] == b"\x00\x00\x01":
        return "MPEG"
    if all(32 <= c < 127 for c in head[:4]):
        return "txt:" + head[:4].decode("ascii")
    return "bin:" + head[:4].hex()


def read_toc(f):
    f.seek(0)
    hdr = f.read(8)
    if hdr[:4] != b"AFS\0":
        raise SystemExit("not an AFS file (no AFS header)")
    count = struct.unpack_from("<I", hdr, 4)[0]
    table = f.read(count * 8)
    ents = [struct.unpack_from("<II", table, i * 8) for i in range(count)]
    names = [None] * count
    meta = f.read(8)
    name_off = name_len = 0
    if len(meta) == 8:
        name_off, name_len = struct.unpack("<II", meta)
    if name_off and name_len >= count * 0x30:
        f.seek(name_off)
        blob = f.read(count * 0x30)
        for i in range(count):
            raw = blob[i * 0x30:i * 0x30 + 32]
            names[i] = raw.split(b"\0")[0].decode("cp932", "replace")  # Shift-JIS: the Japanese names
    toc_end = 8 + count * 8 + 8
    return count, ents, names, name_off, name_len, toc_end


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("afs", nargs="+")
    ap.add_argument("--per-type", type=int, default=4)
    ap.add_argument("--max-file-mb", type=float, default=6)
    ap.add_argument("--max-total-mb", type=float, default=120)
    ap.add_argument("--out", default="afs_survey")
    ap.add_argument("--dump", default="", help="entry numbers, e.g. 12,15,2500-2511")
    ap.add_argument("--dump-name", default="", help="names containing any of these words, comma separated")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    total = 0
    written = []
    for path in a.afs:
        base = os.path.splitext(os.path.basename(path))[0]
        with open(path, "rb") as f:
            count, ents, names, name_off, name_len, toc_end = read_toc(f)
            size_of_file = os.path.getsize(path)
            rows, kinds = [], collections.defaultdict(list)
            for i, (off, size) in enumerate(ents):
                if off + size > size_of_file or size == 0:
                    rows.append((i, off, size, names[i] or "", "(outside the file or empty)", ""))
                    continue
                f.seek(off)
                head = f.read(16)
                k = kind(head)
                rows.append((i, off, size, names[i] or "", k, head.hex()))
                kinds[k].append((size, i))
            lp = os.path.join(a.out, base + "_listing.txt")
            with open(lp, "w") as o:
                o.write(f"{path}: {count} entries, {size_of_file} bytes, name table at {name_off:#x} ({name_len} bytes)\n")
                o.write("index  offset    size      kind          name                              first16\n")
                for r in rows:
                    o.write(f"{r[0]:5d}  {r[1]:08x}  {r[2]:9d}  {r[4]:12s}  {r[3]:32s}  {r[5]}\n")
            with open(os.path.join(a.out, base + "_types.txt"), "w") as o:
                for k, v in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
                    sz = sorted(s for s, _ in v)
                    o.write(f"{k:14s} {len(v):6d} entries, {sum(sz) / 1048576:9.1f} MB, smallest {sz[0]}, median {sz[len(sz) // 2]}, largest {sz[-1]}\n")
            f.seek(0)
            toc = f.read(toc_end)
            if name_off and name_len:
                f.seek(name_off)
                toc += f.read(min(name_len, 8 * 1048576))
            with open(os.path.join(a.out, base + "_toc.bin"), "wb") as o:
                o.write(toc)
            sdir = os.path.join(a.out, "samples", base)
            os.makedirs(sdir, exist_ok=True)
            for k, v in kinds.items():
                for size, i in sorted(v)[:a.per_type]:
                    if size > a.max_file_mb * 1048576 or total + size > a.max_total_mb * 1048576:
                        continue
                    f.seek(ents[i][0])
                    nm = (names[i] or "entry").replace("/", "_").replace("\\", "_")
                    fn = os.path.join(sdir, f"{i:05d}_{nm}.bin")
                    with open(fn, "wb") as o:
                        o.write(f.read(size))
                    total += size
            want = set()
            for part in a.dump.split(","):
                if "-" in part:
                    lo, hi = part.split("-")
                    want.update(range(int(lo), int(hi) + 1))
                elif part.strip():
                    want.add(int(part))
            words = [w.strip() for w in a.dump_name.split(",") if w.strip()]
            for i, nm in enumerate(names):
                if nm and any(w in nm for w in words):
                    want.add(i)
            if want:
                ddir = os.path.join(a.out, "dump", base)
                os.makedirs(ddir, exist_ok=True)
                for i in sorted(want):
                    if 0 <= i < count and ents[i][0] + ents[i][1] <= size_of_file:
                        f.seek(ents[i][0])
                        nm = (names[i] or "entry").replace("/", "_").replace("\\", "_").replace(" ", "_")
                        data = f.read(ents[i][1])
                        with open(os.path.join(ddir, f"{i:05d}_{nm}.bin"), "wb") as o:
                            o.write(data)
                        total += len(data)
                print(f"dumped {len(want)} entries to {ddir}")
            print(f"{path}: {count} entries, {len(kinds)} kinds; listing -> {lp}")
    zp = a.out.rstrip("/\\") + ".zip"
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(a.out):
            for fn in files:
                p = os.path.join(root, fn)
                z.write(p, os.path.relpath(p, os.path.dirname(a.out) or "."))
    mb = os.path.getsize(zp) / 1048576
    print(f"\n{zp}: {mb:.1f} MB" + ("  (over 450 MB: lower --max-total-mb)" if mb > 450 else "  - send this file"))


if __name__ == "__main__":
    main()
