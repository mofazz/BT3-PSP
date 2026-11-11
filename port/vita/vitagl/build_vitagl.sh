#!/bin/sh
# Builds vitaGL (https://github.com/Rinnegatamante/vitaGL) at the commit the port was made with, applies the port's
# changes (vitagl.patch: fragment programs kept per blend setting, a larger vertex layout cache, scene splitting and
# texture set-up helpers) and installs it into $VITASDK.
#
#   port/vita/vitagl/build_vitagl.sh [work folder]        (default: port/build/vitagl)
set -e
COMMIT=c9bcd7423438163290fb888cc44ab30032ca2dc8
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/../../build/vitagl}
[ -n "$VITASDK" ] || { echo "VITASDK is not set"; exit 1; }
if [ ! -d "$WORK/.git" ]; then
    git clone https://github.com/Rinnegatamante/vitaGL "$WORK"
fi
cd "$WORK"
git fetch --quiet origin
git checkout --quiet --force "$COMMIT"
git apply "$HERE/vitagl.patch"
make clean > /dev/null
make -j"$(nproc)" NO_DEBUG=1 HAVE_VERTEX_LAYOUT_CACHE=1 USE_SCRATCH_MEMORY=1
make install NO_DEBUG=1 HAVE_VERTEX_LAYOUT_CACHE=1 USE_SCRATCH_MEMORY=1
