#!/usr/bin/env python3
"""PSP build, stage 0b: the game's logic for the PSP-3000, no picture, timed (see README.md).

Re-uses the Vita build's source preparation (port/vita/build.py: the game's sources made ready as the PS2 compiler
read them, the data tables, the maths library) with psp-gcc in place of the ARM compiler, the PSP's own start-up,
memory and file files (port/psp/psp_*.c) in place of the Vita's, and the PSPSDK's build rules for the EBOOT.

    python3 port/psp/build.py              compile, link, EBOOT
    python3 port/psp/build.py compile      only compile
    python3 port/psp/build.py link         only link (the stubs and the EBOOT)

Needs: pspdev (psp-gcc, psp-config, the PSPSDK; PSPDEV is read from the environment or from psp-config), gcc and
make on the host, python3 with libclang (pip install libclang), as the Vita build does.
Environment: PSP_MS_ROOT=<memory stick folder> copies EBOOT.PBP to <it>/PSP/GAME/BT3PSP/; BT3_PSP_OPT=-Os for a smaller program.
"""
import concurrent.futures, importlib.util, multiprocessing, os, pathlib, re, shutil, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "port/tools"))
spec = importlib.util.spec_from_file_location("vita_build", ROOT / "port/vita/build.py")
vb = importlib.util.module_from_spec(spec)
sys.modules["vita_build"] = vb
spec.loader.exec_module(vb)

def pspdev():
    p = os.environ.get("PSPDEV")
    if not p:
        try:
            p = subprocess.run(["psp-config", "--pspdev-path"], capture_output=True, text=True, check=True).stdout.strip()
        except Exception:
            raise SystemExit("pspdev not found: set PSPDEV (e.g. export PSPDEV=$HOME/pspdev) and put $PSPDEV/bin on PATH")
    return p

PSPDEV = pspdev()
PSP_INCLUDES = [f"-I{pathlib.Path(PSPDEV) / 'psp/include'}", f"-I{pathlib.Path(PSPDEV) / 'psp/sdk/include'}"]
vb.P = str(pathlib.Path(PSPDEV) / "bin/psp-")
vb.OBJ = ROOT / "port/build/obj_psp"
vb.OUT = ROOT / "port/build/psp"
vb.GEN = ROOT / "port/build/gen/psp"
OPT = os.environ.get("BT3_PSP_OPT", "-O2")
TRACE = os.environ.get("BT3_PSP_TRACE", "0") == "1"   # call recorder for crash hunting (psp_main.c: crumbs.txt); BT3_PSP_TRACE=1 switches it on (it slows the game a lot)

# PORT_VITA is the shared code's name for "32-bit target without the PS2's address space" (hardware registers
# in ordinary memory, the game's data tables read from the disc); the PSP takes the same paths.
vb.COMMON = [OPT, "-g", "-fno-strict-aliasing", "-ffp-contract=off", "-w", "-G0",
             # the PS2's divide never traps, the MIPS32 compiler's default is a trap on a zero divisor (a "break" the PSP cannot survive)
             "-mno-check-zero-division",
             # GCC 14+ makes these errors; the decompiled sources and the GS front end were written for older compilers
             "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion", "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
             "-Wno-error=return-mismatch", "-Wno-error=pointer-sign", "-Wno-error=discarded-qualifiers", "-DPORT_VITA=1", "-DPORT_VITA_FIXED=1", "-DPORT_PSP=1",
             '-DPORT_VITA_SFX=".txt"', "-Dgetenv=Psp_Getenv"]
if TRACE:
    vb.COMMON.append("-finstrument-functions")
vb.GAME = [vb.P + "gcc", "-std=gnu89", "-c", "-fcommon"] + vb.COMMON + vb.ABI + vb.NOBUILTIN + \
          ["-include", "port_libm.h", "-Iinclude", "-Iport/include", "-Iport/vita", "-Iport/psp", "-include", "port_compat.h"]
