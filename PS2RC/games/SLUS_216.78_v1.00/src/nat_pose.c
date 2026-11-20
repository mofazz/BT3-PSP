/* nat_pose.c - BT3 (SLUS-216.78 v1.00): native fast paths of the fighters' skeleton update. Written from the
 * game's own MIPS code as PS2RC translates it (work/trans), not from any decompilation source. The natives keep the
 * original's control flow and call the same helpers (the VU0 matrix-stack and matrix routines are hand-written
 * assembly translated "exact": every call from translated code saves and reloads the whole register file - calling
 * them straight from C with only their arguments set is where the time goes away). ps2rc.cfg pose_native=0: the
 * translated bodies.
 *
 * 0x24E3F8 (BtlObjPose_CalcMatrices; a0 = object): for each node of the object's hierarchy list (+68, records
 * linked by their size at +0; +10 node index, +4 matrix-stack pops, +6 last record) the parent's matrix is pushed,
 * the node's local matrix built (0x24E100) with its translation (+144) and multiplied into its world matrix (+16),
 * which becomes the current one. */
#include <string.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"

void fn_0024e3f8__ynguest(R5900Ctx *c);
void fn_00120b18(R5900Ctx *c); void fn_00120b80(R5900Ctx *c); void fn_00120ab0(R5900Ctx *c);
void fn_00120b98(R5900Ctx *c); void fn_0024e100(R5900Ctx *c); void fn_001201b8(R5900Ctx *c);
void fn_00120ae0(R5900Ctx *c);

#define PO_LW(a, o) rmb_lw(m_, (uint32_t)(a), (o))
#define PO_LHU(a, o) ((uint32_t)rmb_lh(m_, (uint32_t)(a), (o)))
#define PO_ARG(n, v) (c->r[n].d[0] = SX32((uint32_t)(v)))
#define PO_CALL(fn, ra) do { c->r[29].d[0] = SX32(sp); c->r[31].d[0] = SX32(ra); fn(c); } while (0)

/* 0x1201B8 (VU0 matrix multiply; a0 = out, a1 = A, a2 = B): out row j = sum_k A row k * B[j][k] (VMULAx, VMADDAy,
   VMADDAz, VMADDw per row), A left in VF4..7, the rows in VF8..11, ACC = the last row's sum before its w term - the
   VU0 state the translated body leaves */
static int po_mm = -1;
void fn_001201b8__ynguest(R5900Ctx *c);
static void pose_matmul(R5900Ctx *c, uint32_t out, uint32_t pa, uint32_t pb)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    R5900Reg128 A[4], B[4];
    _Pragma("GCC unroll 16")
    for (int k = 0; k < 4; k++) { rm_lq(m_, pa + 16 * k, &A[k]); rm_lq(m_, pb + 16 * k, &B[k]); }
    float a[4][4], b[4][4];                          /* operands clamped once: the same values each FMAC sees */
    _Pragma("GCC unroll 16")
    for (int k = 0; k < 4; k++)
        _Pragma("GCC unroll 4")
        for (int l = 0; l < 4; l++) { a[k][l] = u2f(vu0_clamp(A[k].w[l])); b[k][l] = u2f(vu0_clamp(B[k].w[l])); }
    float ac[4] = {0, 0, 0, 0};
    _Pragma("GCC unroll 4")
    for (int j = 0; j < 4; j++) {
        const float b0 = b[j][0], b1 = b[j][1], b2 = b[j][2], b3 = b[j][3];
        _Pragma("GCC unroll 4")
        for (int l = 0; l < 4; l++) {
            float t = a[0][l] * b0;                  /* VMULAx */
            t = t + a[1][l] * b1;                    /* VMADDAy: product rounded, then the sum */
            t = t + a[2][l] * b2;                    /* VMADDAz */
            ac[l] = t;
            c->vu0.vf[8 + j].w[l] = f2u(t + a[3][l] * b3);   /* VMADDw */
        }
    }
    _Pragma("GCC unroll 4")
    for (int l = 0; l < 4; l++) c->vu0.acc.w[l] = f2u(ac[l]);
    _Pragma("GCC unroll 16")
    for (int k = 0; k < 4; k++) c->vu0.vf[4 + k] = A[k];
    _Pragma("GCC unroll 4")
    for (int j = 0; j < 4; j++) rm_sq(m_, out + 16 * j, &c->vu0.vf[8 + j]);
}
void y_nat_001201b8(R5900Ctx *c)
{
    if (po_mm < 0) { const char *e = hle_cfg_get("matmul_native"); po_mm = e ? atoi(e) : 0; }
    if (po_mm == 2) {   /* check: native into a copy of the VU0 state, then the translated body; compare */
        const R5900Mem m_ = {c->ram, c->ram_mask};
        R5900Reg128 in[8], o[4], vs[8], acc;
        for (int k = 0; k < 4; k++) { rm_lq(m_, c->r[5].w[0] + 16 * k, &in[k]); rm_lq(m_, c->r[6].w[0] + 16 * k, &in[4 + k]); }
        const uint32_t out = c->r[4].w[0];
        pose_matmul(c, out, c->r[5].w[0], c->r[6].w[0]);
        for (int k = 0; k < 4; k++) rm_lq(m_, out + 16 * k, &o[k]);
        for (int k = 0; k < 8; k++) vs[k] = c->vu0.vf[4 + k];
        acc = c->vu0.acc;
        for (int k = 0; k < 4; k++) { rm_sq(m_, c->r[5].w[0] + 16 * k, &in[k]); rm_sq(m_, c->r[6].w[0] + 16 * k, &in[4 + k]); }
        fn_001201b8__ynguest(c);
        static int nbad, ncall;
        ncall++;
        int bad = memcmp(&acc, &c->vu0.acc, 16) != 0;
        for (int k = 0; k < 8; k++) bad |= memcmp(&vs[k], &c->vu0.vf[4 + k], 16) != 0;
        for (int k = 0; k < 4; k++) { R5900Reg128 t; rm_lq(m_, out + 16 * k, &t); bad |= memcmp(&t, &o[k], 16) != 0; }
        if (bad && nbad++ < 10) LOG("nat_pose: matmul call %d differs\n", ncall);
        if (ncall % 100000 == 0) LOG("nat_pose: matmul check %d calls, %d differ\n", ncall, nbad);
        return;
    }
    if (po_mm) pose_matmul(c, c->r[4].w[0], c->r[5].w[0], c->r[6].w[0]); else fn_001201b8__ynguest(c);
}

