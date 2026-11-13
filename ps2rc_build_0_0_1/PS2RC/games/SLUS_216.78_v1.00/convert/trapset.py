"""BT3 (SLUS-216.78 v1.00) conversion step: the trap set - the translated functions whose memory accesses must reach
the hardware model (hle/core/hle_mmio.c) instead of being masked into guest RAM (prepare_gen.py --trapset).

Same method as the GTA pack's convert/trapset.py:
  1. static: functions that build a hardware address with `lui` (upper halves 0x1000/0x1001, 0x1100/0x1101,
     0x1200/0x1201 and their KSEG1 views 0xB000..; the +1 forms come from negative 16-bit offsets) and use it as the
     base of a load / store - in the main program and in the kernel patch overlay (ov1);
  2. Sony libraries that drive hardware through pointers kept in data (DMA, graphics, IPU, VIF/VU) - here by name
     from the Tenkaichi3Decomp symbol files of this boot program (instead of a signature database);
  3. optional: function lists of host profiling runs (--profile FILE).

    python convert/trapset.py [--decomp-config DIR] [--profile FILE ...] [--out FILE]

Default output: the pack's [build] trapset (work dir: derived from the player's disc)."""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK_DIR = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(PACK_DIR, "..", ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "ps2rc", "pipeline"))
from ps2rc import config, pack as _pack  # noqa: E402
from ps2rc.image import load_elf  # noqa: E402
from ps2rc.analysis.functions import discover_functions  # noqa: E402
from translate import overlay_unit  # noqa: E402

PACK_NAME = os.path.basename(PACK_DIR)
PACK = _pack.load(PACK_NAME)
DECOMP_CONFIG = os.environ.get("BT3_DECOMP_CONFIG", "")   # the decomp project's config folder (symbol names), optional

HW_LUI = set()
for base in (0x1000, 0x1100, 0x1200, 0xB000, 0xB100, 0xB200):
    HW_LUI |= {base, base + 1}
SCE_PREFIXES = ("sceDma", "sceGs", "sceIpu", "WaitDma", "WatchDma", "defStopDMA", "_waitIpuIdle", "_sceMpegWaitIpu",
                "_sceMpegStopIpuDma", "sceVif", "sceVu0", "sceVu1", "sceGif")
MEM_OPS = {0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D,
           0x2E, 0x31, 0x36, 0x37, 0x39, 0x3E, 0x3F}
ADD_IMM = {0x09, 0x0D, 0x19}
ADD_REG = {0x21, 0x25, 0x2D}


def scan(img, fns, prefix, why):
    """A function is marked when a register loaded with a hardware upper half (lui) - or derived from one by
    addiu/ori/daddiu/addu/or/daddu - is the base of a load or store (linear order, no kill analysis: conservative)."""
    for a, fn in fns.items():
        hw, lui_seen = set(), set()
        for pc in sorted(fn.addrs):
            try:
                w = img.word(pc)
            except Exception:
                continue
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if op == 0x0F and (w & 0xFFFF) in HW_LUI and rt:
                hw.add(rt)
                lui_seen.add(w & 0xFFFF)
            elif op in ADD_IMM and rs in hw and rt:
                hw.add(rt)
            elif op == 0 and (w & 63) in ADD_REG and (rs in hw or rt in hw) and rd:
                hw.add(rd)
            elif op in MEM_OPS and rs in hw:
                why.setdefault("%sfn_%08x" % (prefix, a), set()).add(
                    "hw base (lui %s)" % "/".join("%04x" % u for u in sorted(lui_seen)))


def decomp_library_functions(cfg):
    """{address: name} of the decomp's function symbols whose names are Sony hardware library functions (without a
    decomp folder: the list kept in this pack, sce_hw_funcs.txt)."""
    out = {}
    if not cfg:
        for line in open(os.path.join(HERE, "sce_hw_funcs.txt"), encoding="ascii"):
            parts = line.split()
            if len(parts) == 2 and not line.startswith("#"):
                out[int(parts[0], 16)] = parts[1]
        return out
    files = [os.path.join(cfg, "symbol_addrs.txt")] + glob.glob(os.path.join(cfg, "symbols", "*.txt"))
    for f in files:
        if not os.path.exists(f):
            continue
        for line in open(f, encoding="utf-8", errors="replace"):
            m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)\s*;.*type:func", line)
            if m and m.group(1).startswith(SCE_PREFIXES):
                out[int(m.group(2), 16)] = m.group(1)
    return out


def main():
    args = sys.argv[1:]
    out = PACK.path("build.trapset")
    cfg = DECOMP_CONFIG
    profiles = []
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out, i = args[i + 1], i + 2
        elif args[i] == "--profile":
            profiles.append(args[i + 1])
            i += 2
        elif args[i] == "--decomp-config":
            cfg, i = args[i + 1], i + 2
        else:
            i += 1
    disc = config.disc_dir(PACK_NAME, required=True)
    img = load_elf(os.path.join(disc, PACK.get("id.boot_elf")))
    code = [tuple(PACK.get("memory.code"))]
    fns = discover_functions(img, code, extra_seeds=[img.entry]).functions
    why = {}
    scan(img, fns, "", why)
    for name in PACK.get("overlays.files", []):
        ov, oimg, ofs, _ctors = overlay_unit(img, os.path.join(disc, name),
                                             bool(PACK.get("overlays.split_zero_runs", False)))
        scan(oimg, ofs.functions, "ov%d_" % ov.id, why)
    n_static = len(why)
    for a, n in sorted(decomp_library_functions(cfg).items()):
        if a in fns:
            why.setdefault("fn_%08x" % a, set()).add("sce " + n)
    n_sce = len(why) - n_static
    n_dyn = 0
    for p in profiles:
        for ln in open(p, encoding="utf-8", errors="replace"):
            parts = ln.split()
            if not parts or "fn_" not in parts[0]:
                continue
            name = parts[0].replace("__guest", "").replace("__stub", "")
            if name not in why:
                n_dyn += 1
            why.setdefault(name, set()).add("seen " + os.path.basename(p))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("# trap set for the PSP build (convert/trapset.py): %d functions - static lui %d, Sony hardware "
                "libraries +%d, host profiles +%d\n" % (len(why), n_static, n_sce, n_dyn))
        for n in sorted(why):
            f.write("%s  # %s\n" % (n, ", ".join(sorted(why[n]))))
    print("trap set: %d functions (static %d, sce +%d, profiles +%d) -> %s" % (len(why), n_static, n_sce, n_dyn, out))


if __name__ == "__main__":
    main()
