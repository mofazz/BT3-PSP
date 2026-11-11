#!/usr/bin/env python3
"""BT3 -> PSP texture pack, the NFSU2 port's way: the costly work done once on the PC from the player's own disc.

Every texture of the game's sprite sheets (AFS entries that are packs of BPE-packed sheets; each sheet a pack of
texture files, src/sys/gfxm_b_b.h) is put through the PS2 GS memory as the game uploads it (usually 8- or 4-bit
pictures written as 32-bit), read back in its own format, and stored in the PSP GE's:
  - T8 / T4 kept as palette indices (the palette stays the game's: the GE reads it from GS memory as before),
  - 32-bit pictures as 8888, or with --clut8 median-cut to 256 colours (NFSU2: everything CLUT8),
  - at least 16 bytes a row and a multiple of 8 rows (padded), then GE-swizzled (16-byte x 8-row blocks: ~4x
    faster through the GE's 8 KB texture cache on a real PSP, NFSU2's measurement),
  - in disc order in one file (one Memory Stick read per texture, sorted index: NFSU2's packing).

Key of each texture: FNV-1a 64 of its whole PS2 pixel packet (TexEntry.pix, pixSize bytes), which the runtime can
compute at upload time (Tex_UploadPixels).

    python bt3_psp_tex.py <disc folder with DATA/pzs3us1.afs> [-o bt3psp.tex] [--clut8] [--max N]

File: 'BT3T', u32 version 1, u32 count, u32 index offset, u32 data offset, 12 bytes 0; then count entries of 32
bytes sorted by key: u64 key, u32 offset, u32 bytes, u16 w, u16 h (padded), u16 tw, u16 th (the PS2 texture's own
size), u8 format (0 = 8888, 1 = T8, 2 = T4, 3 = T8 with its own palette: 1024 bytes in front of the indices),
u8 swizzled, u8 shift (halvings: the UVs scale by it), u8 0, u32 0. Version 2.
"""
import argparse, struct, sys, time
from pathlib import Path

import numpy as np

# ------------------------------------------------------------------------------------------------ AFS, BPE, packs

def afs_entries(path):
    with open(path, 'rb') as f:
        head = f.read(8)
        if head[:3] != b'AFS':
            raise SystemExit(f'{path}: not an AFS archive')
        n, = struct.unpack_from('<I', head, 4)
        toc = struct.unpack(f'<{2 * n}I', f.read(8 * n))
    return [(toc[2 * i], toc[2 * i + 1]) for i in range(n)]


def bpe(buf, pos):
    """Philip Gage's byte-pair expansion (src/sys/bpe.c); each block's pairs expanded once, then joined."""
    raw, packed = struct.unpack_from('<II', buf, pos)
    src, p, end, out = buf, pos + 8, pos + 8 + packed, []
    while p < end:
        left, right = list(range(256)), [0] * 256
        c, count = 0, src[p]
        p += 1
        while True:
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c] = src[p]
                p += 1
                if c != left[c]:
                    right[c] = src[p]
                    p += 1
                c += 1
            if c == 256:
                break
            count = src[p]
            p += 1
        size = (src[p] << 8) | src[p + 1]
        p += 2
        exp = [None] * 256

        def expand(s, depth=0):
            if exp[s] is None:
                exp[s] = bytes([s]) if left[s] == s or depth > 300 else expand(left[s], depth + 1) + expand(right[s], depth + 1)
            return exp[s]
        out.append(b''.join(expand(s) for s in src[p:p + size]))
        p += size
    data = b''.join(out)
    if len(data) != raw:
        raise ValueError(f'BPE: {len(data)} of {raw} bytes')
    return data


def pack_sections(buf, size=None):
    """A pack (MPACK_AT): word 0 the count, words 1..count byte offsets. [] when buf is not one."""
    size = len(buf) if size is None else size
    if size < 8:
        return []
    n, = struct.unpack_from('<I', buf, 0)
    if n == 0 or n > 4096 or 4 + 4 * n > size:
        return []
    offs = struct.unpack_from(f'<{n}I', buf, 4)
    if offs[0] < 4 + 4 * n or any(o >= size or o & 3 for o in offs):
        return []
    return list(offs)


def bpe_ok(buf, at, limit):
    if at + 8 > limit:
        return False
    raw, packed = struct.unpack_from('<II', buf, at)
    return 16 < packed <= limit - at - 8 and packed < raw < packed * 64