vb.R2L_ARGS = ["-x", "cpp-output", "--target=mipsel-unknown-linux-gnu", "-msoft-float", "-std=gnu89", "-fsigned-char", "-fno-short-enums", "-w",
               "-Wno-error=return-type", "-Wno-error=return-mismatch"]

ORIG_CC_PORT = vb.cc_port


def port_sources():
    S, PS = ROOT / "port/src", ROOT / "port/psp"
    out = [(ROOT / "src/port/vu0_a.c", "vu0"), (ROOT / "src/port/vu0_b.c", "vu0")]
    for n in ("gs_marker", "headless", "mathf_pc", "plat_gs", "plat_mc", "plat_stub", "plat_sys", "softfloat_ps2"):
        out.append((S / (n + ".c"), "ee"))
    for n in ("psp_main", "psp_mem", "psp_file", "psp_float", "psp_glue"):
        out.append((PS / (n + ".c"), "hard"))
    out.append((vb.GEN / "vita_dat.c", "hard"))
    out.append((PS / "psp_gs.c", "hard"))
    out.append((PS / "psp_gu.c", "gs"))         # the sceGu back end (it includes gs_internal.h: the "gs" kind has that on its path)
    out.append((PS / "psp_state.c", "hard"))
    out.append((PS / "exc/exc_user.c", "hard"))      # the user side of the crash catcher (PS2RC's psp_exc, see exc/LICENSE)
    out.append((PS / "exc/ps2rc_kcall.S", "asm"))
    out.append((S / "gs/gs_sony.c", "hard"))
    # the software GS front end, from a copy whose unused 4 MB depth buffer per target is left out
    src = (S / "gs/gs_core.c").read_text()
    assert "calloc(SURF_W * SURF_H, 4)" in src
    # the record ring (8 MB) is for the threaded GPU back end (gGsSplit); the list snapshots (3 x 1 MB) fall back to reading in place when full
    for old, new in (("calloc(SURF_W * SURF_H, 4)", "NULL"), ("#define GR_SIZE (8u << 20)", "#define GR_SIZE (64u << 10)"),
                     # no third list buffer: Dma_Flush (src/sys/dma.c) swaps in Port_DmaSpare's buffer when this is 1, and without the Vita's
                     # renderer thread there is no spare buffer (it returned 0, so the game's list pointer became 0 after the first flush)
                     ("int gVitaTriple = 1;", "int gVitaTriple = 0;"),
                     ("#define SNAP_WORDS ((1024u << 10) / 4)", "#define SNAP_WORDS ((256u << 10) / 4)")):
        assert old in src, old
        src = src.replace(old, new)
    # (every use of Port_GsVif1Chain inside gs_core.c goes to the _real one; the wrapper at the end is the exported function and keeps a time)
    src = "#define Port_GsVif1Chain Port_GsVif1Chain_real\n" + src + """
#undef Port_GsVif1Chain
extern unsigned long long gPspGsUs;
extern unsigned gPspGsCalls;
void Port_GsVif1Chain(uint32_t tadr, int tte) {
    unsigned long long t = sceKernelGetProcessTimeWide();
    Port_GsVif1Chain_real(tadr, tte);
    gPspGsUs += sceKernelGetProcessTimeWide() - t;
    gPspGsCalls++;
}
"""
    (vb.GEN / "psp_gs_core.c").write_text(src)
    out.append((vb.GEN / "psp_gs_core.c", "gs"))
    out.append((S / "gs/gs_vu1.c", "gs"))
    return out


