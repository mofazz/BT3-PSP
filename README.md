# BT3-PSP

> Based on PS2RC by SandstoneDev  
> https://github.com/SandstoneDev/PS2RC  
> Community: https://discord.gg/wV8x46ZTDu

Port no oficial para PSP de **Dragon Ball Z: Budokai Tenkaichi 3** (PS2, NTSC-U, **SLUS-216.78**, versión 1.00),
hecho con el recompilador estático **PS2RC**: el código del juego se traduce de MIPS a C y se compila para la PSP en
tu propio PC, a partir de **tu propia ISO**. No es un emulador.

Este repositorio **no contiene datos del juego**: ni la ISO, ni código traducido, ni texturas, ni mapas. Todo lo que
sale del disco (traducción, EBOOT, volcados y mapas nativos) se genera en tu máquina y no debe distribuirse.

## Estado

En desarrollo; no llega aún a velocidad completa. Medido en PPSSPP (sus tiempos no son los de una PSP real; sirven
para comparar):

| | Velocidad del juego |
|---|---|
| Batalla con el escenario original del juego | ~0,46x |
| Batalla con mapa nativo (`map_native=1`) | 0,40x – 0,77x según el escenario |
| Batalla sin dibujar nada (límite de la lógica del juego) | ~0,75x |

- Menús, demo y batallas funcionan; los luchadores, efectos y HUD se dibujan con la GE de la PSP.
- **Mapas nativos**: el escenario estático del juego se sustituye por un mapa preparado en el PC (celdas con dos
  niveles de detalle, cielo de media esfera, edificios lejanos como cajas). El juego deja de hacer su culling y su
  dibujo de ese escenario; el cielo animado, los objetos animados y los escombros siguen siendo del juego.
- Sin sonido (el ADX va en silencio) ni vídeos de introducción.

## Contenido

| Ruta | Qué es |
|---|---|
| `PS2RC/` | El recompilador PS2RC (licencia en `PS2RC/LICENSE`) con el paquete del juego `PS2RC/games/SLUS_216.78_v1.00/`: hooks, funciones nativas, el renderizador GS→GE y el mapa nativo (`src/gs_ge_map.h`) |
| `build_psp.bat` / `build_psp.sh` | Construyen la carpeta de PSP desde la ISO (Windows / WSL-Linux) |
| `herramientas/mapas/` | Extracción de escenarios y generación de los mapas nativos con LOD |
| `herramientas/pruebas/` | Medición de velocidad en PPSSPP |
| `herramientas/config.sh` | Rutas locales que usan las herramientas |

## 1. Construir el juego

Requisitos:

1. **Python 3.10+** en Windows con `pyelftools` y `pillow` (`python -m pip install pyelftools pillow`).
2. **Toolchain de PSP**: WSL2 (Ubuntu) con [pspdev](https://pspdev.github.io/) en `~/pspdev` (el conversor lo
   encuentra solo), o una carpeta de toolchain nativa con `--toolchain`.
3. Tu ISO de SLUS-216.78 v1.00 (el conversor comprueba tamaño y SHA-256 del ejecutable; otras versiones se rechazan).

```
build_psp.bat "C:\ruta\BT3.iso" "C:\salida\PS2RC_BT3"
```

Tarda unos 15 minutos (traducción + compilación). Copia la carpeta de salida a `PSP/GAME/` de la Memory Stick (o del
`memstick` de PPSSPP). La carpeta de trabajo `work/` (código traducido) queda junto al script; tras cambiar el paquete,
vuelve a ejecutar con `--restart`.

## 2. Mapas nativos (opcional, recomendado)

Se generan **en tu PC** desde el juego ya convertido. Requisitos adicionales:

- [PPSSPP](https://www.ppsspp.org/) (para volcar los escenarios), [Blender](https://www.blender.org/) 4.x/5.x
  (estructura de LOD) y Git Bash.
- meshoptimizer en WSL: `wsl bash herramientas/mapas/build_meshopt.sh` (descarga
  [zeux/meshoptimizer](https://github.com/zeux/meshoptimizer) v0.22, MIT, y lo compila con g++; crea `~/mo_venv`).

En Git Bash:

```
export BT3_GAME=/c/PPSSPP/memstick/PSP/GAME/PS2RC_BT3        # la carpeta del juego convertido
export BT3_PPSSPP='C:\PPSSPP\PPSSPPWindows64.exe'
bash herramientas/mapas/volcar_escenarios.sh                  # ~2 min por escenario (1..34)
bash herramientas/mapas/generar_mapas.sh                      # ~30 s por escenario
```

1. `volcar_escenarios.sh` arranca la demo en cada escenario (`demo_stage=N`) y guarda su geometría estática completa
   (`stage_dump=1200`) en `work_mapas/stage_dumps/`.
2. `generar_mapas.sh`: exportación a glTF → celdas y bloques en Blender, cada pieza clasificada como suelo, edificio,
   árbol/recortado, otros o un tipo propio del escenario → reducción por tipo con meshoptimizer (el suelo como una
   superficie base más sus capas superpuestas; el cielo como media esfera) → `maps/stage_NN.bt3m` en la carpeta del
   juego.

Ajustes por escenario y reglas para piezas especiales (por ejemplo las gradas, la casa y la plataforma del Torneo,
escenario 4) en `herramientas/mapas/stage_rules.json`.

## Opciones (`ps2rc.cfg` en la carpeta del juego)

| Opción | Efecto |
|---|---|
| `map_native=1` | Usa el mapa nativo del escenario si existe (`maps/stage_NN.bt3m`); por defecto en la carpeta convertida |
| `map_lod=1` / `2` | Fuerza un nivel de detalle (pruebas) |
| `demo_direct=1` | Arranca directamente en la batalla de demostración |
| `demo_skip=N` / `demo_stage=N` | Elige otra pareja de la demo / fuerza el escenario N |
| `eft_off=bits` | Desactiva grupos de efectos (pruebas de coste): 1 partículas, 2 polvo, 4 cielo animado, 8 agua, 16 escenario, 64 luchadores, 128 HUD… |
| `pace_maxlag_ms=67` | Retraso máximo que el juego intenta recuperar acelerando (0 = sin límite) |
| `gs_frameskip=-1` | Salto de fotogramas automático (desactivado por defecto: se ve cortado y la lógica limita a ~0,75x) |
| `prof=1` | Perfilador: resumen de tiempos en `session.log` |

## Medir

```
bash herramientas/pruebas/medir.sh 20 prueba demo_direct=1 demo_skip=1 map_native=1
bash herramientas/pruebas/probar_mapas.sh 4 29
```

## Créditos y licencias

- **PS2RC** por SandstoneDev (https://github.com/SandstoneDev/PS2RC, comunidad: https://discord.gg/wV8x46ZTDu).
  Licencia en `PS2RC/LICENSE` y `PS2RC/NOTICE.md` (sin cambios): uso gratuito, sin datos de juegos.
- **meshoptimizer** por Arseny Kapoulkine (MIT), descargado por `build_meshopt.sh`; no se incluye aquí.
- Las direcciones y nombres de funciones del juego se tomaron de
  [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp); este repositorio no contiene su código.
- Port no oficial, sin relación con Bandai Namco, Spike ni Sony. "PlayStation", "PS2" y "PSP" son marcas de Sony
  Interactive Entertainment.
