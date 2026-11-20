/* nat_anim.c - BT3 (SLUS-216.78 v1.00): native fast paths of the fighters' animation sampling. Written from the
 * game's own MIPS code as PS2RC translates it (work/trans), not from any decompilation source: the same float
 * operations in the same order with the runtime's FPU input clamp (F5 / f_in5) where the translated body applies it,
 * so the results are bit-identical. ps2rc.cfg anim_native=0: the translated bodies; anim_check=1: each call also runs
 * the translated body on the same input and logs any difference (the translated result is kept).
 *
 * 0x256E00 (Quat_Unpack; a0 = out quaternion, a1 = 64 bits: three 20-bit components, the index of the omitted one in
 * the top 4 bits): each component (n * k1 - 0.5) * k2 (k1, k2 at gp - 23280 / - 23276), the omitted one
 * sqrt(1 - sum of squares) (0x28F770 when the root is NaN), put back at its index.
 * 0x2568F8 (Quat_Slerp; a0 = out, a1 = q1, a2 = q2, f12 = t): q2 negated when the dot product is negative, the dot
 * clamped to 1, theta = acos (0x11F780), sin theta (0x28F598); below the epsilon at gp - 23300 out = q1, else
 * q1 sin((1 - t) theta) / sin theta + q2 sin(t theta) / sin theta. */
#include <string.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"
#include "nat_libm.h"

void fn_00256e00__ynguest(R5900Ctx *c);
void fn_002568f8__ynguest(R5900Ctx *c);
void fn_0028f770(R5900Ctx *c);
void fn_0011f780(R5900Ctx *c);
void fn_0028f598(R5900Ctx *c);

static int anim_opt(const char *k, int def)
{
    const char *e = hle_cfg_get(k);
    return e ? atoi(e) : def;
}
/* a translated float function f(f12) -> f0, with a stack frame below the caller's */
static uint32_t anim_call_f(R5900Ctx *c, void (*fn)(R5900Ctx *), uint32_t f12, uint32_t sp, uint32_t ra)
{
    R5900CSave cs_;
    c->fpr[12].u = f12;
    c->r[29].d[0] = SX32(sp);
    c->r[31].d[0] = SX32(ra);
    rt_cs_save(c, &cs_);
    fn(c);
    rt_cs_restore(c, &cs_);
    return c->fpr[0].u;
}

/* sin / acos natively (nat_libm.c), the translated function for the inputs it does not cover */
static int anim_lm = -1;
static uint32_t anim_sin(R5900Ctx *c, uint32_t x, uint32_t sp, uint32_t ra)
{
    int ok = 0;
    uint32_t r = anim_lm ? nat_sinf_bits(x, &ok) : 0;
    return ok ? r : anim_call_f(c, fn_0028f598, x, sp, ra);
}
static uint32_t anim_acos(R5900Ctx *c, uint32_t x, uint32_t sp, uint32_t ra)
{
    int ok = 0;
    uint32_t r = anim_lm ? nat_acosf_bits(x, &ok) : 0;
    return ok ? r : anim_call_f(c, fn_0011f780, x, sp, ra);
}

static void quat_unpack(R5900Ctx *c, uint32_t q[4])
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t gp = c->r[28].w[0], sp0 = c->r[29].w[0];
    const uint64_t a1 = c->r[5].d[0];
    const uint32_t f3 = rmb_lw(m_, gp, -23280), f1k = rmb_lw(m_, gp, -23276), f2 = 0x3f000000u;
    uint32_t t[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; i++) {
        uint32_t r2 = (uint32_t)(a1 >> (20 * i)) & 0xFFFFFu;
        uint32_t f12 = fpu_cvt_s(r2);
        uint32_t f0 = f2u(u2f(f12) * F5(f3));
        f0 = f2u(u2f(f0) - F5(f2));
        f0 = f2u(u2f(f0) * F5(f1k));
        t[i] = f0;
    }
    uint32_t f12 = 0;
    for (int i = 0; i < 3; i++) { uint32_t f0 = f2u(F5(t[i]) * F5(t[i])); f12 = f2u(F5(f12) + u2f(f0)); }
    f12 = f2u(F5(0x3f800000u) - u2f(f12));
    uint32_t r = fpu_sqrt_c(f12);
    if (u2f(r) == u2f(r)) c->fcr31 |= FCR31_C;
    else {
        c->fcr31 &= ~FCR31_C;
        r = anim_call_f(c, fn_0028f770, f12, sp0 - 48, 0x256ef8);
        c->r[29].d[0] = SX32(sp0);
    }
    uint32_t k = (uint32_t)(a1 >> 60);
    for (int j = 3; j > (int)k; j--) t[j] = t[j - 1];
    if (k < 4) t[k] = r;
    memcpy(q, t, 16);
    c->fpr[1].u = r;
}

