#!/bin/sh
# One-step build (Linux, macOS, or WSL on Windows).
#   ./build.sh                      builds EBOOT.PBP
#   PSP_MS_ROOT=/mnt/e ./build.sh   also copies it to <stick>/PSP/GAME/BT3PROBE/
set -e
cd "$(dirname "$0")"
if ! command -v psp-config >/dev/null 2>&1; then
    [ -n "$PSPDEV" ] && export PATH="$PATH:$PSPDEV/bin"
fi
command -v psp-gcc >/dev/null 2>&1 || { echo "psp-gcc not found: set PSPDEV=~/pspdev and add \$PSPDEV/bin to PATH"; exit 1; }
make clean >/dev/null 2>&1 || true
make
echo "Built: $(pwd)/EBOOT.PBP"
if [ -n "$PSP_MS_ROOT" ]; then
    mkdir -p "$PSP_MS_ROOT/PSP/GAME/BT3PROBE"
    cp EBOOT.PBP "$PSP_MS_ROOT/PSP/GAME/BT3PROBE/"
    echo "Copied to $PSP_MS_ROOT/PSP/GAME/BT3PROBE/"
fi
