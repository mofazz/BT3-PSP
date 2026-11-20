/* nat_clip.c - BT3 (SLUS-216.78 v1.00): native control flow of the effects' polygon clipper 0x121A10, written from
 * the game's MIPS as PS2RC translates it (work/trans). a0 = polygon (n vertices of 48 bytes, position first, clipped
 * in place), a1 = plane, a2 = n; returns the new vertex count (n when nothing is outside, 0 when all is).
 *   0x121C98: each vertex's signed distance to the plane (16-byte records in the frame at sp + 432);
 *   per edge (cur, next = cur + 1 wrapping): cur inside -> 0x121970 copies it out; when exactly one end is outside,
 *   0x121C68 gives the crossing parameter and 0x121C18 writes the crossing vertex (out records in the frame at sp);
 *   0x1219D8 copies the result back over the polygon.
 * The helpers are the hand-written VU0 routines (translated "exact"); called here with only their arguments set, the
 * translated body's save / reload of the whole register file around each of them goes away - the same helpers run
 * on the same memory, so the results and the VU0 / GPR state they leave are the translated body's.
 * ps2rc.cfg clip_native=0: the translated body. */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"

void fn_00121a10__ynguest(R5900Ctx *c);
void fn_00121c98(R5900Ctx *c);
void fn_00121970(R5900Ctx *c);
void fn_00121c68(R5900Ctx *c);
void fn_00121c18(R5900Ctx *c);
void fn_001219d8(R5900Ctx *c);

#define CL_ARG(n, v) (c->r[n].d[0] = SX32((uint32_t)(v)))

void y_nat_00121a10(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("clip_native"); on = e ? atoi(e) : 1; }
    if (!on) { fn_00121a10__ynguest(c); return; }
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t sp0 = c->r[29].w[0], sp = sp0 - 720;
    const uint32_t poly = c->r[4].w[0], plane = c->r[5].w[0];
    const int32_t n = (int32_t)c->r[6].w[0];
    const uint64_t ra = c->r[31].d[0];
    /* the callee-saved registers the original spills (its frame: the same bytes) */
    rmb_sd(m_, sp, 688, c->r[22].d[0]); rmb_sd(m_, sp, 696, c->r[23].d[0]);
    rmb_sd(m_, sp, 640, c->r[16].d[0]); rmb_sd(m_, sp, 648, c->r[17].d[0]);
    rmb_sd(m_, sp, 656, c->r[18].d[0]); rmb_sd(m_, sp, 664, c->r[19].d[0]);
    rmb_sd(m_, sp, 672, c->r[20].d[0]); rmb_sd(m_, sp, 680, c->r[21].d[0]);
    rmb_sd(m_, sp, 704, c->r[30].d[0]); rmb_sd(m_, sp, 712, ra);
    rmb_sw(m_, sp, 624, poly);
    c->r[29].d[0] = SX32(sp);
    uint32_t ret = 0;
    if (n != 0) {
        const uint32_t dist = sp + 432;
        CL_ARG(4, dist); CL_ARG(5, poly); CL_ARG(6, plane); CL_ARG(7, n); c->r[31].d[0] = 0x121a68;
        fn_00121c98(c);
        int32_t cnt = 0;
        int flag[16];
        int fc = 0;
        for (int32_t i = 0; i < n; i++) {   /* (polygons of up to 4 + 4 vertices: n stays small) */
            const uint32_t d = rmb_lw(m_, dist, 16 * i);
            fc = F5(d) < F5(0);
            const int f = fc ? 1 : 0;
            rmb_sw(m_, sp, 576 + 4 * i, (uint32_t)f);
            if (i < 16) flag[i] = f;
            cnt += f;
        }
        if (fc) c->fcr31 |= FCR31_C; else c->fcr31 &= ~FCR31_C;
        if (n > 16) { c->r[29].d[0] = SX32(sp0); fn_00121a10__ynguest(c); return; }   /* (never seen) */
        if (cnt == 0) ret = (uint32_t)n;
        else if (cnt == n) ret = 0;
        else {
            uint32_t out = sp;
            for (int32_t i = 0; i < n; i++) {
                const int32_t j = i + 1 < n ? i + 1 : 0;
                const uint32_t cur = poly + 48 * (uint32_t)i, nxt = poly + 48 * (uint32_t)j;
                const uint32_t dcur = rmb_lw(m_, dist, 16 * i);
                if (!flag[i]) {
                    CL_ARG(4, out); CL_ARG(5, cur); c->r[31].d[0] = 0x121b24;
                    fn_00121970(c);
                    out += 48;
                    if (!flag[j]) continue;
                } else if (flag[j]) continue;
                CL_ARG(4, plane); CL_ARG(5, cur); CL_ARG(6, nxt); c->fpr[12].u = dcur;
                c->r[31].d[0] = flag[i] ? 0x121b90 : 0x121b4c;
                fn_00121c68(c);
                CL_ARG(4, out); CL_ARG(5, nxt); CL_ARG(6, cur); c->fpr[12].u = c->fpr[0].u;
                c->r[31].d[0] = flag[i] ? 0x121ba8 : 0x121b64;
                fn_00121c18(c);
                out += 48;
            }
            const uint32_t k = (out - sp) / 48u;
            c->lo.d[0] = SX32(k); c->hi.d[0] = SX32((out - sp) % 48u);
            CL_ARG(4, poly); CL_ARG(5, sp); CL_ARG(6, k); c->r[31].d[0] = 0x121be4;
            fn_001219d8(c);
            ret = k;
        }
    }
    c->r[2].d[0] = SX32(ret);
    c->r[31].d[0] = ra;
    c->r[29].d[0] = SX32(sp0);
}

