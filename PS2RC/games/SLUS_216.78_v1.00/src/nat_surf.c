/* nat_surf.c - BT3 (SLUS-216.78 v1.00): native fast paths of the stage surface effect (water). Written from the
 * game's own MIPS code as PS2RC translates it (work/trans), not from any decompilation source: the native reads and
 * writes what the translated body does, with the runtime's own FPU / VU0 models (r5900_rt.h), so the results are
 * bit-identical. ps2rc.cfg nat_check=1: each call also runs the translated body on the same input and logs every
 * difference (the translated body's result is kept).
 *
 * 0x139D78 (EftSurf_DrawTriOtClipped; args a0 = triangle (3 x 48-byte vertices: position, s t q, colour; the buffer
 * has room for 9), a1 = layer, a3 = flip z, t0 = TEX0, t1 = z bias): clips the triangle against the 5 planes of the
 * surface state (Sutherland-Hodgman, 0x121A10), projects the polygon with the VU0 current matrix (0x121D48) and
 * queues each fan triangle as a 128-byte GS packet in the ordering table at the first vertex's depth. */
#include <string.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"

void fn_00139d78__ynguest(R5900Ctx *c);

typedef struct { uint32_t w[12]; } SVtx;             /* position x y z w, s t q w, colour r g b a (float bits) */

static inline uint32_t fm(int op, uint32_t a, uint32_t s, uint32_t t) { return vu0_fmac1(op, a, s, t, 0, 0); }

static int surf_clip(R5900Ctx *c, SVtx *poly, const uint32_t *pl, int n, int *partial)
{
    *partial = 0;
    if (n == 0) return 0;
    uint32_t x3 = c->vu0.vf[3].w[0], d[16];
    int fl[16], nout = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t *p = poly[i].w;
        uint32_t m0 = fm(VU0_MUL, 0, pl[0], p[0]), m1 = fm(VU0_MUL, 0, pl[1], p[1]), m2 = fm(VU0_MUL, 0, pl[2], p[2]);
        uint32_t acc = fm(VU0_ADD, 0, m0, m1);
        acc = fm(VU0_MADD, acc, x3, m2);
        d[i] = fm(VU0_MADD, acc, x3, pl[3]);
        fl[i] = F(d[i]) < F(0);
        nout += fl[i];
    }
    if (nout == 0) return n;
    if (nout == n) return 0;
    SVtx out[16];                                     /* (more than 9: the original overflows the caller's
                                                         buffer the same way) */
    int k = 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        const SVtx *vi = &poly[i], *vj = &poly[j];
        if (k >= 15) break;
        if (!fl[i]) {
            out[k++] = *vi;
            if (!fl[j]) continue;
        } else if (fl[j]) continue;
        /* t = d_i / (plane . (v_i - v_j)); out = v_j t + v_i (1 - t), every lane of the three quadwords */
        uint32_t e0 = fm(VU0_SUB, 0, vi->w[0], vj->w[0]), e1 = fm(VU0_SUB, 0, vi->w[1], vj->w[1]);
        uint32_t e2 = fm(VU0_SUB, 0, vi->w[2], vj->w[2]);
        e0 = fm(VU0_MUL, 0, pl[0], e0); e1 = fm(VU0_MUL, 0, pl[1], e1); e2 = fm(VU0_MUL, 0, pl[2], e2);
        uint32_t den = fm(VU0_MADD, fm(VU0_ADD, 0, e0, e1), x3, e2);
        uint32_t t = fpu_div(d[i], den);
        uint32_t u = fm(VU0_SUB, 0, 0x3F800000u, t);
        for (int l = 0; l < 12; l++) out[k].w[l] = fm(VU0_MADD, fm(VU0_MUL, 0, vj->w[l], t), vi->w[l], u);
        k++;
    }
    memcpy(poly, out, (size_t)k * sizeof(SVtx));
    *partial = 1;
    return k;
}

static inline uint32_t ftoi(uint32_t raw, float scale)
{
    float f = u2f(vu0_clamp(raw)) * scale;
    return f >= 2147483647.0f ? 0x7FFFFFFFu : f <= -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;
}