static void quat_slerp(R5900Ctx *c, uint32_t out[4])
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t gp = c->r[28].w[0], sp0 = c->r[29].w[0], sp = sp0 - 96;
    const uint32_t f23 = c->fpr[12].u;
    uint32_t s[4], u[4];
    for (int i = 0; i < 4; i++) { s[i] = rmb_lw(m_, c->r[5].w[0], 4 * i); u[i] = rmb_lw(m_, c->r[6].w[0], 4 * i); }
    uint32_t f0 = f2u(F5(s[0]) * F5(u[0])), f3 = f2u(F5(s[1]) * F5(u[1])), f2 = f2u(F5(s[2]) * F5(u[2]));
    f0 = f2u(u2f(f0) + u2f(f3));
    uint32_t f1 = f2u(F5(s[3]) * F5(u[3]));
    f0 = f2u(u2f(f0) + u2f(f2));
    f0 = f2u(u2f(f0) + u2f(f1));
    if (u2f(f0) < F5(0)) {
        for (int i = 0; i < 4; i++) u[i] ^= 0x80000000u;
        f0 = f2u(F5(s[0]) * F5(u[0]));
        f1 = f2u(F5(s[1]) * F5(u[1]));
        f3 = f2u(F5(s[2]) * F5(u[2]));
        f2 = f2u(F5(s[3]) * F5(u[3]));
        f0 = f2u(u2f(f0) + u2f(f1));
        f0 = f2u(u2f(f0) + u2f(f3));
        f0 = f2u(u2f(f0) + u2f(f2));
    }
    const uint32_t f22 = 0x3f800000u;
    if (F5(f22) < u2f(f0)) f0 = f22;
    uint32_t f20 = anim_acos(c, f0, sp, 0x2569dc);
    uint32_t f21 = anim_sin(c, f20, sp, 0x2569e8);
    c->r[29].d[0] = SX32(sp0);
    const uint32_t eps = rmb_lw(m_, gp, -23300);
    if (F5(f21) < F5(eps)) { memcpy(out, s, 16); return; }
    uint32_t f12 = f2u(F5(f22) - F5(f23));
    f21 = ((f21 & 0x7F800000u) == 0) ? (((f22 ^ f21) & 0x80000000u) | F_FMAX) : f2u(F5(f22) / F5(f21));
    uint32_t sa = anim_sin(c, f2u(F5(f20) * u2f(f12)), sp, 0x256a34);
    uint32_t wa = f2u(F5(sa) * F5(f21));
    uint32_t sb = anim_sin(c, f2u(F5(f20) * F5(f23)), sp, 0x256a40);
    c->r[29].d[0] = SX32(sp0);
    uint32_t wb = f2u(F5(sb) * F5(f21));
    for (int i = 0; i < 4; i++) {
        uint32_t x = f2u(F5(s[i]) * F5(wa)), y = f2u(F5(u[i]) * u2f(wb));
        out[i] = f2u(u2f(x) + u2f(y));
    }
}

static void anim_store(R5900Ctx *c, uint32_t dst, const uint32_t q[4])
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    for (int i = 0; i < 4; i++) rmb_sw(m_, dst, 4 * i, q[i]);
}

void y_nat_00256e00(R5900Ctx *c)
{
    static int on = -1, chk = -1;
    if (on < 0) { on = anim_opt("anim_native", 1); chk = anim_opt("anim_check", 0); }
    if (!on) { fn_00256e00__ynguest(c); return; }
    uint32_t q[4];
    const uint32_t dst = c->r[4].w[0];
    if (chk) {
        R5900Ctx save = *c;
        quat_unpack(c, q);
        *c = save;
        fn_00256e00__ynguest(c);
        const R5900Mem m_ = {c->ram, c->ram_mask};
        static int nbad, ncall;
        ncall++;
        for (int i = 0; i < 4; i++)
            if (rmb_lw(m_, dst, 4 * i) != q[i] && nbad < 20) {
                nbad++;
                LOG("nat_anim: unpack call %d word %d native %08x translated %08x\n", ncall, i, q[i], rmb_lw(m_, dst, 4 * i));
            }
        return;
    }
    quat_unpack(c, q);
    anim_store(c, dst, q);
}

void y_nat_002568f8(R5900Ctx *c)
{
    static int on = -1, chk = -1;
    if (on < 0) { on = anim_opt("anim_native", 1); chk = anim_opt("anim_check", 0); anim_lm = anim_opt("libm_native", 1); }
    if (!on) { fn_002568f8__ynguest(c); return; }
    uint32_t q[4];
    const uint32_t dst = c->r[4].w[0];
    if (chk) {
        R5900Ctx save = *c;
        quat_slerp(c, q);
        *c = save;
        fn_002568f8__ynguest(c);
        const R5900Mem m_ = {c->ram, c->ram_mask};
        static int nbad, ncall;
        ncall++;
        for (int i = 0; i < 4; i++)
            if (rmb_lw(m_, dst, 4 * i) != q[i] && nbad < 20) {
                nbad++;
                LOG("nat_anim: slerp call %d word %d native %08x translated %08x\n", ncall, i, q[i], rmb_lw(m_, dst, 4 * i));
            }
        if (ncall % 20000 == 0) LOG("nat_anim: check %d slerp calls, %d differ\n", ncall, nbad);
        return;
    }
    quat_slerp(c, q);
    anim_store(c, dst, q);
}
