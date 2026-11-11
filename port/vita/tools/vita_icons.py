"""The button icons of the game's text (the boot file's member 8, src/battle/col_c_b.c) for the Vita: L3 and R3,
pictures of the PS2's sticks, become the same black plates as L1 / L2 / R1 / R2, with a 3.

    make(icon_file) -> {texture index: (pixel indices h x w, 16 palette entries RGBA with the PS2's alpha 0..128)}

- 16 L3 and 19 R3: the L2 / R2 icons (15, 18) with the 2 redrawn as a 3, pixel by pixel in their own greys;
- 26 L3 and 27 R3 held down (two frames: the yellow arrow above the stick, then pressing it): the new plate under
  the arrow of the held square (24), the second frame pressed in by two pixels."""
import numpy as np
from bt3tex import texture

H = '0123456789abcdef'
# the 3, in the palette indices of the L2 / R2 icons (rows 11..21, the last one the plate's shadow under the digit)
THREE_L = ["22477422", "27aaaa72", "3aa88aa4", "5aa33aa5", "35526aa4", "2228aa72", "22255aa5", "27723aa6", "4aa88aa4",
           "27aaaa72", "24555542"]  # columns 16..23 of L2
THREE_R = ["2257632", "28aaa96", "6aa78a9", "7a826aa", "35428a9", "2228aa7", "22255aa", "27723aa", "6aa78a9",
           "28aaa96", "2455554"]  # columns 17..23 of R2
# the palette of the held icons: the plates' greys, the arrow's yellow, the white of the outer glow
HELD_PALETTE = [(0, 0, 0, 0), (0, 0, 0, 127), (19, 19, 19, 127), (36, 36, 36, 127), (63, 63, 63, 127), (81, 81, 81, 127),
                (96, 96, 96, 127), (126, 126, 126, 127), (182, 182, 182, 127), (225, 225, 225, 127), (242, 242, 242, 127),
                (255, 252, 3, 127), (255, 255, 255, 30), (255, 255, 255, 61), (255, 255, 255, 88), (255, 255, 255, 127)]


def palette_of(t):
    c = t['colors'].astype(int)
    return [tuple(int(v) for v in row) for row in c]


def plate3(d, src, rows, c0):
    t = texture(d, src)
    g = t['idx'].copy()
    for k, r in enumerate(rows):
        for j, ch in enumerate(r):
            g[11 + k, c0 + j] = H.index(ch)
    return g, palette_of(t)


def to_rgba(idx, pal):
    return np.array(pal, dtype=int)[idx]


def nearest(rgba, pal):
    p = np.array(pal, dtype=int)
    out = np.zeros(rgba.shape[:2], dtype=np.uint8)
    for y in range(rgba.shape[0]):
        for x in range(rgba.shape[1]):
            v = rgba[y, x]
            if v[3] < 8:
                out[y, x] = 0
                continue
            dist = ((p[:, :3] - v[:3]) ** 2).sum(1) * (v[3] / 128.0) + ((p[:, 3] - v[3]) * 3) ** 2
            out[y, x] = int(np.argmin(dist))
    return out


def held(d, plate_idx, plate_pal):
    """the held icon's two 32 x 32 frames side by side in its 64 x 64 texture"""
    t24 = texture(d, 24)
    arrow = to_rgba(t24['idx'][3:11, 2:18], palette_of(t24))  # 8 rows, 16 columns, its tip on the last row
    plate = to_rgba(plate_idx[6:27, 0:29], plate_pal)  # the plate and its glow, 21 rows
    img = np.zeros((64, 64, 4), dtype=int)

    def put(src, y, x):
        h, w = src.shape[:2]
        for yy in range(h):
            for xx in range(w):
                if src[yy, xx, 3] > 0 and 0 <= y + yy < 64 and 0 <= x + xx < 64:
                    img[y + yy, x + xx] = src[yy, xx]

    # frame 0: the arrow above the plate
    put(plate, 8, 0)
    put(arrow, 0, 6)
    # frame 1: pressed, plate and arrow two pixels lower, the arrow's tip on the plate's edge
    put(plate, 10, 32)
    put(arrow, 3, 38)
    return nearest(img, HELD_PALETTE), HELD_PALETTE


def make(d):
    l3, l3pal = plate3(d, 15, THREE_L, 16)
    r3, r3pal = plate3(d, 18, THREE_R, 17)
    return {16: (l3, l3pal), 19: (r3, r3pal), 26: held(d, l3, l3pal), 27: held(d, r3, r3pal)}