static void surf_project(R5900Ctx *c, int32_t (*scr)[4], uint32_t (*stq)[4], const SVtx *poly, int n)
{
    const uint32_t *m16 = c->vu0.vf[16].w, *m17 = c->vu0.vf[17].w, *m18 = c->vu0.vf[18].w, *m19 = c->vu0.vf[19].w;
    uint32_t z1 = c->vu0.vf[1].w[2], w13 = c->vu0.vf[13].w[3];
    for (int i = 0; i < n; i++) {
        const uint32_t *p = poly[i].w, *s = poly[i].w + 4;
        uint32_t v[4];
        for (int l = 0; l < 4; l++) {
            uint32_t acc = fm(VU0_MUL, 0, m16[l], p[0]);
            acc = fm(VU0_MADD, acc, m17[l], p[1]);
            acc = fm(VU0_MADD, acc, m18[l], p[2]);
            v[l] = fm(VU0_MADD, acc, m19[l], p[3]);
        }
        uint32_t a = vu0_clamp(0x3F800000u), b = vu0_clamp(v[3]);
        uint32_t q = (b & 0x7FFFFFFFu) == 0 ? (((a ^ b) & 0x80000000u) | 0x7F7FFFFFu) : vu0_clamp(f2u(u2f(a) / u2f(b)));
        uint32_t st[3] = {s[0], s[1], z1};
        for (int l = 0; l < 3; l++) { v[l] = fm(VU0_MUL, 0, v[l], q); stq[i][l] = fm(VU0_MUL, 0, st[l], q); }
        stq[i][3] = w13;
        scr[i][0] = (int32_t)ftoi(v[0], 16.0f); scr[i][1] = (int32_t)ftoi(v[1], 16.0f);
        scr[i][2] = (int32_t)ftoi(v[2], 1.0f); scr[i][3] = (int32_t)ftoi(v[3], 1.0f);
    }
}

static inline uint8_t col8(uint32_t f)               /* float -> unsigned int, low byte (as cvt.w.s / 2^31 split) */
{
    if (F(0x4F000000u) <= F(f)) return (uint8_t)fpu_cvt_w(fpu_sub(f, 0x4F000000u));
    return (uint8_t)fpu_cvt_w(f);
}

/* the check mode's record of what the native wrote */
#define NC_MAX 240
static struct { uint32_t a, old, val; } nc_w[NC_MAX];   /* word writes outside the new packets */
static int nc_n;
static int nc_on;
static inline void nat_sw(uint32_t a, uint32_t v)
{
    if (nc_on && nc_n < NC_MAX) { nc_w[nc_n].a = a; nc_w[nc_n].old = gw(a); nc_w[nc_n].val = v; nc_n++; }
    sw_(a, v);
}

