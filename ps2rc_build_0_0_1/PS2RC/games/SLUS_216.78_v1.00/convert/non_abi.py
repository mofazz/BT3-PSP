"""Which functions the ABI-mode translation (build_psp --fast) must keep exact (ps2rc.analysis.abi), for BT3.

    PS2RC_DISC=<staged disc> python games/SLUS_216.78_v1.00/convert/non_abi.py [out.json]

Same function sets as ps2rc/pipeline/translate.py (main program discovered from the entry point, the [overlays] files
discovered by overlay_unit). Writes {"counts", "functions" (reasons), "exact_starts": {unit: [hex]}}; the default
output is the pack's lists/non_abi.json (fast.toml: [translate] non_abi). Without it the ABI-mode build kept only
Heap_Create exact, and the battle camera went wild in PPSSPP (a function that breaks the calling convention)."""
import collections
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "ps2rc", "pipeline"))

from ps2rc import pack as _pack  # noqa: E402
from ps2rc.image import load_elf  # noqa: E402
from ps2rc.analysis.functions import discover_functions  # noqa: E402
from ps2rc.analysis.abi import Unit, analyse  # noqa: E402
from translate import overlay_unit  # noqa: E402


def main():
    t0 = time.time()
    p = _pack.load(os.environ.get("PS2RC_PACK", "SLUS_216.78_v1.00"))
    disc = p.disc()
    img = load_elf(os.path.join(disc, p.get("id.boot_elf"))).stripped()
    code = [tuple(p.get("memory.code"))] + ([tuple(p.get("memory.ctor_code"))] if p.get("memory.ctor_code") else [])
    funcs = discover_functions(img, code, extra_seeds=[img.entry]).functions
    units = [Unit("main", img, funcs)]
    for name in p.get("overlays.files", []):
        _ov, oimg, ofs, _c = overlay_unit(img, os.path.join(disc, name), bool(p.get("overlays.split_zero_runs", False)))
        units.append(Unit(name, oimg, ofs.functions))
    print("units: %s" % ", ".join("%s %d" % (u.name, len(u.functions)) for u in units), flush=True)
    res = analyse(units, window=tuple(p.get("overlays.window", (0, 0))))
    exact = {k: v for k, v in res.items() if v.exact}
    by_reason = collections.Counter(r for v in exact.values() for r in v.reasons)
    heap_create = ("main", 0x00254F80)                      # textpatch.tsv patches it in the exact form too
    out = {"counts": {"functions": len(res), "exact_functions": len(exact), "per_reason": dict(by_reason)},
           "functions": {"%s:%08x" % k: {"reasons": sorted(v.reasons), "insns": v.insns}
                         for k, v in sorted(exact.items())},
           "exact_starts": {u.name: sorted({"%08x" % s for (n, s) in exact if n == u.name}
                                           | ({"%08x" % heap_create[1]} if u.name == "main" else set()))
                            for u in units}}
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "lists", "non_abi.json")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print("functions %d: must be exact %d %s -> %s (%.0f s)" % (len(res), len(exact), dict(by_reason), path,
                                                                time.time() - t0))


if __name__ == "__main__":
    main()
