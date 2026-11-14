#!/bin/bash
# build_psp.sh <ISO> <output folder> [--fast] [extra converter options]
# Builds the PSP version of DBZ Budokai Tenkaichi 3 (SLUS-216.78) from your own ISO.
# --fast: the ABI-mode translation (games/SLUS_216.78_v1.00/fast.toml), its own work folder (work_fast) and the
#         XMB title "BT3 PSP fast", so both builds can sit side by side on the memory stick for an A/B test.
# Based on PS2RC by SandstoneDev - https://github.com/SandstoneDev/PS2RC
set -e
if [ $# -lt 2 ]; then
  echo "Uso: bash build_psp.sh /ruta/a/la.ISO /carpeta/de/salida [--fast] [--toolchain carpeta] [--dry-run]"
  exit 1
fi
ISO=$1; OUT=$2; shift 2
KIT="$(cd "$(dirname "$0")" && pwd)"
PACK="$KIT/PS2RC/games/SLUS_216.78_v1.00"
WORK="$KIT/work"; TITLE=()
rm -f "$PACK/pack.private.toml"
if [ "$1" = "--fast" ]; then
  shift
  cp "$PACK/fast.toml" "$PACK/pack.private.toml"
  WORK="$KIT/work_fast"; TITLE=(--title "BT3 PSP fast")
  echo "--fast: ABI-mode translation (fast.toml), work folder $WORK"
fi
cd "$KIT/PS2RC"
PY=$(command -v python3 || command -v python)
"$PY" -c "import elftools, PIL" 2>/dev/null || "$PY" -m pip install pyelftools pillow
set +e
"$PY" -m ps2rc.convert "$ISO" --out "$OUT" --work "$WORK" --no-splash "${TITLE[@]}" "$@"
RC=$?
rm -f "$PACK/pack.private.toml"
[ $RC -eq 0 ] || { echo "La conversion fallo (codigo $RC)."; exit $RC; }
echo "Listo: copia $OUT a PSP/GAME/ de la Memory Stick."
