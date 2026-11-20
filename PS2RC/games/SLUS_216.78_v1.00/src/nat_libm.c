/* nat_libm.c - BT3 (SLUS-216.78 v1.00): native single-precision sin / acos / sqrt of the game's math library. Written
 * from the game's own MIPS code as PS2RC translates it (work/trans): the same float operations in the same order, the
 * same FPU input clamps (F5) where the translated body applies them, so the results are bit-identical. Only the paths
 * the battle uses are native; any other input runs the translated body.
 *   0x28F598 sinf: |x| <= 0x3F490FD8 -> kernel sin (0x2929B8); |x| <= 0x4016CBE3 -> one-step reduction by pi/2 (the
 *            first branch of 0x291B80), then the kernel sin / cos (0x292000) of the quadrant; larger -> translated.
 *   0x11F780 acos wrapper (clamp to [-1, 1]) + 0x2909F8 acosf: |x| < 0.5 polynomial, x >= 0.5 sqrt form, x = +-1;
 *            x <= -0.5 -> translated.
 *   0x291F30 sqrtf: the bit-by-bit integer square root.
 * ps2rc.cfg libm_native=0: the translated bodies; libm_check=1: each native call also runs the translated body and
 * logs the first differences (the translated result is kept). */
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"
#include "nat_libm.h"

void fn_0028f598__ynguest(R5900Ctx *c);
void fn_0011f780__ynguest(R5900Ctx *c);
void fn_00291f30__ynguest(R5900Ctx *c);

static int lm_on = -1, lm_chk;
static void lm_init(void)
{
    const char *e = hle_cfg_get("libm_native");
    lm_on = e ? atoi(e) : 1;
    e = hle_cfg_get("libm_check");
    lm_chk = e ? atoi(e) : 0;
}

static inline uint32_t fadd(uint32_t a, uint32_t b) { return f2u(u2f(a) + u2f(b)); }
static inline uint32_t fsub(uint32_t a, uint32_t b) { return f2u(u2f(a) - u2f(b)); }
static inline uint32_t fmul(uint32_t a, uint32_t b) { return f2u(u2f(a) * u2f(b)); }
static inline uint32_t fdiv(uint32_t a, uint32_t b)
{
    return ((b & 0x7F800000u) == 0) ? (((a ^ b) & 0x80000000u) | F_FMAX) : f2u(u2f(a) / u2f(b));
}

/* 0x2929B8 (f12 = x, f13 = y, a0 = iy) */
static uint32_t lm_ksin(uint32_t x, uint32_t y, int iy)
{
    const uint32_t ix = x & 0x7FFFFFFFu, xc = f_in5(x);
    if (ix <= 0x31FFFFFFu && fpu_cvt_w(x) == 0) return x;
    const uint32_t z = fmul(xc, xc);
    uint32_t r = fmul(z, 0x2F2EC9D2u);
    const uint32_t v = fmul(z, xc);
    r = fadd(r, 0xB2D72F34u);
    r = fadd(fmul(z, r), 0x3638EF1Au);
    r = fadd(fmul(z, r), 0xB9500D01u);
    r = fadd(fmul(z, r), 0x3C088889u);
    if (!iy) return fadd(xc, fmul(v, fadd(fmul(z, r), 0xBE2AAAAAu)));
    const uint32_t yc = f_in5(y);
    uint32_t a = fmul(yc, 0x3F000000u);
    const uint32_t b = fmul(v, r), d = fmul(v, 0xBE2AAAAAu);
    a = fmul(z, fsub(a, b));
    a = fsub(fsub(a, yc), d);
    return fsub(xc, a);
}