static void pose_calc_matrices(R5900Ctx *c)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t sp0 = c->r[29].w[0], sp = sp0 - 128;
    const uint32_t s4 = c->r[4].w[0], s5 = s4 + 2384;
    uint32_t s3 = PO_LW(s4, 68);
    PO_CALL(fn_00120b18, 0x24e42c);
    PO_ARG(4, s4 + 2464);
    PO_CALL(fn_00120b80, 0x24e434);
    for (;;) {
        const uint32_t node = PO_LW(s4 + PO_LHU(s3, 10) * 4, 3436);   /* 0x2505A8 */
        c->r[2].d[0] = SX32(node);
        PO_CALL(fn_00120ab0, 0x24e44c);
        PO_ARG(4, node + 80);
        PO_CALL(fn_00120b98, 0x24e45c);
        PO_ARG(4, s4); PO_ARG(5, sp); PO_ARG(6, node);
        PO_CALL(fn_0024e100, 0x24e46c);
        {   /* 0x121FB8: the matrix's translation row = the node's position (xyz; w kept) */
            R5900Reg128 a, b;
            rm_lq(m_, node + 144, &a);
            rm_lq(m_, sp + 48, &b);
            c->vu0.vf[4] = a;
            b.w[0] = a.w[0]; b.w[1] = a.w[1]; b.w[2] = a.w[2];
            c->vu0.vf[5] = b;
            rm_sq(m_, sp + 48, &b);
        }
        PO_ARG(4, node + 16); PO_ARG(5, node + 80); PO_ARG(6, sp);
        if (po_mm == 1) pose_matmul(c, node + 16, node + 80, sp);
        else if (po_mm == 4) { pose_matmul(c, sp + 64, node + 80, sp); pose_matmul(c, node + 16, node + 80, sp); }
        else if (po_mm == 5) { PO_ARG(4, sp + 64); PO_CALL(fn_001201b8__ynguest, 0x24e488); PO_ARG(4, node + 16); PO_ARG(5, node + 80); PO_ARG(6, sp); PO_CALL(fn_001201b8__ynguest, 0x24e488); }
        else PO_CALL(fn_001201b8, 0x24e488);
        PO_ARG(4, node + 16);
        PO_CALL(fn_00120b80, 0x24e490);
        uint32_t v0 = PO_LHU(s3, 4);
        if (v0) { PO_ARG(4, v0); PO_CALL(fn_00120ae0, 0x24e4a4); }
        if (PO_LHU(s3, 6)) break;
        s3 += PO_LW(s3, 0);
    }
    {   /* 0x2505A8 (node 0), 0x121FA8: the root node's position into the object */
        const uint32_t root = PO_LW(s4, 3436);
        R5900Reg128 q;
        rm_lq(m_, root + 64, &q);
        c->r[8] = q;
        rm_sq(m_, s5 + 32, &q);
        c->r[2].d[0] = SX32(root);
    }
    c->r[29].d[0] = SX32(sp0);
}