/* 0x121C98 (a0 = out, 16-byte records; a1 = vertices, 48-byte stride; a2 = plane; a3 = n): VF4 = plane, then per
   vertex VF5.xyz = VF4 * VF5, ACC.x = x + y, ACC.x += VF3.x * z, VF6.x = ACC.x + VF3.x * VF4.w, stored (VF6.yzw as
   they were); VF5 is loaded one record ahead. The same FMAC lanes with the VU0 registers held in locals. */
void fn_00121c98__ynguest(R5900Ctx *c);
void y_nat_00121c98(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("clipvu_native"); on = e ? atoi(e) : 1; }
    if (!on) { fn_00121c98__ynguest(c); return; }
    const R5900Mem m_ = {c->ram, c->ram_mask};
    uint64_t r4 = c->r[4].d[0], r5 = c->r[5].d[0], r7 = c->r[7].d[0];
    const uint32_t r6 = c->r[6].w[0];
    if (r7 == 0) return;
    R5900Reg128 L, P, D = c->vu0.vf[6];
    rm_lq(m_, r6, &L);
    rm_lq(m_, (uint32_t)r5, &P);
    const uint32_t k3 = c->vu0.vf[3].w[0];
    uint32_t a0 = c->vu0.acc.w[0];
    do {
        const uint32_t x = vu0_fmac1(VU0_MUL, 0u, L.w[0], P.w[0], 0, 1);
        const uint32_t y = vu0_fmac1(VU0_MUL, 0u, L.w[1], P.w[1], 0, 1);
        const uint32_t z = vu0_fmac1(VU0_MUL, 0u, L.w[2], P.w[2], 0, 1);
        P.w[0] = x; P.w[1] = y; P.w[2] = z;
        a0 = vu0_fmac1(VU0_ADD, 0u, x, y, 3, 1);
        a0 = vu0_fmac1(VU0_MADD, a0, k3, z, 6, 1);
        D.w[0] = vu0_fmac1(VU0_MADD, a0, k3, L.w[3], 4, 1);
        r5 = SX32((uint32_t)r5 + 48u);
        rm_lq(m_, (uint32_t)r5, &P);
        rm_sq(m_, (uint32_t)r4, &D);
        r7 = SX32((uint32_t)r7 - 1u);
        r4 = SX32((uint32_t)r4 + 16u);
    } while (r7 != 0);
    c->vu0.vf[4] = L; c->vu0.vf[5] = P; c->vu0.vf[6] = D; c->vu0.acc.w[0] = a0;
    c->r[4].d[0] = r4; c->r[5].d[0] = r5; c->r[7].d[0] = r7;
}

