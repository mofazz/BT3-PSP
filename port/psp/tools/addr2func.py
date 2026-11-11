#!/usr/bin/env python3
"""Names for the addresses in crumbs.txt (or any list of hex addresses): addr2func.py BT3PSP.elf crumbs.txt
Needs `nm` (psp-nm or the host's). Newest call first."""
import bisect, shutil, subprocess, sys
elf, lst = sys.argv[1], sys.argv[2]
nm = shutil.which("psp-nm") or shutil.which("nm")
syms = []
for line in subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
for line in open(lst):
    line = line.strip()
    if not line or "=" in line:
        print(line)
        continue
    a = int(line, 16)
    k = bisect.bisect_right(addrs, a) - 1
    print(f"{a:08x}  {syms[k][1]}+0x{a - syms[k][0]:x}" if k >= 0 else f"{a:08x}  ?")