void y_nat_0024e3f8(R5900Ctx *c)
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("pose_native"); on = e ? atoi(e) : 1; }
    if (on) pose_calc_matrices(c); else fn_0024e3f8__ynguest(c);
}

/* 0x2403F8 (the stage octree's cube test; a0 = view, f12..f14 = centre, f15 = half size): the 8 corners through
   the view matrix (+80) and the projection (+144) (0x121FD0 twice), divided by w (0x121F68) and tested against
   x, y in [-1, 1], z in [0, 1]: 0 = every corner inside, 2 = all outside one side, 1 = partly. Native: the same VU0
   operations on the same operands (VU0 registers left as the translated body leaves them), the corner vectors kept
   in host memory (the translated body's own stack frame, which nothing reads). ps2rc.cfg pose_native=0: off. */
void fn_002403f8__ynguest(R5900Ctx *c);
static void cb_matvec(R5900Ctx *c, const uint32_t *m, const uint32_t *v, uint32_t *out)   /* 0x121FD0 */
{
    uint32_t a[4];
    for (int l = 0; l < 4; l++) a[l] = vu0_fmac1(VU0_MUL, 0u, m[l], v[0], 0, 1);
    for (int l = 0; l < 4; l++) a[l] = vu0_fmac1(VU0_MADD, a[l], m[4 + l], v[1], 4, 1);
    for (int l = 0; l < 4; l++) a[l] = vu0_fmac1(VU0_MADD, a[l], m[8 + l], v[2], 4, 1);
    uint32_t r[4];
    for (int l = 0; l < 4; l++) r[l] = vu0_fmac1(VU0_MADD, a[l], m[12 + l], v[3], 4, 1);
    for (int l = 0; l < 4; l++) {
        c->vu0.vf[4].w[l] = m[l]; c->vu0.vf[5].w[l] = m[4 + l]; c->vu0.vf[6].w[l] = m[8 + l];
        c->vu0.vf[7].w[l] = m[12 + l]; c->vu0.vf[8].w[l] = r[l]; c->vu0.acc.w[l] = a[l]; out[l] = r[l];
    }
}
static uint32_t cube_test(R5900Ctx *c)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t a0 = c->r[4].w[0];
    const uint32_t cx = c->fpr[12].u, cy = c->fpr[13].u, cz = c->fpr[14].u, h = c->fpr[15].u;
    uint32_t mv[16], mp[16];
    for (int i = 0; i < 16; i++) { mv[i] = rmb_lw(m_, a0, 80 + 4 * i); mp[i] = rmb_lw(m_, a0, 144 + 4 * i); }
    const uint32_t lo[3] = {f2u(F5(cx) - F5(h)), f2u(F5(cy) - F5(h)), f2u(F5(cz) - F5(h))};
    const uint32_t hi[3] = {f2u(F5(cx) + F5(h)), f2u(F5(cy) + F5(h)), f2u(F5(cz) + F5(h))};
    uint32_t any = 0, all = 0xFFFFFFFFu;
    static int fast = -1;                             /* gs_cube_fast (default 1): host floats first; the exact VU0
                                                         path only when a corner lies near a test boundary (the
                                                         decision is the same either way), and for the last corner
                                                         (the VU0 registers it leaves) */
    if (fast < 0) { const char *e = hle_cfg_get("gs_cube_fast"); fast = e ? atoi(e) : 1; }
    int k0 = 0;
    if (fast) {
        int ok = 1;
        for (int i = 0; i < 16 && ok; i++) {
            uint32_t ea = (mv[i] >> 23) & 0xFF, eb = (mp[i] >> 23) & 0xFF;
            ok = ea != 0xFF && eb != 0xFF && (ea || !(mv[i] & 0x7FFFFFFFu)) && (eb || !(mp[i] & 0x7FFFFFFFu));
        }
        for (int i = 0; i < 3 && ok; i++) {
            uint32_t ea = (lo[i] >> 23) & 0xFF, eb = (hi[i] >> 23) & 0xFF;
            ok = ea != 0xFF && eb != 0xFF && ea && eb;
        }
        uint32_t fa = 0, fl = 0xFFFFFFFFu;
        for (int k = 0; k < 7 && ok; k++) {
            const float v[4] = {u2f((k & 4) ? hi[0] : lo[0]), u2f((k & 2) ? hi[1] : lo[1]), u2f((k & 1) ? hi[2] : lo[2]), 1.0f};
            float t1[4], t2[4];
            for (int l = 0; l < 4; l++)
                t1[l] = u2f(mv[l]) * v[0] + u2f(mv[4 + l]) * v[1] + u2f(mv[8 + l]) * v[2] + u2f(mv[12 + l]) * v[3];
            for (int l = 0; l < 4; l++)
                t2[l] = u2f(mp[l]) * t1[0] + u2f(mp[4 + l]) * t1[1] + u2f(mp[8 + l]) * t1[2] + u2f(mp[12 + l]) * t1[3];
            float w = t2[3];
            if (!(w > 1e-6f || w < -1e-6f)) { ok = 0; break; }
            const float x = t2[0] / w, y = t2[1] / w, z = t2[2] / w;
            const float ex = 1e-3f * (1.0f + (x < 0 ? -x : x)), ey = 1e-3f * (1.0f + (y < 0 ? -y : y));
            const float ez = 1e-3f * (1.0f + (z < 0 ? -z : z));
            if (!(x == x && y == y && z == z) || x > 1e30f || x < -1e30f || y > 1e30f || y < -1e30f || z > 1e30f || z < -1e30f) { ok = 0; break; }
            if ((x + 1.0f < ex && x + 1.0f > -ex) || (x - 1.0f < ex && x - 1.0f > -ex) || (y + 1.0f < ey && y + 1.0f > -ey)
                || (y - 1.0f < ey && y - 1.0f > -ey) || (z < ez && z > -ez) || (z - 1.0f < ez && z - 1.0f > -ez)) { ok = 0; break; }
            uint32_t s0 = 0;
            if (x < -1.0f) s0 |= 1;
            if (1.0f < x) s0 |= 2;
            if (y < -1.0f) s0 |= 4;
            if (1.0f < y) s0 |= 8;
            if (z < 0.0f) s0 |= 16;
            if (1.0f < z) s0 |= 32;
            fa |= s0; fl &= s0;
        }
        if (ok) { any = fa; all = fl; k0 = 7; }
    }
    for (int k = k0; k < 8; k++) {                     /* x outermost, z innermost, as the original's loops */
        const uint32_t v[4] = {(k & 4) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 1) ? hi[2] : lo[2], 0x3f800000u};
        uint32_t t1[4], t2[4];
        cb_matvec(c, mv, v, t1);
        cb_matvec(c, mp, t1, t2);
        {   /* 0x121F68: VF4 = t2 / w */
            for (int l = 0; l < 4; l++) c->vu0.vf[4].w[l] = t2[l];
            c->r[8].d[0] = SX32(t2[3]);
            c->vu0.vf[5] = c->r[8];
            const uint32_t a_ = vu0_clamp(0x3f800000u), b_ = vu0_clamp(t2[3]);
            c->vu0.vi[22] = (b_ & 0x7FFFFFFFu) == 0 ? (((a_ ^ b_) & 0x80000000u) | 0x7F7FFFFFu)
                                                    : vu0_clamp(f2u(u2f(a_) / u2f(b_)));
            vu0_fmac(c, VU0_MUL, 4, 0xf, 4, VU0_QREG, 0, 0, 1);
        }
        const float x = F5(c->vu0.vf[4].w[0]), y = F5(c->vu0.vf[4].w[1]), z = F5(c->vu0.vf[4].w[2]);
        uint32_t s0 = 0;
        if (x < -1.0f) s0 |= 1;
        if (1.0f < x) s0 |= 2;
        if (y < -1.0f) s0 |= 4;
        if (1.0f < y) s0 |= 8;
        if (z < 0.0f) s0 |= 16;
        if (1.0f < z) s0 |= 32;
        any |= s0;
        all &= s0;
    }
    c->fpr[12].u = c->vu0.vf[8].w[3];   /* (the last 0x121F68 argument, as the translated body leaves f12) */
    if (!any) return 0;
    return all ? 2 : 1;
}
void y_nat_002403f8(R5900Ctx *c)
{
    {   /* stage extraction (gs_ge_vu1.h stage_dump): every octree cube "inside", the whole stage drawn */
        extern int gs_stage_dump_all(void);
        if (gs_stage_dump_all()) { c->r[2].d[0] = 0; return; }
    }
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("pose_native"); on = e ? atoi(e) : 1; }
    if (!on) { fn_002403f8__ynguest(c); return; }
    c->r[2].d[0] = cube_test(c);
}