/* 0x121C18 (a0 = out, a1 = B, a2 = A, f12 = t): VF13 = r8 with its low word t (r8 = t sign-extended), VF14.w =
   1 - t, then for the three quadwords out = B * t + A * (1 - t) (VMULA x, VMADD w); VF4..VF12, VF13, VF14.w, ACC as
   the translated body leaves them. */
void fn_00121c18__ynguest(R5900Ctx *c);
void y_nat_00121c18(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("clipvu_native"); on = e ? atoi(e) : 1; }
    if (!on) { fn_00121c18__ynguest(c); return; }
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t r4 = c->r[4].w[0], r5 = c->r[5].w[0], r6 = c->r[6].w[0];
    c->r[8].d[0] = SX32(c->fpr[12].u);
    const R5900Reg128 v13 = c->r[8];
    c->vu0.vf[13] = v13;
    const uint32_t t = v13.w[0];
    const uint32_t omt = vu0_fmac1(VU0_SUB, 0u, 0x3F800000u, t, 1, 1);
    c->vu0.vf[14].w[3] = omt;
    R5900Reg128 B[3], A[3], O[3], acc;
    _Pragma("GCC unroll 3")
    for (int k = 0; k < 3; k++) { rm_lq(m_, r5 + 16 * k, &B[k]); rm_lq(m_, r6 + 16 * k, &A[k]); }
    _Pragma("GCC unroll 3")
    for (int k = 0; k < 3; k++)
        _Pragma("GCC unroll 4")
        for (int l = 0; l < 4; l++) {
            acc.w[l] = vu0_fmac1(VU0_MUL, 0u, B[k].w[l], t, 0, 1);
            O[k].w[l] = vu0_fmac1(VU0_MADD, acc.w[l], A[k].w[l], omt, 6, 1);
        }
    _Pragma("GCC unroll 3")
    for (int k = 0; k < 3; k++) {
        c->vu0.vf[4 + 2 * k] = B[k]; c->vu0.vf[5 + 2 * k] = A[k]; c->vu0.vf[10 + k] = O[k];
        rm_sq(m_, r4 + 16 * k, &O[k]);
    }
    c->vu0.acc = acc;
}

/* 0x130BA8 (the effects' view-frustum planes: five planes at 0x31BE10 from the field of view at gp - 32532, the
   camera matrix at [gp - 22176] + 64 and gp - 32528; sin / cos / 0x28F130, the inverse view (0x1202A0), each plane
   through it (0x121FD0, 0x122088)). The quad builder 0x1AC650 calls it for every polygon: the result is a pure
   function of those inputs, so the last call's result is replayed while they are unchanged - the planes, and every
   register word the call changed set to the value the call left (the same values the call computes again).
   ps2rc.cfg frustum_memo=0: off; frustum_memo=2: each replay also runs the call and compares the whole state. */
