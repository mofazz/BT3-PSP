# BT3-PSP

A work-in-progress native PSP port of **Dragon Ball Z: Budokai Tenkaichi 3** (PlayStation 2, USA release,
SLUS-21678). The game's own decompiled code is compiled for the PSP. It is not an emulator.

This repository contains **no game data**. The game's files are copied from your own disc of the USA release to
the memory stick, see [Game files](#game-files).

This is an unofficial port. It is not affiliated with the projects it is based on (see [Credits](#credits));
please report problems with the PSP version here, not upstream.

## Status

The port is in an early stage and is not playable at full speed yet. The target is the PSP-3000 with a custom
firmware (ARK) and extra memory enabled.

- The game's logic runs on the PSP, and the menus are drawn.
- Two renderers exist: a software one (correct but very slow) and a GPU one through the PSP's GE (`gu.txt`), which
  draws only the frame's own buffer for now (no glow, blur or shadow buffers).
- Sound and the opening and ending films are not implemented.

The stages, modes, log files and debugging tools are described in [port/psp/README.md](port/psp/README.md).

## Building

The build needs the [PSPDEV](https://pspdev.github.io/) toolchain, Python 3 and libclang. No game files are needed
to build.

```
pip install libclang
export PSPDEV=$HOME/pspdev; export PATH=$PATH:$PSPDEV/bin
python3 port/psp/build.py
```

This produces `port/build/psp/EBOOT.PBP` and `port/build/psp/ps2rc_exc.prx` (the crash catcher, which goes next to
the EBOOT). With `PSP_MS_ROOT=/path/to/memory/stick` in front, both are copied to `PSP/GAME/BT3PSP/` there.

## Game files

You need a disc image (`.iso`) of your own copy of the USA release, SLUS-21678. Other releases (PAL, Japanese) are
not supported. From the image, copy `SLUS_216.78` and the `BIN` and `DATA` folders into a `disc` folder next to the
EBOOT, so that the memory stick has:

```
ms0:/PSP/GAME/BT3PSP/EBOOT.PBP
ms0:/PSP/GAME/BT3PSP/ps2rc_exc.prx
ms0:/PSP/GAME/BT3PSP/disc/SLUS_216.78
ms0:/PSP/GAME/BT3PSP/disc/BIN/DBZP.BIN
ms0:/PSP/GAME/BT3PSP/disc/DATA/PZS3US0.AFS        (and the other files of the DATA folder)
```

Then start **BT3 PSP** from the XMB. In PPSSPP, use the same layout inside its `memstick` folder.

## Credits

- [arielxflores](https://github.com/arielxflores) for
  [Tenkaichi3Decomp-Vita](https://github.com/arielxflores/Tenkaichi3Decomp-Vita), the PS Vita port whose code and
  memory model this PSP port is built on (`port/vita/`)
- [z3xox](https://github.com/z3xox) and the contributors of
  [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp), the PC port, and the authors of the
  BT3-Decompiled decompilation it uses
- [Matteo Santoro](https://github.com/Matt-code-d) and [limeking614-dev](https://github.com/limeking614-dev) for
  fixes carried over from their forks
- [SandstoneDev](https://github.com/SandstoneDev/PS2RC) for PS2RC, whose crash catcher is used in `port/psp/exc/`
- Spike, the developer of Dragon Ball Z: Budokai Tenkaichi 3
- The [PSPDEV](https://pspdev.github.io/) team
- The third-party code and data used by the port are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

## License

The port's code is under the [GNU Affero General Public License, version 3](LICENSE), the license of
Tenkaichi3Decomp's port code since October 2026. The port code it is based on, from before that change, came under
the MIT license; its notice is kept in [LICENSE-MIT](LICENSE-MIT). `port/psp/exc/` is used under the PS2RC License
(see `port/psp/exc/LICENSE` and `port/psp/exc/NOTICE.md`). `port/vita/vitagl/vitagl.patch` modifies vitaGL and is
under vitaGL's license (LGPL-3.0).

The game's code in `src/` and `include/` comes from the decompilation, and the EBOOT's images use the game's
artwork: they belong to the game's rights holders. This project is not affiliated with or endorsed by them, and
distributes none of the game's data. You need your own legally obtained copy of the game to use it.
