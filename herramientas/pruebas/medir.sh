#!/bin/bash
# medir.sh <game second> <tag> [ps2rc.cfg lines...] - one PPSSPP speed measurement of the converted game: the demo
# battle fast-forwarded to <game second>, then BSECS (default 5) real seconds measured; prints the BENCH lines
# (game seconds per real second, frames shown). Example:
#   bash medir.sh 20 mapa demo_direct=1 demo_skip=1 map_native=1
# PPSSPP timings are not a real PSP's: compare runs with each other only.
. "$(dirname "$0")/../config.sh"
AT=$1; TAG=$2; shift 2
mkdir -p "$BT3_GAME/bench"
cp "$BT3_GAME/ps2rc.cfg" "$BT3_WORK/ps2rc.cfg.saved" 2>/dev/null
rm -f "$BT3_GAME/session.log"
{ printf 'mem_volatile=1\nscreen=0\nbench_at=%s\nbench_secs=%s\n' "$AT" "${BSECS:-5}"; for l in "$@"; do echo "$l"; done; } > "$BT3_GAME/ps2rc.cfg"
start=$(date +%s)
bt3_ppsspp_start
until grep -qE "BENCH: [0-9.]+ game s|result=|CRASH|WATCHDOG" "$BT3_GAME/session.log" 2>/dev/null; do
  sleep 2
  [ $(( $(date +%s) - start )) -gt 400 ] && { echo "TIMEOUT"; break; }
done
sleep 2; bt3_ppsspp_stop
cp "$BT3_GAME/session.log" "$BT3_GAME/bench/session_$TAG.log" 2>/dev/null
cp "$BT3_WORK/ps2rc.cfg.saved" "$BT3_GAME/ps2rc.cfg" 2>/dev/null
echo "[$TAG] wall $(( $(date +%s) - start )) s"
grep -E "BENCH: [0-9.]+ game s|gs_map: frame|WATCHDOG|CRASH" "$BT3_GAME/session.log" | tail -3 | cut -c1-200
