# DBZ Budokai Tenkaichi 3 – PSP, build 0.0.1

> Based on PS2RC by SandstoneDev  
> https://github.com/SandstoneDev/PS2RC  
> Community: https://discord.gg/wV8x46ZTDu

Kit para construir la versión de PSP de *Dragon Ball Z: Budokai Tenkaichi 3* (NTSC-U, **SLUS-216.78**, versión
1.00) a partir de **tu propia ISO** del juego. El kit no contiene datos del juego: la ISO se lee en tu PC y todo lo
que se deriva de ella (código traducido, EBOOT, archivos del disco) se genera en tu máquina.

## Build 0.0.2 (English)

Changes made for speed and for the real PSP-3000. None of them has been compiled for the PSP or run yet: there was no
PSP toolchain where they were written. The default build's translation and prepare step were run on a PC with the
new hooks and passed.

| Change | Files | Why |
|---|---|---|
| memcmp, strcmp, strlen, strcpy, memmove, sqrtf, sinf, cosf, atanf, acosf, asinf and atan2f run as native C | `src/libc_hle.c`, `hooks.def` | In the PPSSPP logs, memcmp was 80–99 % of the game's loop iterations in menus and battle, with strcmp next. `ps2rc.cfg libc_hle=0` turns this off. |
| Smaller fixed memory window (28 MB) that grows to 32 MB when there is room | `pack.toml` (`ram_end_psp`, `[stage.cfg] heap_extra_kb=-1`, `mem_reserve_kb=3072`) | The old window needed 33.9 MB at start. PPSSPP reports about 5 MB more free memory than a real PSP has, so the real PSP-3000 probably did not have enough. 3 MB stay free for the GE list and the texture cache. |
| `log_sync=2` | `pack.toml [stage.cfg]` | With `log_sync=1`, the game thread opens, appends to and closes `session.log` for every line, which costs up to 100 ms per frame on a memory stick. Now the watchdog thread writes the log every 500 ms. |
| The "native VU1" log line is written every 100,000 batches (was every 5,000) | `src/gs_ge_vu1.h` | Less log traffic in battle. |
| `--fast` build option | `build_psp.sh`, `build_psp.bat`, `fast.toml`, `textpatch_fast.tsv` | This builds with ABI-mode translation (`v5s3`) instead of `v0`, which keeps every guest register in memory after every instruction. The PS2RC reference port uses an ABI mode. On a PC, the translation (9,535 functions, 0 stubbed) and the prepare step both ran, and every generated unit passed a syntax check. It has not been compiled for the PSP or run. |

### Rebuilding

The hook list changed, so the old `work` folder cannot be reused. Add `--restart`, or delete `work`:

```
build_psp.bat "C:\path\BT3.iso" "C:\out\normal" --restart
build_psp.bat "C:\path\BT3.iso" "C:\out\fast" --fast --restart
```

The `--fast` build shows as "BT3 PSP fast" in the XMB. Copy each output folder to `PSP/GAME/` under a different
folder name to compare them.

### What to send back

1. From the **real PSP**: `session.log`, right after a start that fails. The next start renames it to
   `session_prev.log`. Its first lines tell whether the problem is memory (`window_alloc`, `Not enough free memory`),
   the crash handler, or something later.
2. A profile: add `prof=1` to `ps2rc.cfg`, play one battle for a minute, then send `session.log`. A summary of where
   the time goes is written every 10 s. On the real PSP only, `prof_hw=1` also reads the CPU's cache-miss counters;
   never set it in PPSSPP.

Timings from PPSSPP are not meaningful: it does not charge for the GE, and it paces the game loop itself.

## Contenido

| Ruta | Qué es |
|---|---|
| `PS2RC/` | Recompilador PS2RC con el paquete del juego (`PS2RC/games/SLUS_216.78_v1.00/`) y el renderizador para la GE de la PSP |
| `build_psp.bat` | Construye la carpeta de PSP desde la ISO (Windows) |
| `build_psp.sh` | Lo mismo para WSL / Linux |
| `README_ERRORES_CONOCIDOS.md` | Errores corregidos y pendientes de esta versión |

## Requisitos

1. **Python 3.10 o superior** con `pyelftools` y `pillow`:
   ```
   python -m pip install pyelftools pillow
   ```
