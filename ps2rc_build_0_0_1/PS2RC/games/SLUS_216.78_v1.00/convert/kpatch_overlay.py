"""BT3 (SLUS-216.78 v1.00): the kernel patches as one translatable MWo3 overlay image.

At start-up the game copies blocks of MIPS code from its data segment into the EE kernel's RAM and runs them as
syscall handlers (seen in the host harness, syscall 90 "Copy" then SetSyscall):
    Copy(80076000, 002ea530, 740)   -> syscall 91 and the handlers it hands out (252-255, 300, 8)
    Copy(80075000, 002ead00, 330)   -> syscall 91 again, later in the start-up
That code is linked for its kernel address, so it is never part of the translated program. Both blocks stay resident
together, so they go into ONE overlay image covering [80075000, 80076740) (zero fill between them), loaded at
80075000 - 0x40; translate.py translates it like any overlay (pack.toml [overlays]) and src/pack_main.c makes it the
resident overlay from boot.

Everything comes from the player's own disc and goes to the work dir (never into the pack).

    python kpatch_overlay.py <SLUS_216.78 path> <output .mwo3 path>
"""
import hashlib
import struct
import sys

BLOCKS = [(0x002EAD00, 0x80075000, 0x330),      # (ELF address, kernel address, size)
          (0x002EA530, 0x80076000, 0x740)]
BASE = 0x80075000
END = 0x80076740
OVERLAY_ID = 1
NAME = b"bt3_kpatch"
SEGS = [(0x1000, 0x100000, 0x1C33C0), (0x1C4400, 0x2C3400, 0x3BD6E)]      # (file offset, address, size) of PT_LOAD
ELF_SHA256 = "811188ba9b416500d921cd4d9514df0cbf42f3a41a99cf5aac5a3da37171bf99"


def file_offset(va, size):
    for off, base, segsize in SEGS:
        if base <= va and va + size <= base + segsize:
            return off + va - base
    raise SystemExit("address %08x is outside the program's file data" % va)


def main(elf_path, out_path):
    elf = open(elf_path, "rb").read()
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        raise SystemExit("%s is not the SLUS_216.78 v1.00 boot program this pack supports" % elf_path)
    text = bytearray(END - BASE)
    for src, dst, size in BLOCKS:
        o = file_offset(src, size)
        text[dst - BASE:dst - BASE + size] = elf[o:o + size]
    hdr = b"MWo3" + struct.pack("<7I", OVERLAY_ID, BASE - 0x40, len(text), 0, 0, 0, 0) + NAME.ljust(32, b"\0")
    assert len(hdr) == 0x40
    with open(out_path, "wb") as f:
        f.write(hdr + bytes(text))
    for src, dst, size in BLOCKS:
        print("kernel patch: %#x bytes from %08x at %08x" % (size, src, dst))
    print("overlay image %08x-%08x -> %s" % (BASE, END, out_path))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