void fn_00130ba8__ynguest(R5900Ctx *c);
#define FM_W0 ((sizeof(R5900Reg128) * 34 + 4 + 4 * 33 + 4) / 4)        /* r[32], hi, lo, sa, fpr[32], acc, fcr31 */
#define FM_WV (sizeof(R5900VU0) / 4)
#define FM_N (FM_W0 + FM_WV)
static void fm_snap(const R5900Ctx *c, uint32_t *w)
{
    memcpy(w, &c->r[0], FM_W0 * 4);
    memcpy(w + FM_W0, &c->vu0, FM_WV * 4);
}
static void fm_put(R5900Ctx *c, uint32_t i, uint32_t v)
{
    if (i < FM_W0) ((uint32_t *)&c->r[0])[i] = v; else ((uint32_t *)&c->vu0)[i - FM_W0] = v;
}
void y_nat_00130ba8(R5900Ctx *c)
{
    static int on = -1;
    static uint32_t key[20], kvalid, planes[20], s0[FM_N], s1[FM_N], s2[FM_N];
    static uint16_t chg[FM_N];
    static uint32_t nchg, hits, miss, bad;
    if (on < 0) { const char *e = hle_cfg_get("frustum_memo"); on = e ? atoi(e) : 1; }
    if (!on) { fn_00130ba8__ynguest(c); return; }
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t gp = c->r[28].w[0], pl = 0x31BE10u;
    uint32_t k[20];
    k[0] = rmb_lw(m_, gp, -32532); k[1] = rmb_lw(m_, gp, -32528); k[2] = c->r[29].w[0];
    const uint32_t cam = rmb_lw(m_, gp, -22176) + 64;
    for (int i = 0; i < 16; i++) k[3 + i] = rmb_lw(m_, cam, 4 * i);
    k[19] = c->vu0.vf[3].w[0];                          /* (0x122088's constant) */
    if (kvalid && !memcmp(k, key, sizeof k)) {
        hits++;
        if (on == 2) {                                   /* check: the replay against the call itself */
            static uint32_t pr[FM_N], rl[FM_N];
            fm_snap(c, pr);
            for (uint32_t j = 0; j < nchg; j++) pr[chg[j]] = s1[chg[j]];
            fn_00130ba8__ynguest(c);
            fm_snap(c, rl);
            int d = memcmp(pr, rl, sizeof pr) != 0;
            for (int i = 0; i < 20; i++) d |= rmb_lw(m_, pl, 4 * i) != planes[i];
            if (d && bad++ < 10) {
                uint32_t i0 = 0; while (i0 < FM_N && pr[i0] == rl[i0]) i0++;
                LOG("nat_clip: frustum memo differs (call %u, first word %u: %08x vs %08x)\n", hits, i0,
                    i0 < FM_N ? pr[i0] : 0, i0 < FM_N ? rl[i0] : 0);
                for (uint32_t i = 0; i < FM_N; i++)
                    if (pr[i] != rl[i]) LOG("  word %u: replay %08x call %08x (miss: %08x -> %08x)\n", i, pr[i], rl[i], s0[i], s1[i]);
            }
            if (hits % 20000 == 0) LOG("nat_clip: frustum memo check %u replays, %u differ\n", hits, bad);
            return;
        }
        for (int i = 0; i < 20; i++) rmb_sw(m_, pl, 4 * i, planes[i]);
        for (uint32_t j = 0; j < nchg; j++) fm_put(c, chg[j], s1[chg[j]]);
        return;
    }
    miss++;
    fm_snap(c, s0);
    fn_00130ba8__ynguest(c);
    fm_snap(c, s1);
    /* which words the call writes: run it again from the entry state with every scratch word inverted (GPRs
       but gp / sp, HI / LO, FPRs 0 - 19, the C bit, VF4 - VF31, ACC); a word the call writes comes out the same.
       Words left alone (sp, gp, callee-saved, VF0 - VF3, ...) count when the call changed them. */
    {
        static uint32_t pert[FM_N];
        static int pinit;
        if (!pinit) {
            pinit = 1;
            for (int r = 1; r < 32; r++) if (r != 28 && r != 29) for (int w = 0; w < 4; w++) pert[r * 4 + w] = ~0u;
            for (int w = 0; w < 8; w++) pert[128 + w] = ~0u;                              /* hi, lo */
            for (int f = 0; f < 20; f++) pert[137 + f] = ~0u;                          /* fpr 0..19 */
            pert[FM_W0 - 1] = FCR31_C;                                                 /* fcr31: the C bit */
            const uint32_t vf0 = (uint32_t)(((const uint8_t *)&c->vu0.vf[0] - (const uint8_t *)&c->vu0) / 4);
            for (int v = 4; v < 32; v++) for (int w = 0; w < 4; w++) pert[FM_W0 + vf0 + v * 4 + w] = ~0u;
            const uint32_t ac = (uint32_t)(((const uint8_t *)&c->vu0.acc - (const uint8_t *)&c->vu0) / 4);
            for (int w = 0; w < 4; w++) pert[FM_W0 + ac + w] = ~0u;
        }
        for (uint32_t i = 0; i < FM_N; i++) fm_put(c, i, s0[i] ^ pert[i]);
        fn_00130ba8__ynguest(c);
        fm_snap(c, s2);
        nchg = 0;
        for (uint32_t i = 0; i < FM_N; i++)
            if (pert[i] ? s2[i] == s1[i] : s0[i] != s1[i]) chg[nchg++] = (uint16_t)i;
        for (uint32_t i = 0; i < FM_N; i++) fm_put(c, i, s1[i]);                        /* the state of one call */
    }
    for (int i = 0; i < 20; i++) planes[i] = rmb_lw(m_, pl, 4 * i);
    memcpy(key, k, sizeof k);
    kvalid = 1;
}
