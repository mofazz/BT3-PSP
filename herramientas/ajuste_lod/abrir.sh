#!/bin/bash
# abrir.sh - starts the LOD tuner (servidor.py) with the paths of herramientas/config.sh and opens it in the browser:
# http://127.0.0.1:8765 . Ctrl+C here stops it.
. "$(dirname "$0")/../config.sh"
T="$(cd "$(dirname "$0")" && pwd)"
export BT3_WORK="$(cygpath -w "$BT3_WORK")" BT3_GAME="$(cygpath -w "$BT3_GAME")" BT3_PPSSPP BLENDER MO_VENV MO_LIB
(sleep 2; start "" "http://127.0.0.1:${BT3_TUNER_PORT:-8765}") &
python "$T/servidor.py"