def cc_port(item):
    f, kind = item
    if kind == "asm":
        o = vb.OBJ / ("pc_" + f.stem + "_s.o")
        sdk = subprocess.run(["psp-config", "--pspsdk-path"], capture_output=True, text=True).stdout.strip()
        cmd = [vb.P + "gcc", "-c", "-x", "assembler-with-cpp", "-G0", f"-I{sdk}/include", f"-I{sdk}/lib", f"-I{f.parent}", str(f), "-o", str(o)]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        return (o if r.returncode == 0 else None), f, r.stderr
    if kind == "gs":
        o = vb.OBJ / ("gs_" + f.stem + ".o")
        cmd = [vb.P + "gcc", "-std=gnu99", "-c"] + vb.COMMON + PSP_INCLUDES + ["-DVITA_RARE(x)=(!!(x))", "-Dgetenv=Vita_GsGetenv", f"-I{HERE / 'shim'}", "-Iinclude", "-Iport/include",
                                                              "-Iport/src", "-Iport/vita", f"-I{ROOT / 'port/src/gs'}", "-Iport/psp", str(f), "-o", str(o)]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        return (o if r.returncode == 0 else None), f, r.stderr
    if kind != "hard":
        return ORIG_CC_PORT(item)
    o = vb.OBJ / ("pc_" + f.stem + ".o")
    cmd = [vb.P + "gcc", "-std=gnu99", "-c"] + vb.COMMON + PSP_INCLUDES + ["-DVITA_RARE(x)=(!!(x))", "-Iinclude", "-Iport/include", "-Iport/src", "-Iport/vita",
                                                          "-Iport/psp", "-fno-instrument-functions", f"-I{vb.GEN}", f"-I{HERE / 'shim'}", f"-I{ROOT / 'port/src/gs'}", str(f), "-o", str(o)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return (o if r.returncode == 0 else None), f, r.stderr


def compile_all():
    vb.OBJ.mkdir(parents=True, exist_ok=True)
    vb.GEN.mkdir(parents=True, exist_ok=True)
    vb.float_expected()
    data = vb.data_tables()
    ctx = multiprocessing.get_context("fork")
    with concurrent.futures.ProcessPoolExecutor(os.cpu_count() or 4, mp_context=ctx) as ex:
        res = list(ex.map(vb.cc_game, vb.portsrc.sources()))
        res += list(ex.map(vb.cc_libm, sorted((ROOT / "port/third_party/newlib_libm").glob("*.c"))))
        res += list(ex.map(cc_port, port_sources()))
    bad = [(f, e) for o, f, e in res if o is None]
    for f, e in bad:
        print("FAILED", f.name, vb.first_error(e))
    objs = [str(o) for o, _, _ in res if o] + data
    (vb.OBJ / "objects.txt").write_text("\n".join(objs) + "\n")
    print(f"{len(objs)} objects, {len(bad)} failed")
    if bad:
        (vb.OBJ / "errors.txt").write_text("\n\n".join(f"== {f}\n{e}" for f, e in bad))
        print(f"full compiler output of the failures: {vb.OBJ / 'errors.txt'}")
    return not bad


WRAPS = "Progress_Main main exit Movie_PlayOpening Movie_PlayEnding".split()   # the two movies: no video decoder on the PSP yet (psp_glue.c skips them)

# XMB picture and background: ICON0.PNG must be 144 x 80 and PIC1.PNG 480 x 272 (they were missing: the EBOOT had no icon)
ICON_LINES = "\n".join(f"{k} = {HERE / f}" for k, f in (("PSP_EBOOT_ICON", "ICON0.PNG"),) if (HERE / f).exists())

MAKEFILE = """# generated by port/psp/build.py
TARGET = BT3PSP
OBJS = $(shell cat {objs})
PSP_LARGE_MEMORY = 1
LIBS = -lpspgu -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpsppower -lpsprtc -lpspuser -lm
LDFLAGS += {wraps} -Wl,-Map={map}
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = BT3 PSP
{icon}
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
"""


def run_make():
    out = vb.OUT
    out.mkdir(parents=True, exist_ok=True)
    (out / "Makefile").write_text(MAKEFILE.format(objs=vb.OBJ / "objects.txt", wraps=" ".join(f"-Wl,--wrap={n}" for n in WRAPS), map=out / "bt3.map", icon=ICON_LINES))
    return subprocess.run(["make", "-C", str(out)], capture_output=True, text=True)


def link():
    objs_txt = vb.OBJ / "objects.txt"
    for attempt in range(4):
        r = run_make()
        need = sorted(set(re.findall(r"undefined reference to `(\w+)'", r.stderr)))
        if r.returncode == 0 or not need:
            break
        # symbols nothing provides: data counters (gXxx) get zeroed storage, everything else a stub that stops with its name
        data_syms = [s for s in need if re.match(r"g[A-Z]", s) or re.match(r"D_[0-9A-Fa-f]+$", s)]
        lines = ["/* Generated by port/psp/build.py: one stub per symbol nothing provides. */", "extern void Port_Missing(const char *name);"]
        lines += [f"unsigned long long {s}[64];" for s in data_syms]
        lines += [f'int {s}(void) {{ Port_Missing("{s}"); return 0; }}' for s in need if s not in data_syms]
        (vb.GEN / "stubs.c").write_text("\n".join(lines) + "\n")
        so = vb.OBJ / f"pc_stubs{attempt}.o"
        subprocess.run([vb.P + "gcc", "-c", "-O1", "-G0", str(vb.GEN / "stubs.c"), "-o", str(so)], check=True)
        t = objs_txt.read_text().split()
        objs_txt.write_text("\n".join(t + [str(so)]) + "\n")
        print(f"link: {len(need)} symbols without a definition, stubbed ({len(data_syms)} of them data)")
        (vb.OUT / f"undefined{attempt}.txt").write_text("\n".join(need) + "\n")
    errs = [l for l in r.stderr.splitlines() if "multiple definition" in l or "undefined reference" in l or "rror" in l]
    print("link:", "OK" if r.returncode == 0 else "FAILED")
    for l in errs[:20]:
        print("  " + l[-220:])
    if r.returncode != 0:
        (vb.OUT / "make-output.txt").write_text(r.stdout + "\n" + r.stderr)
        print(f"full output: {vb.OUT / 'make-output.txt'}")
        return False
    pbp = vb.OUT / "EBOOT.PBP"
    if not pbp.exists():
        print("no EBOOT.PBP was made; see", vb.OUT)
        return False
    elf = vb.OUT / "BT3PSP.elf"
    print(f"{pbp}: {pbp.stat().st_size // 1024} KB" + (f"  (ELF {elf.stat().st_size // 1024} KB with debug info)" if elf.exists() else ""))
    ms = os.environ.get("PSP_MS_ROOT")
    if ms:
        d = pathlib.Path(ms) / "PSP/GAME/BT3PSP"
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy(pbp, d / "EBOOT.PBP")
        print("copied to", d)
    return True


def build_exc():
    """ps2rc_exc.prx, the kernel half of the crash catcher (port/psp/exc/, PS2RC's psp_exc): copied out and built with its own Makefile."""
    src = HERE / "exc"
    work = ROOT / "port/build/exc"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    for n in ("Makefile", "exc_main.c", "exc_me.c", "exc_asm.S", "ps2rc_exc.h", "ps2rc_sctrl.S", "ps2rc_sctrl.h", "exports.exp"):
        shutil.copy(src / n, work / n)
    r = subprocess.run(["make", "-C", str(work)], capture_output=True, text=True)
    prx = work / "ps2rc_exc.prx"
    if r.returncode != 0 or not prx.exists():
        (work / "make-output.txt").write_text(r.stdout + "\n" + r.stderr)
        print("ps2rc_exc.prx FAILED, see", work / "make-output.txt")
        return False
    if vb.OUT.exists():
        shutil.copy(prx, vb.OUT / "ps2rc_exc.prx")
    print("ps2rc_exc.prx:", prx.stat().st_size, "bytes")
    ms = os.environ.get("PSP_MS_ROOT")
    if ms:
        d = pathlib.Path(ms) / "PSP/GAME/BT3PSP"
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy(prx, d / "ps2rc_exc.prx")
        print("copied to", d)
    return True


def main():
    steps = [a for a in sys.argv[1:] if not a.startswith("-")] or ["compile", "link", "exc"]
    if "compile" in steps and not compile_all():
        sys.exit(1)
    if "link" in steps and not link():
        sys.exit(1)
    if "exc" in steps and not build_exc():
        sys.exit(1)


if __name__ == "__main__":
    main()
