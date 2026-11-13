"""BT3 (SLUS-216.78 v1.00): the menu overlay BIN/DBZP.BIN as a translatable MWo3 overlay image.

DBZP.BIN is a flat image the game loads at 0x00334C00 (Overlay_Load(0) in the decomp's src/main.c) and enters at
Progress_Main (0x00336A90): code 0x334C00..0x3B0E04 (file offset 0..0x7C204), data after it up to the end of the file
(0x3BE71C, where the game heap starts) - layout from the decomp's config/DBZP.yaml. This writes it with a 0x40-byte
MWo3 header in front (overlay id 2, loaded at 0x334C00 - 0x40), which translate.py translates like any overlay
(pack.toml [overlays]); src/pack_main.c makes it the overlay of the main window.

Everything comes from the player's own disc and goes to the work dir (never into the pack).

    python dbzp_overlay.py <disc folder> <output .mwo3 path>
"""
import os
import struct
import sys

VRAM = 0x00334C00
TEXT = 0x7C204
OVERLAY_ID = 2
NAME = b"dbzp"
SIZE = 563996                                     # BIN/DBZP.BIN of the SLUS-216.78 v1.00 disc


def main(disc, out_path):
    path = os.path.join(disc, "bin", "dbzp.bin")
    if not os.path.exists(path):
        path = os.path.join(disc, "BIN", "DBZP.BIN")
    raw = open(path, "rb").read()
    if len(raw) != SIZE:
        raise SystemExit("%s: %d bytes, the SLUS-216.78 v1.00 one has %d" % (path, len(raw), SIZE))
    hdr = b"MWo3" + struct.pack("<7I", OVERLAY_ID, VRAM - 0x40, TEXT, len(raw) - TEXT, 0, 0, 0) + NAME.ljust(32, b"\0")
    with open(out_path, "wb") as f:
        f.write(hdr + raw)
    print("menu overlay: %s, code %08x-%08x, data to %08x -> %s" % (path, VRAM, VRAM + TEXT, VRAM + len(raw), out_path))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
