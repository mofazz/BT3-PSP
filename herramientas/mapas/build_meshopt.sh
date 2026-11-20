#!/bin/bash
# build_meshopt.sh - run once INSIDE WSL (Ubuntu): builds meshoptimizer's simplifier (zeux/meshoptimizer v0.22, MIT,
# downloaded from GitHub) as ~/mo_src/libmeshopt.so and makes ~/mo_venv (numpy + pillow) for stage_meshopt.py.
# (The pip package needs Visual Studio on Windows and python3-dev on Linux; the library alone needs only g++.)
set -e
sudo_hint() { echo "missing $1: sudo apt install $2"; exit 1; }
command -v g++ >/dev/null || sudo_hint g++ g++
command -v curl >/dev/null || sudo_hint curl curl
python3 -m venv --help >/dev/null 2>&1 || sudo_hint venv python3-venv
mkdir -p ~/mo_src && cd ~/mo_src
[ -d meshoptimizer-0.22 ] || { curl -sL -o mo.tar.gz https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.22.tar.gz && tar xzf mo.tar.gz; }
g++ -O2 -shared -fPIC -o ~/mo_src/libmeshopt.so meshoptimizer-0.22/src/*.cpp
[ -x ~/mo_venv/bin/python ] || python3 -m venv ~/mo_venv
~/mo_venv/bin/pip install -q numpy pillow
echo "ok: ~/mo_src/libmeshopt.so, ~/mo_venv"
