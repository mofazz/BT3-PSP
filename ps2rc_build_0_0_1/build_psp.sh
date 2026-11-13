#!/bin/bash
# build_psp.sh <ISO> <output folder> [extra converter options]
# Builds the PSP version 0.0.1 of DBZ Budokai Tenkaichi 3 (SLUS-216.78) from your own ISO.
# Based on PS2RC by SandstoneDev - https://github.com/SandstoneDev/PS2RC
set -e
if [ $# -lt 2 ]; then
  echo "Uso: bash build_psp.sh /ruta/a/la.ISO /carpeta/de/salida [--toolchain carpeta] [--dry-run]"
  exit 1
fi
ISO=$1; OUT=$2; shift 2
cd "$(dirname "$0")/PS2RC"
PY=$(command -v python3 || command -v python)
"$PY" -c "import elftools, PIL" 2>/dev/null || "$PY" -m pip install pyelftools pillow
"$PY" -m ps2rc.convert "$ISO" --out "$OUT" --work "$(cd .. && pwd)/work" --no-splash "$@"
echo "Listo: copia $OUT a PSP/GAME/ de la Memory Stick."
