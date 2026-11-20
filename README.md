# BT3-PSP

> Based on PS2RC by SandstoneDev  
> https://github.com/SandstoneDev/PS2RC  
> Community: https://discord.gg/wV8x46ZTDu

An unofficial PSP port of **Dragon Ball Z: Budokai Tenkaichi 3** (PS2, NTSC-U, **SLUS-216.78**, version 1.00), made
with the **PS2RC** static recompiler: the game's code is translated from MIPS to C and compiled for the PSP on your own
PC, from **your own ISO**. It is not an emulator.

This repository contains **no game data**: no ISO, no translated code, no textures and no maps. Everything derived from
the disc (the translation, the EBOOT, stage dumps and native maps) is made on your machine and must not be shared.

## Status

Work in progress; not full speed yet. Measured in PPSSPP (its timings are not a real PSP's; use them to compare runs):

| | Game speed |
|---|---|
| Battle with the game's own stage | ~0.46x |
| Battle with a native map (`map_native=1`) | 0.40x – 0.77x depending on the stage |
| Battle with nothing drawn (the game logic's own limit) | ~0.75x |

- Menus, the demo and battles run; fighters, effects and the HUD are drawn with the PSP's GE.
- **Native maps**: the game's static stage is replaced by a map prepared on the PC (cells with two levels of detail, a
  half-sphere sky, distant buildings as boxes). The game no longer culls or draws that stage itself; the scrolling sky,
  animated objects and debris are still the game's.
- No sound (ADX streams are silent) and no intro movies.

## Contents

| Path | What it is |
|---|---|
| `PS2RC/` | The PS2RC recompiler (licence in `PS2RC/LICENSE`) with the game pack `PS2RC/games/SLUS_216.78_v1.00/`: hooks, native functions, the GS→GE renderer and the native map (`src/gs_ge_map.h`) |
| `build_psp.bat` / `build_psp.sh` | Build the PSP folder from the ISO (Windows / WSL-Linux) |
| `herramientas/mapas/` | Stage extraction and native map generation with LOD |
| `herramientas/ajuste_lod/` | The LOD tuner: a local page to tune each map, compare variants and choose one |
| `herramientas/pruebas/` | Speed measurement in PPSSPP |
| `herramientas/config.sh` | The local paths the tools use |

Prefer a window? The **BT3 PSP Builder** in the [releases](https://github.com/mofazz/BT3-PSP/releases) wraps the same
converter with a private Python.

## 1. Build the game

Requirements:

1. **Python 3.10+** on Windows with `pyelftools` and `pillow` (`python -m pip install pyelftools pillow`).
2. **A PSP toolchain**: WSL2 (Ubuntu) with [pspdev](https://pspdev.github.io/) in `~/pspdev` (the converter finds it),
   or a native toolchain folder passed with `--toolchain`.
3. Your ISO of SLUS-216.78 v1.00 (the converter checks the boot program's size and SHA-256; other releases are
   refused).

```
build_psp.bat "C:\path\BT3.iso" "C:\output"
```

It takes about 15 minutes (translation + compile). Copy the folder it writes (`PS2RC_SLUS21678`) to `PSP/GAME/` on
the Memory Stick (or PPSSPP's `memstick`). The work folder `work/` (translated code) stays next to the script; after
changing the pack, run again with `--restart`.

## 2. Native maps (optional, recommended)

Made **on your PC** from the converted game. Extra requirements:

- [PPSSPP](https://www.ppsspp.org/) (to dump the stages), [Blender](https://www.blender.org/) 4.x/5.x (the LOD
  structure) and Git Bash.
- meshoptimizer in WSL: `wsl bash herramientas/mapas/build_meshopt.sh` (downloads
  [zeux/meshoptimizer](https://github.com/zeux/meshoptimizer) v0.22, MIT, builds it with g++ and makes `~/mo_venv`).

In Git Bash:

```
export BT3_GAME=/c/PPSSPP/memstick/PSP/GAME/PS2RC_SLUS21678   # the converted game's folder
export BT3_PPSSPP='C:\PPSSPP\PPSSPPWindows64.exe'
bash herramientas/mapas/volcar_escenarios.sh                  # ~2 min per stage (1..34)
bash herramientas/mapas/generar_mapas.sh                      # ~30 s per stage
```

1. `volcar_escenarios.sh` starts the demo battle on each stage (`demo_stage=N`) and saves its whole static geometry
   (`stage_dump=1200`) in `work_mapas/stage_dumps/`.
2. `generar_mapas.sh`: export to glTF → cells and blocks in Blender, every piece classed as ground, building,
   tree/cut-out, other or a stage's own kind → each kind reduced on its own with meshoptimizer (the ground as one base
   surface plus its overlay layers; the sky as a half sphere) → `maps/stage_NN.bt3m` in the game folder.

Per-stage settings and rules for special pieces (e.g. the stands, the house and the platform of the Tournament,
stage 4) live in `herramientas/mapas/stage_rules.json`.

### Tuning each map by hand: the LOD tuner

```
python -m pip install PySide6 PyOpenGL
bash herramientas/ajuste_lod/abrir.sh          # the desktop window (ajustador.py)
bash herramientas/ajuste_lod/abrir.sh web      # the same tool as a local page, http://127.0.0.1:8765
```

A window with an OpenGL 3D viewer (left drag orbits, right drag pans, the wheel zooms, F frames the stage), per
stage:

- **Preparar estructura**: exports the stage's dump and builds the cells, blocks and kinds in Blender (again after
  changing a rule).
- **Reducción por tipo**: for every kind (ground, buildings, trees/cut-outs, others and the stage's own kinds) the
  share of triangles `r` and the allowed error `e`, near (LOD1) and far (LOD2).
- **Generar variante** / **4 semillas**: meshoptimizer runs with those settings (or with them ×0.5, ×0.75, ×1 and
  ×1.5) and each variant is listed with its triangles per kind.
- **Vista**: the original stage, the near level, the far level, or the game's choice by distance from the viewer's
  camera; colours by kind and wireframe; the visible triangle count.
- **Pieza seleccionada**: in the original view, click a building, tree or rock: it can get its own reduction (a rule
  with its box).
- **Elegir para el juego**: packs that variant into `maps/stage_NN.bt3m` in the game folder and saves its settings in
  `stage_rules.json`, so `generar_mapas.sh` makes the same map again. **Probar en PPSSPP** opens that stage.

## Options (`ps2rc.cfg` in the game folder)

| Option | Effect |
|---|---|
| `map_native=1` | Use the stage's native map when there is one (`maps/stage_NN.bt3m`); on by default in the converted folder |
| `map_lod=1` / `2` | Force one level of detail (tests) |
| `demo_direct=1` | Start straight in the demo battle |
| `demo_skip=N` / `demo_stage=N` | Another demo pairing / force stage N |
| `eft_off=bits` | Turn effect groups off (cost tests): 1 particles, 2 dust, 4 scrolling sky, 8 water, 16 stage, 64 fighters, 128 HUD… |
| `pace_maxlag_ms=67` | The most lag the game tries to win back by running faster (0 = no limit) |
| `gs_frameskip=-1` | Automatic frame skip (off by default: it looks choppy and the game logic caps it at ~0.75x) |
| `prof=1` | Profiler: a summary of where the time goes in `session.log` |

## Measuring

```
bash herramientas/pruebas/medir.sh 20 test demo_direct=1 demo_skip=1 map_native=1
bash herramientas/pruebas/probar_mapas.sh 4 29
```

## Credits and licences

- **PS2RC** by SandstoneDev (https://github.com/SandstoneDev/PS2RC, community: https://discord.gg/wV8x46ZTDu).
  Licence in `PS2RC/LICENSE` and `PS2RC/NOTICE.md` (unchanged): free of charge, no game data.
- **meshoptimizer** by Arseny Kapoulkine (MIT), downloaded by `build_meshopt.sh`; not included here.
- The game's function addresses and names come from [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp);
  this repository contains none of its code.
- Unofficial port, not affiliated with Bandai Namco, Spike or Sony. "PlayStation", "PS2" and "PSP" are trademarks of
  Sony Interactive Entertainment.