static void surf_tri_ot(R5900Ctx *c)
{
    uint32_t tri = (uint32_t)c->r[4].d[0], gp = (uint32_t)c->r[28].d[0];
    int32_t layer = (int32_t)c->r[5].d[0], flip = (int32_t)c->r[7].d[0], zbias = (int32_t)c->r[9].d[0];
    uint64_t tex0 = c->r[8].d[0];
    uint32_t planes = gw(gw(gp - 22700) + 16);
    SVtx poly[16];
    memcpy(poly, gptr(tri, 3 * sizeof(SVtx)), 3 * sizeof(SVtx));
    int n = 3, wn = 0;
    for (int k = 0; k < 5; k++) {
        uint32_t pl[4];
        int part;
        memcpy(pl, gptr(planes + 16 * (uint32_t)k, 16), 16);
        n = surf_clip(c, poly, pl, n, &part);
        if (part) wn = n;                             /* the translated body wrote this polygon back (0x1219D8) */
    }
    /* the caller's buffer holds the last partly clipped polygon (a later plane leaving nothing writes nothing) */
    if (wn) {
        if (nc_on) for (int i = 0; i < wn * 12; i++) nat_sw(tri + 4 * (uint32_t)i, ((const uint32_t *)poly)[i]);
        else gcopy_out(tri, poly, (uint32_t)wn * sizeof(SVtx));
    }
    if (n == 0) return;
    int32_t scr[16][4];
    uint32_t stq[16][4];
    surf_project(c, scr, stq, poly, n);
    const uint32_t thr = gw(gp - 32272);
    const uint32_t *col0 = poly[0].w + 8;
    for (int i = 2; i < n; i++) {
        int32_t z = scr[0][2] >> 8;
        if (flip) z = 0x1000 - z;
        if (scr[0][2] > 0xFFFFFF) scr[0][2] = 0xFFFFFF;
        if (scr[i - 1][2] > 0xFFFFFF) scr[i - 1][2] = 0xFFFFFF;
        if (scr[i][2] > 0xFFFFFF) scr[i][2] = 0xFFFFFF;
        const uint32_t *col1 = poly[i - 1].w + 8, *col2 = poly[i].w + 8;
        if (F(col0[3]) < F(thr) && F(col1[3]) < F(thr) && F(col2[3]) < F(thr)) continue;
        int abe = 1;
        int32_t lay = layer;
        if (lay < 0) { lay = 0; abe = 0; }
        int32_t l = lay >= 2 ? lay - 2 : lay;
        uint32_t pk = gw(gp - 22980);
        sw_(gp - 22980, pk + 128);
        uint8_t *b = gptr(pk, 128);
        uint32_t w;
        uint64_t d;
        w = 0x20000007u; memcpy(b + 0, &w, 4);
        w = 0; memcpy(b + 4, &w, 4);
        w = 0x10000000u; memcpy(b + 8, &w, 4);
        w = 0x50000007u; memcpy(b + 12, &w, 4);
        d = 0xC400000000008001ull; memcpy(b + 16, &d, 8);
        d = 0xF42142142160ull; memcpy(b + 24, &d, 8);
        d = ((uint64_t)abe << 6) | 27u; memcpy(b + 32, &d, 8);
        memcpy(b + 40, &tex0, 8);
        const uint32_t *cs[3] = {col0, col1, col2};
        const int vi[3] = {0, i - 1, i};
        for (int k = 0; k < 3; k++) {
            uint8_t *o = b + 48 + 24 * k;
            for (int ch = 0; ch < 4; ch++) o[ch] = col8(cs[k][ch]);
            memcpy(o + 4, &stq[vi[k]][2], 4);                       /* q */
            memcpy(o + 8, &stq[vi[k]][0], 4);                       /* s */
            memcpy(o + 12, &stq[vi[k]][1], 4);                      /* t */
            uint16_t x = (uint16_t)scr[vi[k]][0], y = (uint16_t)scr[vi[k]][1];
            uint8_t f8 = o[23];
            uint64_t xyz = (uint64_t)x | (uint64_t)y << 16 | (uint64_t)((uint32_t)scr[vi[k]][2] & 0xFFFFFFu) << 32;
            (void)f8;
            memcpy(o + 16, &xyz, 8);
            o[23] = 0xFF;
        }
        d = 0; memcpy(b + 120, &d, 8);
        int32_t zz = z - zbias;
        uint32_t base = gw(gp - 22972), e;
        if (zz < 0) e = base + ((uint32_t)l << 3);
        else if (zz < 4096) e = base + (((uint32_t)zz * 2 + (uint32_t)l) << 3);
        else e = base + ((uint32_t)l << 3) + 0xFFF0u;
        uint32_t tail = gw(e + 4);
        nat_sw(tail + 4, pk);
        nat_sw(e + 4, pk);
    }
}