# ------------------------------------------------------------------------------------------------ texture files

def tex_files(sheet):
    """(section offset, entries) of every texture file in an unpacked sheet."""
    offs = pack_sections(sheet)
    ends = sorted(set(offs + [len(sheet)]))
    for o in offs:
        end = ends[ends.index(o) + 1]
        length = end - o
        if length < 0x20:
            continue
        cnt, eo = struct.unpack_from('<II', sheet, o)
        eo *= 4
        if not 0 < cnt <= 512 or eo < 0x14 or eo + cnt * 0x40 > length:
            continue
        ents, ok = [], True
        for e in range(cnt):
            w = struct.unpack_from('<IIiiiiIIii8sQ', sheet, o + eo + e * 0x40)
            pix, clut, ps, cs, pixblt, tex0 = w[0], w[1], w[2], w[3], w[6], w[11]
            if pix and (ps < 0x70 or ps & 15 or pix * 4 + ps > length or tex0 == 0):
                ok = False
                break
            if clut and (cs < 0 or clut * 4 + cs > length):
                ok = False
                break
            ents.append((pix, ps, pixblt, tex0))
        if ok:
            yield o, ents

# ------------------------------------------------------------------------------------------------ the GS memory

BLOCK32 = np.array([[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]])
BLOCK16 = np.array([[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15], [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]])
COL32 = np.array([[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
                  [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]])
COL8F = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54], [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
         [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23], [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31]]