/* 0x292000 (f12 = x, f13 = y) */
static uint32_t lm_kcos(uint32_t x, uint32_t y)
{
    const uint32_t ix = x & 0x7FFFFFFFu, xc = f_in5(x);
    if (ix <= 0x31FFFFFFu && fpu_cvt_w(x) == 0) return 0x3F800000u;
    const uint32_t z = fmul(xc, xc);
    uint32_t r = fadd(fmul(z, 0xAD47D74Eu), 0x310F74F5u);
    r = fadd(fmul(z, r), 0xB493F27Bu);
    r = fadd(fmul(z, r), 0x37D00D00u);
    r = fadd(fmul(z, r), 0xBAB60B60u);
    r = fadd(fmul(z, r), 0x3D2AAAAAu);
    r = fmul(z, r);
    const uint32_t yc = f_in5(y);
    if (ix <= 0x3E999999u) {
        const uint32_t r2 = fsub(fmul(z, r), fmul(xc, yc));
        return fsub(0x3F800000u, fsub(fmul(z, 0x3F000000u), r2));
    }
    const uint32_t qx = (ix > 0x3F480000u) ? 0x3E900000u : f_in5(ix - 0x01000000u);
    const uint32_t hz = fmul(z, 0x3F000000u), a = fsub(0x3F800000u, qx);
    const uint32_t b = fsub(fmul(z, r), fmul(xc, yc));
    return fsub(a, fsub(fsub(hz, qx), b));
}

/* 0x28F598; *ok = 0 when x needs the translated body */
uint32_t nat_sinf_bits(uint32_t x, int *ok)
{
    const uint32_t ix = x & 0x7FFFFFFFu;
    *ok = 1;
    if (ix <= 0x3F490FD8u) return lm_ksin(x, 0, 0);
    if (ix > 0x4016CBE3u) { *ok = 0; return 0; }
    /* 0x291B80, |x| <= 3pi/4: one subtraction of pi/2 (pio2_1 + pio2_1t, or pio2_2 + pio2_2t near pi/2) */
    const uint32_t xc = f_in5(x);
    const int near = (ix & 0xFFFFFFF0u) == 0x3FC90FD0u;
    uint32_t z, y0, y1, t;
    if ((int32_t)x > 0) {
        z = fsub(xc, 0x3FC90F80u);
        if (!near) { t = 0x37354442u; y0 = fsub(z, t); }
        else { z = fsub(z, 0x373543FFu); t = 0x2E85A308u; y0 = fsub(z, t); }
        y1 = fsub(fsub(z, y0), t);
        return lm_kcos(y0, y1);                                   /* n = 1 */
    }
    z = fadd(xc, 0x3FC90F80u);
    if (!near) { t = 0x37354442u; y0 = fadd(z, t); }
    else { z = fadd(z, 0x373543FFu); t = 0x2E85A308u; y0 = fadd(z, t); }
    y1 = fadd(fsub(z, y0), t);
    return lm_kcos(y0, y1) ^ 0x80000000u;                         /* n = -1: quadrant 3 */
}

/* 0x291F30 */
uint32_t nat_sqrtf_bits(uint32_t x)
{
    const uint32_t ix = x & 0x7FFFFFFFu;
    if (ix <= 0x007FFFFFu) return x;
    if ((int32_t)x < 0) { const uint32_t d = fsub(f_in5(x), f_in5(x)); return fdiv(d, d); }
    int32_t m = (int32_t)(x >> 23) - 127;
    int32_t v = (int32_t)((x & 0x007FFFFFu) | 0x00800000u);
    v <<= (m & 1);
    m >>= 1;
    v <<= 1;
    int32_t s = 0, q = 0, r = 0x01000000;
    while (r) {
        const int32_t t = s + r;
        if (!(v < t)) { v -= t; s = t + r; q += r; }
        r >>= 1;
        v <<= 1;
    }
    if (v != 0) q += q & 1;
    return (uint32_t)(0x3F000000 + (q >> 1) + (int32_t)((uint32_t)m << 23));
}

