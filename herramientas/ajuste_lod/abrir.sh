#!/bin/bash
# abrir.sh [web] - starts the LOD tuner with the paths of herramientas/config.sh: the desktop window (ajustador.py,
# PySide6 + OpenGL; needs: pip install PySide6 PyOpenGL), or with "web" the same tool as a local page
# (servidor.py, http://127.0.0.1:8765, Ctrl+C stops it).
. "$(dirname "$0")/../config.sh"
T="$(cd "$(dirname "$0")" && pwd)"
export BT3_WORK="$(cygpath -w "$BT3_WORK")" BT3_GAME="$(cygpath -w "$BT3_GAME")" BT3_PPSSPP BLENDER MO_VENV MO_LIB
if [ "$1" = web ]; then
  (sleep 2; start "" "http://127.0.0.1:${BT3_TUNER_PORT:-8765}") &
  python "$T/servidor.py"
else
  python -c "import PySide6, OpenGL" 2>/dev/null || python -m pip install PySide6 PyOpenGL
  python "$T/ajustador.py"
fi