void y_nat_00139d78(R5900Ctx *c)
{
    static int check = -1;
    if (check < 0) { const char *e = hle_cfg_get("nat_check"); check = e && atoi(e) ? 1 : 0; }
    if (!check) { surf_tri_ot(c); return; }
    /* check: native first (its writes recorded), undone, then the translated body; both results compared */
    uint32_t gp = (uint32_t)c->r[28].d[0], tri = (uint32_t)c->r[4].d[0];
    uint32_t cur0 = gw(gp - 22980);
    uint8_t tri0[9 * 48], natpk[16 * 128], nattri[9 * 48];
    memcpy(tri0, gptr(tri, sizeof tri0), sizeof tri0);
    nc_on = 1; nc_n = 0;
    surf_tri_ot(c);
    nc_on = 0;
    uint32_t cur1 = gw(gp - 22980), npk = (cur1 - cur0) / 128;
    if (npk > 16) npk = 16;
    memcpy(natpk, gptr(cur0, npk * 128 ? npk * 128 : 1), npk * 128);
    memcpy(nattri, gptr(tri, sizeof nattri), sizeof nattri);
    int nw = nc_n;
    uint32_t wa[NC_MAX], wv[NC_MAX];
    for (int i = nw - 1; i >= 0; i--) { wa[i] = nc_w[i].a; wv[i] = nc_w[i].val; sw_(nc_w[i].a, nc_w[i].old); }
    sw_(gp - 22980, cur0);
    memcpy(gptr(tri, sizeof tri0), tri0, sizeof tri0);
    fn_00139d78__ynguest(c);
    uint32_t cur2 = gw(gp - 22980);
    static int nbad, ncall;
    ncall++;
    int bad = cur2 != cur1;
    if (!bad && npk && memcmp(natpk, gptr(cur0, npk * 128), npk * 128)) bad = 2;
    if (!bad && memcmp(nattri, gptr(tri, sizeof nattri), sizeof nattri)) bad = 3;
    for (int i = 0; i < nw && !bad; i++) {           /* the last value written to each address */
        int last = 1;
        for (int j = i + 1; j < nw; j++) if (wa[j] == wa[i]) last = 0;
        if (last && gw(wa[i]) != wv[i]) bad = 4;
    }
    if (bad && nbad < 20) {
        nbad++;
        LOG("nat_surf: 0x139D78 call %d differs (%d): packets native %u translated %u\n", ncall, bad, npk,
            (cur2 - cur0) / 128);
        if (bad == 3) {
            const uint8_t *g = gptr(tri, sizeof nattri);
            int shown = 0;
            for (uint32_t o = 0; o < sizeof nattri && shown < 6; o += 4) {
                uint32_t a, b2, o0;
                memcpy(&a, nattri + o, 4); memcpy(&b2, g + o, 4); memcpy(&o0, tri0 + o, 4);
                if (a != b2) { shown++; LOG("nat_surf:   tri word %u: native %08x translated %08x before %08x\n", o / 4, a, b2, o0); }
            }
        }
        if (bad == 2)
            for (uint32_t o = 0; o < npk * 128; o += 4) {
                uint32_t a, b2;
                memcpy(&a, natpk + o, 4);
                memcpy(&b2, (uint8_t *)gptr(cur0, npk * 128) + o, 4);
                if (a != b2) LOG("nat_surf:   packet byte %u: native %08x translated %08x\n", o, a, b2);
            }
    }
    if (ncall % 2000 == 0) LOG("nat_surf: check 0x139D78: %d calls, %d differ\n", ncall, nbad);
}

/* 0x13A9D0 (EftSurf_DrawPolyOtClipped: the reflecting surface's triangle, two textures with fog): ps2rc.cfg
   surf_reflect (default 0) = 0 skips it: the reflection is not drawn (+5 % in a water stage); 1: the translated body runs */
void fn_0013a9d0__ynguest(R5900Ctx *c);
void y_nat_0013a9d0(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("surf_reflect"); on = e ? atoi(e) : 0; }   /* default 0: off (+5 % in the water stage) */
    if (on) fn_0013a9d0__ynguest(c);
}

/* 0x138BE0 (EftSurf_DrawMeshes): every visible mesh of the surface, each triangle's three vertices copied to the
   stack, their texture coordinates scrolled (or the colour faded by distance), then queued by one of the triangle
   writers. Native: the same steps and float operations as the translated body (the runtime's FPU input clamp f_in5
   where it applies it), the VU0 helpers 0x121950 / 0x122230 / 0x121F50 / 0x121FA8 inline (VU0 state written as they
   do); every other call goes to the translated function. ps2rc.cfg surf_native=0: the translated body. */
void fn_00138be0__ynguest(R5900Ctx *c);
void fn_002a9acc(R5900Ctx *c); void fn_00130ba8(R5900Ctx *c); void fn_00130b98(R5900Ctx *c);
void fn_00242668(R5900Ctx *c); void fn_00137f60(R5900Ctx *c); void fn_00139408(R5900Ctx *c);
void fn_00120ab0(R5900Ctx *c); void fn_00120b80(R5900Ctx *c); void fn_00138978(R5900Ctx *c);
void fn_00121ed8(R5900Ctx *c); void fn_00122088(R5900Ctx *c); void fn_0011f588(R5900Ctx *c);
void fn_00139c40(R5900Ctx *c); void fn_00139a40(R5900Ctx *c); void fn_001398d8(R5900Ctx *c);
void fn_001396f8(R5900Ctx *c); void fn_0013a9d0(R5900Ctx *c); void fn_00139d78(R5900Ctx *c);
void fn_001395e8(R5900Ctx *c); void fn_00120ac8(R5900Ctx *c);

