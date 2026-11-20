#!/bin/bash
# config.sh - the local paths the tools use (Git Bash on Windows). Export them before running a tool, or edit the
# defaults here. Nothing here is uploaded anywhere: the dumps and maps are made from your own disc on this PC and
# must never be distributed.
#
#   BT3_GAME     the converted game's folder on the memory stick, as a Git Bash path, e.g.
#                /c/PPSSPP/memstick/PSP/GAME/PS2RC_SLUS-21678 (the folder build_psp.* wrote)
#   BT3_PPSSPP   PPSSPP's executable (Windows path), for the extraction and the tests
#   BT3_WORK     where the stage dumps, the glTF exports and the packed maps go (default: ../work_mapas)
#   BLENDER      Blender's executable (Windows path), for stage_lod.py
#   MO_VENV      the WSL Python venv with numpy + pillow (default ~/mo_venv, made by mapas/build_meshopt.sh)
#   MO_LIB       meshoptimizer built as a shared library in WSL (default ~/mo_src/libmeshopt.so, same script)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${BT3_GAME:?export BT3_GAME=<the game folder on the memory stick, Git Bash path>}"
BT3_PPSSPP=${BT3_PPSSPP:-'C:\PPSSPP\PPSSPPWindows64.exe'}
BT3_WORK=${BT3_WORK:-"$HERE/../work_mapas"}
BLENDER=${BLENDER:-'C:\Program Files\Blender Foundation\5.0\blender.exe'}
MO_VENV=${MO_VENV:-'~/mo_venv'}
MO_LIB=${MO_LIB:-'~/mo_src/libmeshopt.so'}
mkdir -p "$BT3_WORK"
BT3_WORK="$(cd "$BT3_WORK" && pwd)"
EB_WIN="$(cygpath -w "$BT3_GAME/EBOOT.PBP")"

# starts PPSSPP on the game's EBOOT (killing a running one first)
bt3_ppsspp_start() {
  taskkill //IM PPSSPPWindows64.exe //F >/dev/null 2>&1; sleep 1
  powershell -NoProfile -Command "Start-Process -FilePath '$BT3_PPSSPP' -ArgumentList '\"$EB_WIN\"'" >/dev/null
}
bt3_ppsspp_stop() { taskkill //IM PPSSPPWindows64.exe //F >/dev/null 2>&1; }
