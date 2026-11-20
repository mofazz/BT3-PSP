#!/bin/bash
# probar_mapas.sh [stage ids...] - each stage's demo battle with its native map (map_native=1) for a short
# measurement: speed, whether the map loaded, its triangles per frame, any crash. Default: every map in $BT3_GAME/maps.
. "$(dirname "$0")/../config.sh"
T="$(cd "$(dirname "$0")" && pwd)"
LIST="$*"; [ -z "$LIST" ] && LIST=$(ls "$BT3_GAME"/maps/stage_[0-9][0-9].bt3m | grep -o '[0-9][0-9]\.bt3m' | grep -o '[0-9][0-9]')
for nn in $LIST; do
  n=$((10#$nn))
  bash "$T/medir.sh" 20 mapa$nn demo_direct=1 demo_stage=$n map_native=1 > /dev/null 2>&1
  L="$BT3_GAME/bench/session_mapa$nn.log"
  sp=$(grep -ho '= [0-9.]*x' "$L" | head -1)
  ld=$(grep -ho 'gs_map: stage [0-9]*: [0-9]* cells\|gs_map: no native map\|unusable\|needs [0-9]* KB' "$L" | head -1)
  tr=$(grep -h 'gs_map: frame' "$L" | tail -1 | grep -o '[0-9]* triangles per frame')
  bad=$(grep -hoE 'WATCHDOG|CRASH' "$L" | head -1)
  printf 'stage %02d: %s | %s | %s %s\n' $n "${sp:-NO BENCH}" "${ld:-no map line}" "$tr" "${bad:+| $bad}"
done