#define DM_LW(a, o) rmb_lw(m_, (uint32_t)(a), (o))
#define DM_SW(a, o, v) rmb_sw(m_, (uint32_t)(a), (o), (uint32_t)(v))
#define DM_LD(a, o) rmb_ld(m_, (uint32_t)(a), (o))
#define DM_SD(a, o, v) rmb_sd(m_, (uint32_t)(a), (o), (uint64_t)(v))
#define DM_ARG(n, v) (c->r[n].d[0] = SX32((uint32_t)(v)))
#define DM_CALL(fn, ra) do { c->r[29].d[0] = SX32(sp); c->r[31].d[0] = SX32(ra); fn(c); } while (0)

static void dm_vcopy(R5900Mem m_, R5900Ctx *c, uint32_t d, uint32_t s0, uint32_t s1, uint32_t s2)   /* 0x121950 */
{
    rm_lq(m_, s0, &c->r[8]); rm_lq(m_, s1, &c->r[9]); rm_lq(m_, s2, &c->r[10]);
    rm_sq(m_, d, &c->r[8]); rm_sq(m_, d + 16, &c->r[9]); rm_sq(m_, d + 32, &c->r[10]);
}
static uint32_t dm_dist2(R5900Mem m_, R5900Ctx *c, uint32_t a, uint32_t b)                          /* 0x122230 */
{
    rm_lq(m_, a, &c->vu0.vf[4]);
    rm_lq(m_, b, &c->vu0.vf[5]);
    vu0_fmac(c, VU0_SUB, 4, 0xf, 4, 5, -1, 0, 1);
    vu0_fmac(c, VU0_MUL, 4, 0xe, 4, 4, -1, 3, 1);
    vu0_fmac(c, VU0_ADD, VU0_ACC, 0x8, 4, 4, 1, 3, 1);
    vu0_fmac(c, VU0_MADD, 4, 0x8, 3, 4, 2, 6, 1);
    c->r[8] = vu0_vf(c, 4);
    c->fpr[0].u = c->r[8].w[0];
    return c->r[8].w[0];
}
static void dm_scale(R5900Mem m_, R5900Ctx *c, uint32_t d, uint32_t s, uint32_t f12)                 /* 0x121F50 */
{
    R5900Reg128 q;
    rmb_lq(m_, s, 0, &q);
    c->r[8].d[0] = SX32(f12);
    uint32_t v5[4] = {c->r[8].w[0], c->r[8].w[1], c->r[8].w[2], c->r[8].w[3]};
    uint32_t x0 = vu0_fmac1(VU0_MUL, 0u, q.w[0], v5[0], 0, 1), x1 = vu0_fmac1(VU0_MUL, 0u, q.w[1], v5[0], 0, 1);
    uint32_t x2 = vu0_fmac1(VU0_MUL, 0u, q.w[2], v5[0], 0, 1);
    q.w[0] = x0; q.w[1] = x1; q.w[2] = x2;
    rmb_sq(m_, d, 0, &q);
    c->vu0.vf[4] = q;
    for (int l = 0; l < 4; l++) c->vu0.vf[5].w[l] = v5[l];
    c->fpr[12].u = f12;
}