2. **Toolchain de PSP** (psp-gcc con PSPSDK). Cualquiera de estas opciones:
   - WSL2 con pspdev instalado en `~/pspdev` (https://pspdev.github.io/installation/windows.html); el conversor lo
     encuentra solo.
   - Una carpeta `toolchain` nativa de Windows: pásala con `--toolchain <carpeta>` o con la variable de entorno
     `PS2RC_TOOLCHAIN`.
3. Opcional: **ffmpeg** en el PATH para los vídeos de introducción (sin él se omiten).
4. La ISO del juego: SLUS-216.78 v1.00. El conversor comprueba el tamaño y el SHA-256 del ejecutable y rechaza
   otras revisiones.

## Construir

Windows:
```
build_psp.bat "C:\ruta\a\DBZ3_SLUS_21678.ISO" "C:\ruta\de\salida\PS2RC_BT3"
```

WSL / Linux:
```
bash build_psp.sh /ruta/a/DBZ3_SLUS_21678.ISO /ruta/de/salida/PS2RC_BT3
```

El conversor identifica el disco, extrae los archivos, genera los overlays del juego, traduce el código, compila el
`EBOOT.PBP` y copia los datos del juego. Tarda unos 15 minutos. El resultado queda en
`<carpeta de salida>\PS2RC_SLUS21678` (unos 2,9 GB): copia esa carpeta a `PSP/GAME/` de la Memory Stick, o a la de
PPSSPP.

Durante la conversión se crea `work/` junto a este README (unos 0,5 GB). Contiene datos derivados de tu disco: no la
compartas. Puedes borrarla al terminar; si la dejas, la próxima conversión reutiliza los pasos ya hechos.

Para ver el plan sin escribir nada, añade `--dry-run`. Para comprobar una carpeta ya convertida:
`python -m ps2rc.convert verify <carpeta>` (desde `PS2RC/`).

## Opciones (`ps2rc.cfg`, junto al EBOOT)

| Opción | Efecto |
|---|---|
| `osd=0` | Oculta los monitores de la parte superior (FPS, ms, velocidad, triángulos, memoria) |
| `demo_direct=1` | Arranca directamente en una pelea de demostración (pruebas) |
| `demo_skip=N` | Con `demo_direct`, elige otra de las peleas de demostración |
| `gs_dfog=1` | Dibuja la niebla por profundidad (oscurece la imagen: ver errores conocidos) |
| `gs_clip=0` | Sin recorte en CPU de los triángulos cercanos a la cámara |
| `gs_mark_skipped=0` | Usa como textura los destinos que la GE no dibuja (ruido en algunas peleas) |
| `gs_cld=0` | Lee las paletas de la memoria GS en vez del buffer de CLUT |

## Cambios de esta versión (0.0.1)

- Renderizador propio para la GE de la PSP, con programas VU1 nativos para el 3D.
- Sin la pantalla de créditos de inicio. El crédito a PS2RC está en este README, como pide su licencia.
- Monitores en pantalla (v0.0.1).
- Correcciones de texturas, frames oscuros, el suelo cerca de la cámara y la neblina en algunos escenarios: ver
  `README_ERRORES_CONOCIDOS.md`.

## Cambios del kit sobre PS2RC

Para que el conversor pueda construir este juego desde la ISO:
- `PS2RC/ps2rc/convert/pipeline.py`: los overlays que no son archivos del disco se generan con los scripts del
  paquete (`[overlays] makers` en `pack.toml`).
- `PS2RC/ps2rc/convert/toolchain.py`: rutas entrecomilladas en la compilación por WSL, para que funcione con carpetas
  que tienen espacios.
- `PS2RC/games/SLUS_216.78_v1.00/pack.toml`: receta `[convert]` (genera el trap set), los makers de los overlays y
  los archivos del disco que se copian (`BIN/*`, `DATA/*`).
- `PS2RC/games/SLUS_216.78_v1.00/convert/trapset.py` y `sce_hw_funcs.txt`: el trap set ya no depende de la carpeta
  del proyecto de descompilación.

## Licencias

PS2RC se distribuye bajo su propia licencia (`PS2RC/LICENSE`, `PS2RC/NOTICE.md`): uso gratuito, con el crédito de
arriba. Este kit mantiene esos archivos sin cambios. *Dragon Ball Z: Budokai Tenkaichi 3* pertenece a sus
propietarios; necesitas tu propia copia del juego.
