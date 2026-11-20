#!/bin/bash
# generar_mapas.sh [stage ids...] - the native maps from the stage dumps (volcar_escenarios.sh), on this PC:
#   1. stage_export.py   dump -> glTF + PNG                                            (Windows Python)
#   2. stage_lod.py      cells (LOD1, near) inside blocks (LOD2, far); every piece classed as ground, building,
#                        tree / cut-out, other or a stage rule's own kind; buildings boxed for LOD2  (Blender)
#   3. stage_meshopt.py  each kind reduced apart with meshoptimizer; ground as one base surface plus its overlay
#                        layers; the sky dome a low half sphere                         (WSL: build_meshopt.sh)
#   4. stage_pack.py     -> $BT3_WORK/stage_NN.bt3m, copied to $BT3_GAME/maps/          (Windows Python)
# Per-stage settings and rules: stage_rules.json. Default: every dump present. The maps come from your disc: they stay
# on this PC and are never distributed. ps2rc.cfg map_native=1 (the default of the converted game) draws them.
. "$(dirname "$0")/../config.sh"
T="$(cd "$(dirname "$0")" && pwd)"
RULES="$T/stage_rules.json"
mkdir -p "$BT3_GAME/maps" "$BT3_WORK/export"
LIST="$*"; [ -z "$LIST" ] && LIST=$(ls "$BT3_WORK"/stage_dumps/stage_*.bin 2>/dev/null | sed 's/.*stage_\([0-9]*\)\.bin/\1/')
WT=$(wsl wslpath -a "$(cygpath -m "$T")"); WW=$(wsl wslpath -a "$(cygpath -m "$BT3_WORK")")
for nn in $LIST; do
  n=$((10#$nn)); nn=$(printf %02d $n)
  E="$BT3_WORK/export/s$nn"; L="$BT3_WORK/export/s${nn}_lod"; M="$BT3_WORK/export/s${nn}_mo"
  args() { python -c "import json,sys; d=json.load(open(sys.argv[1])).get(sys.argv[2], {}); print(d.get(sys.argv[3], '') if isinstance(d, dict) else '')" "$(cygpath -w "$RULES")" "$n" "$1"; }
  LA=$(args lod_args); MA=$(args meshopt_args)
  [ -f "$E/s$nn.gltf" ] || python "$T/stage_export.py" "$BT3_WORK/stage_dumps/stage_$nn.bin" "$E" --name s$nn \
      > "$BT3_WORK/export/s$nn.log" 2>&1 || { echo "stage $nn: export failed"; continue; }
  rm -rf "$L" "$M"
  "$BLENDER" -b --factory-startup --python "$(cygpath -w "$T/stage_lod.py")" -- "$(cygpath -w "$E/s$nn.gltf")" \
      "$(cygpath -w "$L/s${nn}_lod.gltf")" max_tris=400 min_cell=250 grp_tris=1500 g1f=0 g1min=60 box_h=150 nodec=1 \
      classes=1 rules="$(cygpath -w "$RULES")" stage=$n $LA > "$BT3_WORK/export/s$nn.lod.log" 2>&1
  [ -f "$L/s${nn}_lod.gltf" ] || { echo "stage $nn: LOD failed (export/s$nn.lod.log)"; continue; }
  wsl bash -lc "cd '$WW/export' && $MO_VENV/bin/python '$WT/stage_meshopt.py' s${nn}_lod/s${nn}_lod.gltf s${nn}_mo/s${nn}_mo.gltf \
      lib=$MO_LIB rules='$WT/stage_rules.json' stage=$n $MA" > "$BT3_WORK/export/s$nn.mo.log" 2>&1
  [ -f "$M/s${nn}_mo.gltf" ] || { echo "stage $nn: meshoptimizer failed (export/s$nn.mo.log)"; continue; }
  python "$T/stage_pack.py" "$M/s${nn}_mo.gltf" "$BT3_WORK/stage_$nn.bt3m" --stage $n > "$BT3_WORK/export/s$nn.pack.log" 2>&1 \
      || { echo "stage $nn: pack failed"; continue; }
  cp "$BT3_WORK/stage_$nn.bt3m" "$BT3_GAME/maps/"
  echo "stage $nn: $(tail -1 "$BT3_WORK/export/s$nn.pack.log" | sed 's/.*: stage [0-9]*, //')"
done