static void surf_draw_meshes(R5900Ctx *c)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t sp0 = c->r[29].w[0], gp = c->r[28].w[0];
    const uint32_t sp = sp0 - 768;
    uint32_t s0 = DM_LW(gp, -22700), s1 = s0 + 576;
    uint32_t f1 = f_in5(0x3f800000u), f0 = f_in5(DM_LW(s1, 4280));
    uint32_t v0 = DM_LW(s0, 4);
    uint32_t f22 = f2u(u2f(f0) * u2f(f0));
    DM_SW(sp, 564, DM_LW(v0, 20));
    int32_t nmesh = (int32_t)DM_LW(v0, 24);
    uint32_t f27 = ((f22 & 0x7F800000u) == 0) ? (((f1 ^ f22) & 0x80000000u) | F_FMAX) : f2u(u2f(f1) / u2f(f22));
    uint32_t s5 = DM_LW(v0, 28);
    DM_ARG(4, sp + 464); DM_ARG(5, 0); DM_ARG(6, 16);
    DM_CALL(fn_002a9acc, 0x138c78);
    DM_CALL(fn_00130ba8, 0x138c80);
    DM_CALL(fn_00130b98, 0x138c88);
    DM_SW(DM_LW(gp, -22700), 16, c->r[2].w[0]);
    {   /* 0x121FA8: one quadword */
        R5900Reg128 q;
        rm_lq(m_, DM_LW(gp, -22176) + 544, &q);
        c->r[8] = q;
        rm_sq(m_, sp + 448, &q);
    }
    DM_ARG(4, sp + 560);
    DM_CALL(fn_00242668, 0x138cac);
    DM_SW(sp, 452, DM_LW(sp, 560));
    DM_CALL(fn_00137f60, 0x138cb8);
    uint32_t f26 = c->fpr[0].u;
    DM_ARG(4, 0); DM_ARG(5, 0);
    DM_CALL(fn_00139408, 0x138cc8);
    dm_scale(m_, c, sp + 544, s0 + 4800, f26);
    f0 = f_in5(DM_LW(s1, 4236));
    f0 = f2u(u2f(f0) * F5(0x3c000000u));
    DM_SW(sp, 556, f0);
    uint32_t a0 = DM_LW(s1, 4216);
    if (a0) {
        DM_SD(sp, 464, DM_LD(a0 + ((DM_LW(s1, 4360) + DM_LW(s1, 4368)) << 4), 0));
        DM_SD(sp, 472, DM_LD(DM_LW(s1, 4216) + ((DM_LW(s1, 4364) + DM_LW(s1, 4368)) << 4), 0));
    }
    DM_CALL(fn_00120ab0, 0x138d40);
    DM_ARG(4, DM_LW(gp, -22176) + 320);
    DM_CALL(fn_00120b80, 0x138d4c);
    for (int32_t mi = nmesh; mi > 0; mi--, s5 += 96) {
        DM_SW(sp, 568, mi);
        uint32_t s6 = DM_LW(s5, 32), f21 = DM_LW(s5, 20), f23 = DM_LW(s5, 24), f25 = 0x3f800000u;
        DM_SD(sp, 576, DM_LD((DM_LW(s5, 0) << 6) + DM_LW(sp, 564), 40));
        DM_SW(sp, 584, DM_LW(s5, 28));
        uint32_t s3 = DM_LW(s5, 8);
        int32_t ntri = (int32_t)DM_LW(s5, 4);
        if (s6 & 0x60) continue;
        DM_ARG(4, s5 + 48); DM_ARG(5, s5 + 64);
        DM_CALL(fn_00138978, 0x138da8);
        if (!c->r[2].d[0]) continue;
        uint32_t flag592 = 1;
        if (s6 & 0x200) {
            DM_ARG(4, sp + 432); DM_ARG(5, s5 + 80); c->r[6].w[0] = sp + 448;
            DM_CALL(fn_00121ed8, 0x138dcc);
            DM_ARG(4, sp + 432); DM_ARG(5, sp + 432);
            DM_CALL(fn_00122088, 0x138dd8);
            uint32_t fd = c->fpr[0].u;
            f0 = f_in5(DM_LW(gp, -32292));
            if (u2f(f0) < F5(fd)) continue;
            f0 = DM_LW(gp, -32288);
            if (F5(f0) < F5(fd)) {
                f0 = f2u(F5(fd) - F5(f0));
                uint32_t d1 = DM_LW(gp, -32284);
                f0 = ((d1 & 0x7F800000u) == 0) ? (((f0 ^ d1) & 0x80000000u) | F_FMAX) : f2u(u2f(f0) / F5(d1));
                f25 = f2u(F5(f25) - u2f(f0));
            }
        }
        if (!(s6 & 8)) flag592 = (s6 >> 3) & 2;
        DM_SW(sp, 592, flag592);
        if (ntri <= 0) continue;
        const uint32_t m608 = s6 & 0x80, m604 = s6 & 0xc00, m600 = s6 & 0x800, f20 = 0x3f800000u;
        const uint32_t f24 = DM_LW(gp, -32280);
        DM_SW(sp, 608, m608); DM_SW(sp, 604, m604); DM_SW(sp, 600, m600); DM_SW(sp, 596, sp + 448);
        for (int32_t ti = ntri; ti > 0; ti--, s3 += 144) {
            DM_SW(sp, 572, ti);
            DM_SW(sp, 480, dm_dist2(m_, c, s3, sp + 448));
            DM_SW(sp, 484, dm_dist2(m_, c, s3 + 48, sp + 448));
            DM_SW(sp, 488, dm_dist2(m_, c, s3 + 96, sp + 448));
            DM_SW(sp, 588, 1);
            for (uint32_t k = 0; k < 3; k++) {
                const uint32_t vs = s3 + 48 * k, vd = sp + 48 * k, q1 = sp + 20 + 48 * k, q2 = sp + 32 + 48 * k;
                dm_vcopy(m_, c, vd, vs, vs + 16, vs + 32);
                DM_SW(q1, -8, f20); DM_SW(q1, 4, f20); DM_SW(q1, 8, f20);
                if (m608) {
                    uint32_t g1 = f_in5(DM_LW(q1, 0)), g0 = f_in5(0x3f000000u);
                    int lo = u2f(g1) < u2f(g0);
                    c->fpr[12].u = f21;
                    DM_CALL(fn_0011f588, lo ? 0x138f04 : 0x138f20);
                    g0 = c->fpr[0].u;
                    g1 = f_in5(DM_LW(q1, -4));
                    g0 = f2u(F5(g0) * F5(f24));
                    g1 = lo ? f2u(u2f(g1) + u2f(g0)) : f2u(u2f(g1) - u2f(g0));
                    DM_SW(q1, -4, g1);
                    g0 = f_in5(DM_LW(q1, 0));
                    g0 = f2u(u2f(g0) + F5(f23));
                    DM_SW(q1, 0, g0);
                } else if (m600) {
                    R5900Reg128 q;
                    rm_lq(m_, sp + 544, &q);
                    c->r[8] = q;
                    rm_sq(m_, q2, &q);
                    uint32_t g0 = f_in5(DM_LW(q1, 24)), g1 = f_in5(DM_LW(vs, 44));
                    g0 = f2u(u2f(g0) * u2f(g1));
                    DM_SW(q1, 24, g0);
                    const uint32_t da = sp + 480 + 4 * k;
                    g1 = f_in5(DM_LW(da, 0));
                    if (F5(f22) < u2f(g1)) { DM_SW(da, 0, f22); g1 = f22; }
                    g0 = f_in5(DM_LW(gp, -32276));
                    if (u2f(g0) < F5(g1)) DM_SW(sp, 588, 0);
                    g0 = f2u(F5(g1) * F5(f27));
                    const uint32_t wa = sp + 496 + 12 + 16 * k;
                    g0 = f2u(F5(f20) - u2f(g0));
                    DM_SW(wa, 0, g0);
                    if (F5(f20) < u2f(g0)) DM_SW(wa, 0, f20);
                    else if (u2f(g0) < F5(0)) DM_SW(wa, 0, 0);
                } else {
                    uint32_t g0 = f_in5(DM_LW(q1, -4)), g1 = f_in5(DM_LW(q1, 0)), g2 = f_in5(DM_LW(q1, 24));
                    g0 = f2u(u2f(g0) + F5(f21));
                    g1 = f2u(u2f(g1) + F5(f23));
                    g2 = f2u(u2f(g2) * F5(f25));
                    DM_SW(q1, -4, g0); DM_SW(q1, 0, g1); DM_SW(q1, 24, g2);
                }
                dm_scale(m_, c, q2, q2, f26);
            }
            if (m604 == 0xc00) {
                DM_ARG(4, sp); DM_ARG(5, sp + 464); DM_ARG(6, sp + 496);
                if (DM_LW(sp, 588)) DM_CALL(fn_00139c40, 0x139058); else DM_CALL(fn_00139a40, 0x139074);
            } else if (s6 & 0x400) {
                DM_ARG(4, sp); c->r[5].d[0] = DM_LD(sp, 576);
                if (DM_LW(sp, 588)) DM_CALL(fn_001398d8, 0x139098); else DM_CALL(fn_001396f8, 0x1390a8);
            } else if (m600) {
                DM_ARG(4, sp); DM_ARG(5, 0); DM_ARG(6, sp + 464); DM_ARG(7, sp + 496); DM_ARG(8, DM_LW(sp, 584));
                DM_CALL(fn_0013a9d0, 0x1390d4);
            } else {
                DM_ARG(4, sp); DM_ARG(5, DM_LW(sp, 592)); DM_ARG(6, 1); DM_ARG(7, 0);
                c->r[8].d[0] = DM_LD(sp, 576); DM_ARG(9, DM_LW(sp, 584));
                DM_CALL(fn_00139d78, 0x1390fc);
            }
        }
    }
    DM_ARG(4, 0);
    DM_CALL(fn_001395e8, 0x13912c);
    DM_CALL(fn_00120ac8, 0x139134);
    c->r[29].d[0] = SX32(sp0);
}

