#!/bin/bash
# volcar_escenarios.sh [stage ids...] - extracts stages for the native maps, in PPSSPP, from your own converted game:
# for each stage N the demo battle is started on stage N (ps2rc.cfg demo_stage=N) and the frame of vblank 1200 is
# written whole (stage_dump=1200: every static mesh, its textures and matrices) to $BT3_GAME/stage_dump.bin, kept
# as $BT3_WORK/stage_dumps/stage_NN.bin. Default: stages 1..34 (35..39 do not load). About 2 min a stage.
# The dumps come from your disc: they stay on this PC.
. "$(dirname "$0")/../config.sh"
mkdir -p "$BT3_WORK/stage_dumps"
STAGES="$*"; [ -z "$STAGES" ] && STAGES=$(seq 1 34)
cp "$BT3_GAME/ps2rc.cfg" "$BT3_WORK/ps2rc.cfg.saved" 2>/dev/null
for n in $STAGES; do
  nn=$(printf %02d $n)
  rm -f "$BT3_GAME/session.log" "$BT3_GAME/stage_dump.bin"
  printf 'mem_volatile=1\ndemo_direct=1\ndemo_stage=%d\nosd=0\ngs_meshcache=0\nstage_dump=1200\n' $n > "$BT3_GAME/ps2rc.cfg"
  bt3_ppsspp_start
  start=$(date +%s); res=TIMEOUT
  while [ $(( $(date +%s) - start )) -lt 480 ]; do
    sleep 2
    grep -q "stage_dump: [0-9]* batches" "$BT3_GAME/session.log" 2>/dev/null && { res=OK; break; }
    grep -qE "CRASH|WATCHDOG|cannot write" "$BT3_GAME/session.log" 2>/dev/null && { res=CRASH; break; }
    if [ $(( $(date +%s) - start )) -gt 150 ] && ! grep -q "stage_dump: writing" "$BT3_GAME/session.log" 2>/dev/null; then
      res=NOLOAD; break
    fi
  done
  sleep 1; bt3_ppsspp_stop
  if [ $res = OK ]; then
    cp "$BT3_GAME/stage_dump.bin" "$BT3_WORK/stage_dumps/stage_$nn.bin"
    echo "stage $nn: $(grep -o 'stage_dump: [0-9]* batches, [0-9]* textures' "$BT3_GAME/session.log")"
  else
    echo "stage $nn: $res"
  fi
done
rm -f "$BT3_GAME/stage_dump.bin"
cp "$BT3_WORK/ps2rc.cfg.saved" "$BT3_GAME/ps2rc.cfg" 2>/dev/null