/* 0x11F780 + 0x2909F8; *ok = 0 when x needs the translated body */
uint32_t nat_acosf_bits(uint32_t x, int *ok)
{
    *ok = 1;
    if (F5(x) < -1.0f) x = 0xBF800000u;
    else if (1.0f < F5(x)) x = 0x3F800000u;
    const uint32_t ix = x & 0x7FFFFFFFu, xc = f_in5(x);
    if (ix == 0x3F800000u) return ((int32_t)x > 0) ? 0 : 0x40490FDAu;
    if (ix > 0x3F800000u) { *ok = 0; return 0; }
    if (ix <= 0x3EFFFFFFu) {
        if (ix <= 0x23000000u) return 0x3FC90FDAu;
        const uint32_t z = fmul(xc, xc);
        uint32_t p = fadd(fmul(z, 0x3811EF08u), 0x3A4F7F04u);
        uint32_t q = fadd(fmul(z, 0x3D9DC62Du), 0xBF303360u);
        p = fadd(fmul(z, p), 0xBD241146u);
        q = fadd(fmul(z, q), 0x4001572Cu);
        p = fadd(fmul(z, p), 0x3E4E0AA8u);
        q = fadd(fmul(z, q), 0xC019D138u);
        p = fadd(fmul(z, p), 0xBEA6B08Fu);
        q = fadd(fmul(z, q), 0x3F800000u);
        p = fadd(fmul(z, p), 0x3E2AAAAAu);
        p = fmul(z, p);
        const uint32_t r = fdiv(p, q);
        return fsub(0x3FC90FDAu, fsub(xc, fsub(0x33A22168u, fmul(xc, r))));
    }
    if ((int32_t)x < 0) { *ok = 0; return 0; }
    const uint32_t z = fmul(fsub(0x3F800000u, xc), 0x3F000000u);
    const uint32_t s = nat_sqrtf_bits(z), sc = f_in5(s);
    const uint32_t df = f_in5(s & 0xFFFFF000u);
    const uint32_t cc = fdiv(fsub(z, fmul(df, df)), fadd(sc, df));
    uint32_t p = fadd(fmul(z, 0x3811EF08u), 0x3A4F7F04u);
    uint32_t q = fadd(fmul(z, 0x3D9DC62Du), 0xBF303360u);
    p = fadd(fmul(z, p), 0xBD241146u);
    q = fadd(fmul(z, q), 0x4001572Cu);
    p = fadd(fmul(z, p), 0x3E4E0AA8u);
    q = fadd(fmul(z, q), 0xC019D138u);
    p = fadd(fmul(z, p), 0xBEA6B08Fu);
    q = fadd(fmul(z, q), 0x3F800000u);
    p = fadd(fmul(z, p), 0x3E2AAAAAu);
    p = fmul(z, p);
    const uint32_t r = fdiv(p, q);
    const uint32_t w = fadd(fmul(r, sc), cc);
    const uint32_t h = fadd(df, w);
    return fadd(h, h);
}

static void lm_cmp(const char *what, uint32_t in, uint32_t nat, uint32_t ref)
{
    static int nbad, ncall;
    ncall++;
    if (nat != ref && nbad < 30) { nbad++; LOG("nat_libm: %s(%08x) native %08x translated %08x\n", what, in, nat, ref); }
    if (ncall % 200000 == 0) LOG("nat_libm: check %d calls, %d differ\n", ncall, nbad);
}

void y_nat_0028f598(R5900Ctx *c)
{
    if (lm_on < 0) lm_init();
    int ok = 0;
    const uint32_t x = c->fpr[12].u;
    const uint32_t r = lm_on ? nat_sinf_bits(x, &ok) : 0;
    if (!ok || lm_chk) {
        fn_0028f598__ynguest(c);
        if (ok) lm_cmp("sinf", x, r, c->fpr[0].u);
        return;
    }
    c->fpr[0].u = r;
}

void y_nat_0011f780(R5900Ctx *c)
{
    if (lm_on < 0) lm_init();
    int ok = 0;
    const uint32_t x = c->fpr[12].u;
    const uint32_t r = lm_on ? nat_acosf_bits(x, &ok) : 0;
    if (!ok || lm_chk) {
        fn_0011f780__ynguest(c);
        if (ok) lm_cmp("acosf", x, r, c->fpr[0].u);
        return;
    }
    c->fpr[0].u = r;
}

void y_nat_00291f30(R5900Ctx *c)
{
    if (lm_on < 0) lm_init();
    const uint32_t x = c->fpr[12].u;
    if (!lm_on || lm_chk) {
        fn_00291f30__ynguest(c);
        if (lm_on) lm_cmp("sqrtf", x, nat_sqrtf_bits(x), c->fpr[0].u);
        return;
    }
    c->fpr[0].u = nat_sqrtf_bits(x);
}