void y_nat_00138be0(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("surf_native"); on = e ? atoi(e) : 1; }
    if (on) surf_draw_meshes(c); else fn_00138be0__ynguest(c);
}

/* 0x138978 (EftSurf_IsBoxVisible; a0 = box min, a1 = box max): the 8 corners projected with the VU0 current matrix
   (0x1210A8: VF16-VF19, divide by w, VFTOI0) and tested against the screen rectangle kept at [gp - 22176] + 512..524
   (offset by 1792 / 1824); visible at once when a corner is inside, else unless every corner is behind (z < 0) or the
   corners' off-screen sides leave fewer than two outcodes in common use (as the original's count). Native: the
   corners and the projected points stay in host memory (the translated body keeps them in its own stack frame,
   which nothing reads); the VU0 registers end as the translated body leaves them. ps2rc.cfg surf_native=0: off. */
void fn_00138978__ynguest(R5900Ctx *c);
static uint32_t surf_box_visible(R5900Ctx *c)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t a0 = c->r[4].w[0], a1 = c->r[5].w[0], gp = c->r[28].w[0];
    uint32_t lo[3], hi[3];
    for (int i = 0; i < 3; i++) { lo[i] = DM_LW(a0, 4 * i); hi[i] = DM_LW(a1, 4 * i); }
    const uint32_t scr = DM_LW(gp, -22176);
    const int32_t x0 = (int32_t)DM_LW(scr, 512), x1 = (int32_t)DM_LW(scr, 516);
    const int32_t y0 = (int32_t)DM_LW(scr, 520), y1 = (int32_t)DM_LW(scr, 524);
    uint32_t s2 = 0;
    for (int k = 0; k < 8; k++) {
        c->vu0.vf[4].w[0] = (k & 1) ? hi[0] : lo[0];
        c->vu0.vf[4].w[1] = (k & 2) ? hi[1] : lo[1];
        c->vu0.vf[4].w[2] = (k & 4) ? hi[2] : lo[2];
        c->vu0.vf[4].w[3] = 0x3f800000u;
        vu0_fmac(c, VU0_MUL, VU0_ACC, 0xf, 16, 4, 0, 0, 1);
        vu0_fmac(c, VU0_MADD, VU0_ACC, 0xf, 17, 4, 1, 4, 1);
        vu0_fmac(c, VU0_MADD, VU0_ACC, 0xf, 18, 4, 2, 4, 1);
        vu0_fmac(c, VU0_MADD, 5, 0xf, 19, 4, 3, 4, 1);
        {
            const uint32_t a_ = vu0_clamp(vu0_lane(c, 0, 3)), b_ = vu0_clamp(vu0_lane(c, 5, 3));
            c->vu0.vi[22] = (b_ & 0x7FFFFFFFu) == 0 ? (((a_ ^ b_) & 0x80000000u) | 0x7F7FFFFFu)
                                                    : vu0_clamp(f2u(u2f(a_) / u2f(b_)));
        }
        vu0_fmac(c, VU0_MUL, 5, 0xe, 5, VU0_QREG, 0, 1, 1);
        for (int l = 0; l < 4; l++) {
            float f_ = u2f(vu0_clamp(c->vu0.vf[5].w[l]));
            c->vu0.vf[6].w[l] = f_ >= 2147483647.0f ? 0x7FFFFFFFu : f_ <= -2147483648.0f ? 0x80000000u
                                                                                           : (uint32_t)(int32_t)f_;
        }
        const int32_t px = (int32_t)c->vu0.vf[6].w[0] - 1792, py = (int32_t)c->vu0.vf[6].w[1] - 1824;
        const int32_t pz = (int32_t)c->vu0.vf[6].w[2];
        uint32_t a2;
        if (pz < 0) a2 = 16;
        else {
            a2 = px < x0 ? 33 : 32;
            if (x1 + 1 < px) a2 |= 2;
            if (py < y0) a2 |= 4;
            if (y1 + 1 < py) a2 |= 8;
        }
        s2 |= a2;
        if (!(a2 & ~32u)) return 1;
    }
    uint32_t n = (s2 & 1) + ((s2 & 8) >> 3) + ((s2 & 4) >> 2) + ((s2 & 2) >> 1);
    if (n < 2) return (s2 & 0x30) == 0x30;
    return 1;
}
void y_nat_00138978(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("surf_native"); on = e ? atoi(e) : 1; }
    if (!on) { fn_00138978__ynguest(c); return; }
    c->r[2].d[0] = surf_box_visible(c);
}