COL4F = [[0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
         [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
         [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
         [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63]]
COL8 = np.array([[COL8F[y & 3][(x ^ 4) if (y >> 2) & 1 else x] + (y >> 2) * 64 for x in range(16)] for y in range(16)])
COL4 = np.array([[COL4F[y & 3][(x ^ 4) if (y >> 2) & 1 else x] + (y >> 2) * 128 for x in range(32)] for y in range(16)])


def addr(psm, bw, w, h, x0=0, y0=0):
    """Address of each pixel of a w x h rectangle at block 0 (gs_core.c vram_rw): in 32-bit words for 32 bits, in
    bytes for 8 bits, in nibbles for 4 bits."""
    y, x = np.mgrid[y0:y0 + h, x0:x0 + w]
    bw = max(bw, 1)
    if psm == 0x13:
        return ((((y >> 1) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + BLOCK32[(y >> 4) & 3, (x >> 4) & 7]) << 8) + COL8[y & 15, x & 15]
    if psm == 0x14:
        return ((((y >> 2) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + BLOCK16[(y >> 4) & 7, (x >> 5) & 3]) << 9) + COL4[y & 15, x & 31]
    return (((y & ~0x1F) * bw + ((x >> 1) & ~0x1F) + BLOCK32[(y >> 3) & 3, (x >> 3) & 7]) << 6) + COL32[y & 7, x & 7]


def through_gs(packet, pixblt, tex0):
    """The texture as the GS reads it after the game's upload: (array, psm) or None."""
    dbw, dpsm = pixblt & 0x3F, (pixblt >> 8) & 0x3F
    tbw, psm = (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F
    tw, th = 1 << ((tex0 >> 26) & 15), 1 << ((tex0 >> 30) & 15)
    q = 16 if struct.unpack_from('<I', packet, 12)[0] >> 24 in (0x50, 0x51) else 0   # a VIF DIRECT in front
    giftag, = struct.unpack_from('<Q', packet, q)
    nloop, nreg = giftag & 0x7FFF, (giftag >> 60) & 15 or 16
    regs = {}
    p = q + 16
    for _ in range(nloop * nreg if (giftag >> 58) & 3 == 0 else 0):
        d, r = struct.unpack_from('<QQ', packet, p)
        regs[r & 0xFF] = d
        p += 16
    img, = struct.unpack_from('<Q', packet, p)
    if (img >> 58) & 3 != 2:
        return None
    data = packet[p + 16:p + 16 + (img & 0x7FFF) * 16]
    pos, reg = regs.get(0x51, 0), regs.get(0x52, 0)
    dx, dy, rw, rh = (pos >> 32) & 0x7FF, (pos >> 48) & 0x7FF, reg & 0xFFF, (reg >> 32) & 0xFFF
    if rw == 0 or rh == 0 or tw > 1024 or th > 1024:
        return None
    vram = np.zeros(1 << 22, np.uint8)
    if dpsm in (0, 1):
        words = np.frombuffer(data[:rw * rh * 4].ljust(rw * rh * 4, b'\0'), '<u4').reshape(rh, rw)
        vram.view('<u4')[addr(0, dbw, rw, rh, dx, dy) & 0xFFFFF] = words
    elif dpsm == 0x13:
        vram[addr(0x13, dbw, rw, rh, dx, dy) & 0x3FFFFF] = np.frombuffer(data[:rw * rh].ljust(rw * rh, b'\0'), np.uint8).reshape(rh, rw)
    else:
        return None
    if psm == 0x13:
        return vram[addr(0x13, tbw, tw, th) & 0x3FFFFF], psm
    if psm == 0x14:
        a = addr(0x14, tbw, tw, th)
        return (vram[(a >> 1) & 0x3FFFFF] >> ((a & 1) * 4)) & 15, psm
    if psm in (0, 1):
        px = vram.view('<u4')[addr(0, tbw, tw, th) & 0xFFFFF]
        if psm == 1:
            px = px | 0x80000000
        return px, psm
    return None

# ------------------------------------------------------------------------------------------------ PSP formats

def quantize(rgba):
    """NFSU2's median cut (tools/convert_car.py) on an (n, 4) array: palette (256, 4) and indices."""
    cols, inv, counts = np.unique(rgba, axis=0, return_inverse=True, return_counts=True)
    if len(cols) <= 256:
        pal = np.zeros((256, 4), np.uint8)
        pal[:len(cols)] = cols
        return pal, inv.astype(np.uint8).ravel()
    boxes = [np.arange(len(cols))]
    while len(boxes) < 256:
        spans = [(np.ptp(cols[b], 0).max() * counts[b].sum()) if len(b) > 1 else -1 for b in boxes]
        i = int(np.argmax(spans))
        if spans[i] <= 0:
            break
        b = boxes[i]
        k = int(np.argmax(np.ptp(cols[b], 0)))
        b = b[np.argsort(cols[b, k], kind='stable')]
        acc = np.cumsum(counts[b])
        cut = int(np.clip(np.searchsorted(acc, acc[-1] / 2), 1, len(b) - 1))
        boxes[i:i + 1] = [b[:cut], b[cut:]]
    pal = np.zeros((256, 4), np.uint8)
    which = np.zeros(len(cols), np.uint8)
    for i, b in enumerate(boxes):
        pal[i] = np.round((cols[b].astype(np.float64) * counts[b, None]).sum(0) / counts[b].sum())
        which[b] = i
    return pal, which[inv.ravel()]


def swizzle(lin, row_bytes, h):
    """The GE's swizzled order: 16-byte x 8-row blocks (NFSU2 psp_swizzle)."""
    a = np.frombuffer(lin, np.uint8).reshape(h // 8, 8, row_bytes // 16, 16)
    return a.transpose(0, 2, 1, 3).tobytes()


def to_psp(img, psm, clut8, limit=64):
    """(format, w, h, bytes) at PSP resolution (halved until both sides <= limit), padded and swizzled."""
    shift = 0
    while max(img.shape) > limit:   # NFSU2 to_clut8: indices by nearest, colours by a 2x2 box
        shift += 1
        if psm in (0x13, 0x14) or min(img.shape) < 2:
            img = img[::2, ::2]
        else:
            q = img[:img.shape[0] & ~1, :img.shape[1] & ~1].astype(np.uint64)
            out = np.zeros((q.shape[0] // 2, q.shape[1] // 2), np.uint64)
            for sh in (0, 8, 16, 24):
                ch = sum((q[dy::2, dx::2] >> sh) & 255 for dy in (0, 1) for dx in (0, 1))
                out |= ((ch + 2) >> 2) << sh
            img = out.astype(np.uint32)
    th, tw = img.shape
    if psm == 0x14:
        fmt, minw = 2, 32
    elif psm == 0x13:
        fmt, minw = 1, 16
    else:
        fmt, minw = (3, 16) if clut8 else (0, 4)
    w, h = max(tw, minw), (th + 7) & ~7
    if (w, h) != (tw, th):
        img = np.pad(img, ((0, h - th), (0, w - tw)), mode='edge')
    pal = b''
    if fmt == 2:
        lin = (img[:, 0::2] | (img[:, 1::2] << 4)).astype(np.uint8).tobytes()
        row = w // 2
    elif fmt == 1:
        lin, row = img.astype(np.uint8).tobytes(), w
    else:
        c = img.astype('<u4')
        a = np.minimum(((c >> 24) & 0xFF) * 2, 255).astype('<u4')   # the PS2's alpha 0x80 is 1.0
        c = (c & 0x00FFFFFF) | (a << 24)
        if fmt == 3:
            p, idx = quantize(c.view(np.uint8).reshape(-1, 4))
            pal, lin, row = p.tobytes(), idx.tobytes(), w
        else:
            lin, row = c.tobytes(), w * 4
    return fmt, w, h, pal + swizzle(lin, row, h), shift


def fnv64(b):
    h = 0xCBF29CE484222325
    for x in np.frombuffer(b, np.uint8).tolist():
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

# ------------------------------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('disc', help='folder with DATA/pzs3us1.afs (and pzs3us2.afs)')
    ap.add_argument('-o', '--out', default='bt3psp.tex')
    ap.add_argument('--clut8', action='store_true', help='median-cut 32-bit textures to 256 colours (smaller, slower to build)')
    ap.add_argument('--size', type=int, default=64, help='largest texture side after halving (default 64: small and fast on a PSP-1000)')
    ap.add_argument('--max', type=int, default=0, help='stop after N AFS entries (a quick look)')
    a = ap.parse_args()
    disc = Path(a.disc)
    afs = [p for n in ('pzs3us1.afs', 'pzs3us2.afs') for p in (disc / 'DATA' / n, disc / 'data' / n, disc / n) if p.exists()]
    if not afs:
        raise SystemExit(f'no pzs3us1.afs under {disc}')
    seen, entries, blobs, at, t0 = set(), [], [], 0, time.time()
    stats = {'sheets': 0, 'textures': 0, 'skipped': 0}
    for path in dict.fromkeys(afs):
        toc = afs_entries(path)
        with open(path, 'rb') as f:
            for i, (off, size) in enumerate(toc):
                if a.max and i >= a.max:
                    break
                if size < 64:
                    continue
                f.seek(off)
                head = f.read(min(size, 16384))
                secs = pack_sections(head, size)
                if not secs or not all(bpe_ok(head, o, size) if o + 8 <= len(head) else True for o in secs[:1]):
                    continue
                f.seek(off)
                buf = f.read(size)
                if not all(bpe_ok(buf, o, size) for o in secs):
                    continue
                for o in secs:
                    try:
                        sheet = bpe(buf, o)
                    except (ValueError, IndexError):
                        continue
                    stats['sheets'] += 1
                    for base, ents in tex_files(sheet):
                        for pix, ps, pixblt, tex0 in ents:
                            if not pix:
                                continue
                            packet = sheet[base + pix * 4:base + pix * 4 + ps]
                            key = fnv64(packet)
                            if key in seen:
                                continue
                            seen.add(key)
                            try:
                                r = through_gs(packet, pixblt, tex0)
                            except (struct.error, ValueError, IndexError):
                                r = None
                            if r is None:
                                stats['skipped'] += 1
                                continue
                            img, psm = r
                            fmt, w, h, data, shift = to_psp(img, psm, a.clut8, a.size)
                            tw, th = img.shape[1], img.shape[0]
                            entries.append((key, at, len(data), w, h, tw, th, fmt, 1, shift))
                            blobs.append(data + bytes(-len(data) % 64))
                            at += len(data) + (-len(data) % 64)
                            stats['textures'] += 1
                print(f'\r{path.name} {i + 1}/{len(toc)}: {stats["sheets"]} sheets, {stats["textures"]} textures, '
                      f'{at >> 20} MB, {time.time() - t0:.0f} s', end='', flush=True)
    print()
    entries.sort()
    index_off = 32
    data_off = (index_off + 32 * len(entries) + 63) & ~63
    with open(a.out, 'wb') as f:
        f.write(struct.pack('<4sIIII12x', b'BT3T', 2, len(entries), index_off, data_off))
        for key, off, n, w, h, tw, th, fmt, swz, shift in entries:
            f.write(struct.pack('<QIIHHHHBBBBI', key, off, n, w, h, tw, th, fmt, swz, shift, 0, 0))
        f.write(bytes(data_off - f.tell()))
        for b in blobs:
            f.write(b)
    print(f'{a.out}: {len(entries)} textures ({stats["skipped"]} in formats left to the runtime), '
          f'{(data_off + at) >> 20} MB')


if __name__ == '__main__':
    main()
