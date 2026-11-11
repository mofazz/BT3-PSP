"""A 128 x 128 picture of a PS Vita (front view) for the options screen's Control Type page, in the shading style
of the console's own controller picture: a black, glossy body with rounded ends and a silver rim, the wide screen
in the middle, the d-pad and the left stick on the left, the four face buttons and the right stick on the right,
the L / R shoulder buttons along the top corners. RGBA, alpha 0..255."""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 8
N = 128
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'

def L():
    return Image.new('RGBA', (N * SS, N * SS), (0, 0, 0, 0))

def mask(fn):
    im = Image.new('L', (N * SS, N * SS), 0)
    fn(ImageDraw.Draw(im))
    return np.array(im).astype(float) / 255

def S(v):
    return v * SS

def rr(box, r):
    return lambda d: d.rounded_rectangle([S(v) for v in box], S(r), fill=255)

def ellipse(cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    return lambda d: d.ellipse((S(cx - rx), S(cy - ry), S(cx + rx), S(cy + ry)), fill=255)

def grow(m, px):
    r = int(round(px * SS))
    if r <= 0:
        return m
    from scipy.ndimage import binary_dilation
    y, x = np.mgrid[-r:r + 1, -r:r + 1]
    return binary_dilation(m > 0.5, structure=(x * x + y * y) <= r * r).astype(float)

def shrink(m, px):
    return 1.0 - grow(1.0 - m, px)

class Pic:
    def __init__(self):
        self.rgb = np.zeros((N * SS, N * SS, 3))
        self.a = np.zeros((N * SS, N * SS))

    def put(self, m, color, alpha=1.0):
        """m: coverage 0..1; color: rgb or an image H x W x 3"""
        c = np.array(color, float)
        if c.ndim == 1:
            c = c[None, None, :]
        k = (m * alpha)[..., None]
        self.rgb = c * k + self.rgb * (1 - k)
        self.a = m * alpha + self.a * (1 - m * alpha)

    def image(self):
        out = np.dstack([self.rgb.clip(0, 255), self.a[..., None] * 255]).astype(np.uint8)
        return Image.fromarray(out, 'RGBA').resize((N, N), Image.LANCZOS)

def vgrad(y0, y1, top, bottom, mid=None):
    ys = np.arange(N * SS) / SS
    t = np.clip((ys - y0) / (y1 - y0), 0, 1)[:, None, None]
    top, bottom = np.array(top, float), np.array(bottom, float)
    if mid is not None:
        mid = np.array(mid, float)
        col = np.where(t < 0.5, top * (1 - t * 2) + mid * (t * 2), mid * (2 - t * 2) + bottom * (t * 2 - 1))
    else:
        col = top * (1 - t) + bottom * t
    return np.broadcast_to(col, (N * SS, N * SS, 3))

def radial(cx, cy, r, inner, outer):
    y, x = np.mgrid[0:N * SS, 0:N * SS] / SS
    t = np.clip(np.hypot(x - cx, y - cy) / r, 0, 1)[..., None]
    return np.array(inner, float) * (1 - t) + np.array(outer, float) * t

def make():
    p = Pic()
    BX0, BY0, BX1, BY1, BR = 2, 37, 126, 93, 25
    body = mask(rr((BX0, BY0, BX1, BY1), BR))
    # shoulder buttons: bumps that follow the body's rounded top corners (the edge grown outwards, top corners only)
    ring = np.clip(grow(body, 2.6) - body, 0, 1)
    yy, xx = np.mgrid[0:N * SS, 0:N * SS] / SS
    for left in (True, False):
        reg = ((xx < 44) if left else (xx > 84)) & (yy < BY0 + 13)
        sh = ring * reg
        sh = np.array(Image.fromarray((sh * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(SS * 0.3))) / 255.0
        sh = (sh > 0.5).astype(float)
        p.put(grow(sh, 0.8), (8, 8, 10))
        p.put(sh, vgrad(BY0 - 3, BY0 + 8, (176, 176, 186), (70, 70, 78)))
    # the body: black outline, the silver rim, the glossy black front
    p.put(grow(body, 1.0), (8, 8, 10))
    p.put(body, vgrad(BY0, BY1, (196, 198, 206), (92, 94, 102), (150, 152, 160)))     # the rim (shows at the edge)
    front = shrink(body, 1.4)
    p.put(front, vgrad(BY0, BY1, (58, 58, 64), (14, 14, 17), (30, 30, 34)))
    # gloss: a soft band across the upper half of the front
    gl = mask(rr((BX0 + 8, BY0 + 3, BX1 - 8, BY0 + 12), 6)) * front
    p.put(gl, vgrad(BY0 + 3, BY0 + 12, (110, 110, 120), (40, 40, 46)), 0.45)
    # the screen (16:9) with a thin dark frame
    SX0, SY0, SX1, SY1 = 31, 45, 97, 83
    p.put(mask(rr((SX0 - 1.2, SY0 - 1.2, SX1 + 1.2, SY1 + 1.2), 2)), (6, 6, 8))
    scr = mask(rr((SX0, SY0, SX1, SY1), 1.2))
    p.put(scr, vgrad(SY0, SY1, (40, 52, 74), (12, 16, 26)))
    shine = mask(lambda d: d.polygon([(S(SX0 + 1), S(SY0 + 1)), (S(SX0 + 30), S(SY0 + 1)), (S(SX0 + 12), S(SY1 - 1)), (S(SX0 + 1), S(SY1 - 1))], fill=255)) * scr
    p.put(shine, (120, 140, 175), 0.18)
    # d-pad: four separate direction buttons in a round recess
    cx, cy = 17, 54
    p.put(mask(ellipse(cx, cy, 9.6)), radial(cx, cy - 2, 10, (8, 8, 10), (34, 34, 38)))
    for box in ((cx - 2.4, cy - 8.4, cx + 2.4, cy - 2.6), (cx - 2.4, cy + 2.6, cx + 2.4, cy + 8.4),
                (cx - 8.4, cy - 2.4, cx - 2.6, cy + 2.4), (cx + 2.6, cy - 2.4, cx + 8.4, cy + 2.4)):
        m = mask(rr(box, 1.0))
        p.put(grow(m, 0.6), (4, 4, 6))
        p.put(m, vgrad(box[1], box[3], (122, 122, 130), (46, 46, 52)))
    # face buttons
    fx, fy, sp, br = 111, 54, 6.6, 3.5
    for dx, dy, sym, col in ((0, -1, 'tri', (60, 200, 150)), (1, 0, 'cir', (235, 80, 80)), (0, 1, 'crs', (120, 160, 240)), (-1, 0, 'sqr', (225, 130, 205))):
        bx, by = fx + dx * sp, fy + dy * sp
        m = mask(ellipse(bx, by, br))
        p.put(grow(m, 0.7), (4, 4, 6))
        p.put(m, radial(bx - 1, by - 1.2, br * 1.6, (92, 92, 100), (22, 22, 26)))
        r, w = 1.9, int(round(0.9 * SS))
        def sym_draw(d, sym=sym, bx=bx, by=by, r=r, w=w):
            if sym == 'tri':
                d.polygon([(S(bx), S(by - r)), (S(bx + r), S(by + r * 0.75)), (S(bx - r), S(by + r * 0.75))], outline=255, width=w)
            elif sym == 'cir':
                d.ellipse((S(bx - r), S(by - r), S(bx + r), S(by + r)), outline=255, width=w)
            elif sym == 'crs':
                d.line([(S(bx - r), S(by - r)), (S(bx + r), S(by + r))], fill=255, width=w)
                d.line([(S(bx - r), S(by + r)), (S(bx + r), S(by - r))], fill=255, width=w)
            else:
                q = r * 0.85
                d.rectangle((S(bx - q), S(by - q), S(bx + q), S(by + q)), outline=255, width=w)
        p.put(mask(sym_draw), col)
    # the sticks: small round nubs near the screen's lower corners
    for sx, sy in ((23.5, 76), (104.5, 76)):
        p.put(mask(ellipse(sx, sy, 6.0)), (6, 6, 8))
        p.put(mask(ellipse(sx, sy, 5.0)), radial(sx - 1.2, sy - 1.6, 7, (128, 128, 136), (34, 34, 38)))
        p.put(mask(ellipse(sx, sy + 0.3, 3.0)), radial(sx + 0.6, sy + 1.0, 4, (40, 40, 44), (88, 88, 96)))
    img = p.image()
    # "L" and "R" above the shoulder buttons, white with a black outline (as on the PS2 picture)
    lab = Image.new('RGBA', (N * SS, N * SS), (0, 0, 0, 0))
    f = ImageFont.truetype(FONT, 11 * SS)
    d = ImageDraw.Draw(lab)
    for ch, x in (('L', 24), ('R', 104)):
        b = f.getbbox(ch)
        d.text((S(x) - (b[0] + b[2]) / 2, S(25) - (b[1] + b[3]) / 2), ch, font=f, fill=(255, 255, 255, 255),
               stroke_width=int(1.5 * SS), stroke_fill=(0, 0, 0, 255))
    img.alpha_composite(lab.resize((N, N), Image.LANCZOS))
    return img

if __name__ == '__main__':
    import sys
    im = make()
    im.save(sys.argv[1])
    bg = Image.new('RGBA', im.size, (40, 90, 160, 255)); bg.alpha_composite(im)
    bg.convert('RGB').resize((512, 512), Image.NEAREST).save(sys.argv[2])
