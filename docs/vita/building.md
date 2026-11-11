# Building the Vita version

The build runs on Linux (or WSL on Windows) and needs no game files: the game's built-in data tables are in the
repository as their shape only (`port/data`) and are filled in from the player's disc when the program starts.

## Requirements

- [VitaSDK](https://vitasdk.org/), with its standard packages (`vdpm`: zlib, math-neon, vitaShaRK, SceShaccCgExt,
  taihen). `VITASDK` must point to it and `$VITASDK/bin` be on the `PATH`.
- vitaGL with this port's changes, built from `port/vita/vitagl` (below). It replaces the vitaGL in `$VITASDK`.
- A checkout of [vitasdk/samples](https://github.com/vitasdk/samples) (the build uses its `common/debugScreen.c`).
- Python 3, git, a host `gcc` and `make`.
- libclang and its Python bindings. On Ubuntu / Debian: `sudo apt install libclang-dev python3-clang`, or
  `pip install libclang`.

## Steps

```sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
git clone https://github.com/vitasdk/samples ~/vitasdk-samples

# once: vitaGL at the commit the port was made with, plus port/vita/vitagl/vitagl.patch, installed into $VITASDK
port/vita/vitagl/build_vitagl.sh

VITA_SAMPLES=~/vitasdk-samples python3 port/vita/build.py
```

The result is `port/build/vita_rel/Tenkaichi3Decomp-Vita.vpk` (about 2.5 MB). A full build takes a few minutes;
`BT3_INCREMENTAL=1` reuses the objects of the previous one where the source is older (headers are not tracked).

Other environment variables:

| Variable | Meaning |
|---|---|
| `VITA_SAMPLES` | The vitasdk/samples checkout (default: next to `$VITASDK`) |
| `BT3_LIBCLANG` | libclang's shared library, when it is not found in the usual places |
| `BT3_CLANG_PY` | The folder that holds `clang/cindex.py`, when the bindings are not installed |
| `BT3_VITA_DEV=1` | The development build, see [development.md](development.md) |

`python3 port/vita/build.py compile`, `link` or `vpk` runs one step only.

## What the build does

The game's code is compiled as in the PC port's 32-bit variant: the same sources, the PS2 compiler's float
constants (`port/tools/eeconst.py`), and call arguments evaluated right to left as the PS2 compiler does
(`port/tools/ptr32.py`, through libclang). On top of that, `port/vita/srcfix.py` writes out the PS2's rule for
shift counts, since ARM and MIPS differ there. The ARM EABI's differences from the PS2's ABI are compiled away with
`-fsigned-char -fno-short-enums -fsigned-bitfields`.

The port's own code and the renderer front end (`port/src/gs`) are compiled as ordinary Vita code. The few names
that exist both in the game's libraries and in the SDK's are renamed in the objects (`objcopy --redefine-syms`).
Functions of the PS2's libraries that nothing provides get a generated stub that stops the program with the
function's name; none of them is reached in the game.

The build generates, under `port/build/gen/vita_rel/`, the table of where each of the game's data tables gets its
values on the disc (`vita_dat.c`) and the stubs (`stubs.c`). `port/build/vita_rel/bt3.unstripped.elf` and
`bt3.map` are kept for reading crash dumps.

## vitaGL

`port/vita/vitagl/build_vitagl.sh [work folder]` clones [vitaGL](https://github.com/Rinnegatamante/vitaGL), checks
out commit `c9bcd74`, applies `vitagl.patch` and builds it with

```
make NO_DEBUG=1 HAVE_VERTEX_LAYOUT_CACHE=1 USE_SCRATCH_MEMORY=1
make install ...
```

The patch keeps the fragment programs made for each blend setting with their shader program, caches vertex
layouts by content, and adds the entry points the renderer uses: many draws with one program and state, each with
its own uniforms and indices (`vglFastPrepare` / `vglFastDraw`), a fight's picture handed to the GPU in parts
(`vglSplitScene`), the depth buffer kept across a framebuffer switch (`vglBt3ContNext`), and the display size
changed between the menus and the fights (`vglSwapResolution` for each queued buffer, with the full-screen viewport
that clears and scissor masks are drawn with).
