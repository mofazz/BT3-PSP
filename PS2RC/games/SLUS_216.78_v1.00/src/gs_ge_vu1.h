/* gs_ge_vu1.h - BT3 (SLUS-216.78 v1.00), PSP ONLY, included by gs_ge.h: native VU1 programs for the GE.
 *
 * Programs 0 (fighter parts) and 4 (stage) are not interpreted: at MSCNT their batch is read from VU1 data memory
 * and drawn by the GE in 3D mode. The game's screen matrix S (position -> GS screen x w) becomes the GE projection
 * after a conversion C that maps GS screen coordinates to the 480 x 272 target and GS depth to the 16-bit Z of the
 * through-mode draws (pixel = ndc with viewport scale 1 at 2048 and offset 2048; z = 32767.5 + 32767.5 ndc_z):
 *   clip.x = sx (X - ofx W), clip.y = sy (Y - ofy W), clip.z = (Z / 2^zsh - 32767.5 W) / 32767.5, clip.w = W
 * so the GE divides by the real w: perspective-correct textures, and the depth equals the GS's. Program 0's blend of
 * two bone matrices (p = w A(pos - ofsA) + (1 - w) B(pos - ofsB)) is the GE's two-bone skinning. The programs'
 * own reject test (ADC on triangles touching the clip volume) is not reproduced: the GE clips instead.
 * Program descriptions: the decomp's docs/systems/vu1/prog0_prog1.md and prog4_prog5.md. */

uint32_t vu1_micro_hash(void);
#define VU1H_PROG0 0x6B89F6A5u                        /* FNV-1a of the first 64 pairs, from the ELF's MPG packets */
#define VU1H_PROG4 0x9C152C23u
#define VU1H_PROG1 0x79F2A766u                        /* fade pass */
#define VU1H_PROG7 0x79893F09u                        /* effects (ki blasts' dust, auras): program 4's batch with
                                                         a model matrix and frustum culling - see nat_prog7_batch */
#define VU1H_PROG8 0xF2DBEFDAu                        /* two-matrix blend (Kami's Lookout): see nat_prog8_batch */
#define VU1H_PROG2 0x392445A8u                        /* 2a and 2b (same first 64 pairs) */
#define VU1H_PROG6 0x5EDC8A7Cu                        /* ground shadow: textured with the shadow page (block 0x2A00 =
                                                         page 0x150), which the GE path does not draw, so every
                                                         triangle it makes is dropped; skipped unless gs_prog6=1 */
/* list memory for a native batch: the list is restarted first when it would not fit (GE registers persist), so
   vertices never run past the list into the texture arena */
static void *ge_getmem(uint32_t bytes)
{
    if (sceGuCheckList() + (int)bytes + (int)(16u << 10) > (int)GE_LIST_BYTES) ge_list_restart();
    return sceGuGetMemory((int)bytes);
}
static int ge_native = -1;                            /* ps2rc.cfg gs_native=0: every program interpreted */
static int nat_prog;                                  /* 0 = none, 1 = program 0, 4 = program 4 */
static int nat_batches;                               /* MSCNTs since the MSCALF */
/* (ge_st_nat_batches, ge_st_nat_verts: gs_ge.h) */
static uint32_t ge_nat_us;                            /* native VU1 batches: CPU time (bench) */
static uint32_t ge_begin_us, ge_replay_us, ge_p0_us, ge_draw3_us, ge_rp0_us, ge_rp0_b_us, ge_rp0_pre_us, ge_rp0_lk_us;   /* (bench, gs_tprof=1) */

typedef struct { float u, v; uint32_t c; float x, y, z; } GeV3;
typedef struct { float w0, w1, u, v; uint32_t c; float x, y, z; } GeVW;
typedef struct { float w0, w1; uint32_t c; float nx, ny, nz, x, y, z; } GeVN;   /* the shading layer: normal, no UV */

/* word-wise equality of word-aligned blocks (inline: memcmp is a library call on the PSP) */
static inline int ge_weq(const void *a, const void *b, unsigned nwords)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    uint32_t d = 0;
    for (unsigned i = 0; i < nwords; i++) d |= x[i] ^ y[i];
    return d == 0;
}
static const float *nat_qw(uint32_t qa) { return (const float *)(vu1_mem + 16 * (qa & 0x3FF)); }
/* program 7's matrices (made at its MSCAL as its setup code does): K = S M (screen, S = VU 4..7, M = VU 0..3) and
   P = C M (frustum, C = VU 8..11), columns; nat_begin's sqa NAT_SQA_K reads K */
#define NAT_SQA_K 0x10000u
static uint64_t p7_n, p7_drawn;
static float nat_k7[16] __attribute__((aligned(16))), nat_p7[16], nat_a7;
static const float *nat_mq(uint32_t sqa) { return sqa == NAT_SQA_K ? nat_k7 : nat_qw(sqa); }
static uint32_t nat_w(uint32_t qa, int k)
{
    uint32_t v;
    memcpy(&v, vu1_mem + 16 * (qa & 0x3FF) + 4 * k, 4);
    return v;
}

/* A+D: the 64-bit data and the register number of a quadword */
static void nat_ad(uint32_t qa)
{
    uint64_t d;
    memcpy(&d, vu1_mem + 16 * (qa & 0x3FF), 8);
    reg_write(nat_w(qa, 2) & 0xFF, d);
}

/* a strip GIF tag: PRIM (when PRE) into the register model; returns the vertex count */
static uint32_t nat_tag(uint32_t qa)
{
    uint64_t t;
    memcpy(&t, vu1_mem + 16 * (qa & 0x3FF), 8);
    if ((t >> 46) & 1) reg_write(0x00, (t >> 47) & 0x7FF);
    return (uint32_t)t & 0x7FFF;
}

static uint32_t nat_col(float r, float g, float b, float a)   /* VU colour floats (0x80 = 1.0) -> GE 8888 */
{
    float f[4] = {r, g, b, a};
    uint32_t o = 0;
    for (int k = 0; k < 4; k++) {
        float v = f[k] * 2.0f;
        o |= (uint32_t)(v < 0 ? 0 : v > 255.f ? 255 : (int)v) << (8 * k);
    }
    return o;
}

/* the GE state for the current PRIM / context, then the projection C x S (S = 4 columns at VU qa); returns the
   primitive type, -1 = not drawn */
static ScePspFMatrix4 nat_proj;                       /* the projection of the open batch (CPU clipping) */
static ScePspFMatrix4 nat_proj_last;                  /* the projection sent last (not sent again) */

/* the GE drops a whole triangle when a vertex lands outside its 4096-pixel guard band (the stage's ground near the
   camera: a black wedge). Such triangles are cut in four, again and again (object-space midpoints: the texture
   coordinates and colours are linear there), pieces wholly off one side of the screen dropped. */
#define NAT_CLIP_MAX 6144
static float nat_zk_v = -1.0f;                        /* ps2rc.cfg gs_zscale (default 0.5): the GE depth squeezed by this factor (3D and 2D alike, order kept): Namek's sky dome lies past the far plane the PS2 depth range maps to, and the GE cut it (a black hole in the sky) */
static float nat_zk(void)
{
    if (nat_zk_v < 0) { const char *e = hle_cfg_get("gs_zscale"); nat_zk_v = e ? (float)atof(e) : 0.5f; if (nat_zk_v <= 0) nat_zk_v = 1.0f; }
    return nat_zk_v;
}
float nat_zk_ext(void) { return nat_zk(); }
static float nat_wmin_v = -1.0f;                      /* ps2rc.cfg gs_wmin: the near cutting plane (w), default 1.0 */
static float nat_wmin_get(void)
{
    if (nat_wmin_v < 0) { const char *e = hle_cfg_get("gs_wmin"); nat_wmin_v = e ? (float)atof(e) : 1.0f; if (nat_wmin_v <= 0) nat_wmin_v = 1.0f; }
    return nat_wmin_v;
}
#define NAT_WMIN (nat_wmin_v > 0 ? nat_wmin_v : nat_wmin_get())
static GeV3 nat_cv[NAT_CLIP_MAX];
static uint32_t nat_cn;
static int nat_out(const GeV3 *a)                       /* 0 = inside the band; else the off-screen side bits */
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
    float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    int o = 0;
    if (cw < NAT_WMIN) return 16;
    if (cx < -1500.0f * cw) o |= 1;
    if (cx > (480.0f + 1500.0f) * cw) o |= 2;
    if (cy < -1500.0f * cw) o |= 4;
    if (cy > (272.0f + 1500.0f) * cw) o |= 8;
    if (cx < 0.0f) o |= 64;
    if (cx > 480.0f * cw) o |= 128;
    if (cy < 0.0f) o |= 256;
    if (cy > 272.0f * cw) o |= 512;
    return o;
}
/* the five planes nat_tri_sh cuts against (near, band left / right / top / bottom): one bit per plane the vertex is
   outside of - a triangle whose three vertices share a bit leaves nothing there */
static int nat_hout(const GeV3 *a)
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
    float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    return (cw - NAT_WMIN < 0) | (cx + 1500.0f * cw < 0) << 1 | ((480.0f + 1500.0f) * cw - cx < 0) << 2
           | (cy + 1500.0f * cw < 0) << 3 | ((272.0f + 1500.0f) * cw - cy < 0) << 4;
}
/* nat_out and nat_hout at once: the out code, *h the plane bits */
static int nat_out2(const GeV3 *a, int *h)
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
    float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    *h = (cw - NAT_WMIN < 0) | (cx + 1500.0f * cw < 0) << 1 | ((480.0f + 1500.0f) * cw - cx < 0) << 2
         | (cy + 1500.0f * cw < 0) << 3 | ((272.0f + 1500.0f) * cw - cy < 0) << 4;
    int o = 0;
    if (cw < NAT_WMIN) return 16;
    if (cx < -1500.0f * cw) o |= 1;
    if (cx > (480.0f + 1500.0f) * cw) o |= 2;
    if (cy < -1500.0f * cw) o |= 4;
    if (cy > (272.0f + 1500.0f) * cw) o |= 8;
    if (cx < 0.0f) o |= 64;
    if (cx > 480.0f * cw) o |= 128;
    if (cy < 0.0f) o |= 256;
    if (cy > 272.0f * cw) o |= 512;
    return o;
}
static float nat_wof(const GeV3 *a)
{
    const ScePspFMatrix4 *m = &nat_proj;
    return a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
}
static GeV3 nat_lerp(const GeV3 *a, const GeV3 *b, float f)
{
    GeV3 r;
    r.u = a->u + f * (b->u - a->u); r.v = a->v + f * (b->v - a->v);
    r.x = a->x + f * (b->x - a->x); r.y = a->y + f * (b->y - a->y); r.z = a->z + f * (b->z - a->z);
    uint32_t c = 0;
    for (int k = 0; k < 32; k += 8) {
        float ca = (float)((a->c >> k) & 255), cb = (float)((b->c >> k) & 255);
        int ci = (int)(ca + f * (cb - ca) + 0.5f);
        c |= (uint32_t)(ci < 0 ? 0 : ci > 255 ? 255 : ci) << k;
    }
    r.c = c;
    return r;
}
static GeV3 nat_mid(const GeV3 *a, const GeV3 *b)
{
    GeV3 r;
    r.u = 0.5f * (a->u + b->u); r.v = 0.5f * (a->v + b->v);
    r.x = 0.5f * (a->x + b->x); r.y = 0.5f * (a->y + b->y); r.z = 0.5f * (a->z + b->z);
    uint32_t c = 0;
    for (int k = 0; k < 32; k += 8) c |= ((((a->c >> k) & 255) + ((b->c >> k) & 255)) >> 1) << k;
    r.c = c;
    return r;
}
static void nat_tri(const GeV3 *a, const GeV3 *b, const GeV3 *c, int depth);
/* near clipping (w >= NAT_WMIN) in object space, then the pieces */
static void nat_tri_clip(const GeV3 *a, const GeV3 *b, const GeV3 *c)
{
    const GeV3 *in[3] = {a, b, c};
    float w[3] = {nat_wof(a), nat_wof(b), nat_wof(c)};
    if (w[0] >= NAT_WMIN && w[1] >= NAT_WMIN && w[2] >= NAT_WMIN) { nat_tri(a, b, c, 0); return; }
    GeV3 poly[4];
    int np = 0;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int ii = w[i] >= NAT_WMIN, ij = w[j] >= NAT_WMIN;
        if (ii) poly[np++] = *in[i];
        if (ii != ij) poly[np++] = nat_lerp(in[i], in[j], (NAT_WMIN - w[i]) / (w[j] - w[i]));
    }
    for (int k = 2; k < np; k++) nat_tri(&poly[0], &poly[k - 1], &poly[k], 0);
}
static void nat_tri(const GeV3 *a, const GeV3 *b, const GeV3 *c, int depth)
{
    int oa = nat_out(a), ob = nat_out(b), oc = nat_out(c);
    if (oa & ob & oc & (64 | 128 | 256 | 512)) return;       /* wholly off one side of the screen */
    if (!((oa | ob | oc) & 63) || depth >= 3) {
        if ((oa | ob | oc) & 63) return;                     /* still outside the band: the GE would drop it */
        if (nat_cn + 3 > NAT_CLIP_MAX) return;
        nat_cv[nat_cn++] = *a; nat_cv[nat_cn++] = *b; nat_cv[nat_cn++] = *c;
        return;
    }
    GeV3 ab = nat_mid(a, b), bc = nat_mid(b, c), ca = nat_mid(c, a);
    nat_tri(a, &ab, &ca, depth + 1);
    nat_tri(&ab, b, &bc, depth + 1);
    nat_tri(&ca, &bc, c, depth + 1);
    nat_tri(&ab, &bc, &ca, depth + 1);
}
/* gs_clip_sh (default 1): the triangle cut exactly against the near plane and the four band edges (Sutherland-
   Hodgman in clip space; the plane distances are affine in object space, so the new vertices are object-space
   lerps), then a fan - in place of the subdivision above */
static uint64_t d3_sh, d3_shout, d3_hrej, d3_srej;
static void nat_tri_sh(const GeV3 *a, const GeV3 *b, const GeV3 *c)
{
    d3_sh++;
    GeV3 P[2][12];
    float C[2][12][3];                                /* cx, cy, cw */
    const ScePspFMatrix4 *m = &nat_proj;
    int n = 3, cur = 0;
    P[0][0] = *a; P[0][1] = *b; P[0][2] = *c;
    for (int i = 0; i < 3; i++) {
        const GeV3 *q = &P[0][i];
        C[0][i][0] = q->x * m->x.x + q->y * m->y.x + q->z * m->z.x + m->w.x;
        C[0][i][1] = q->x * m->x.y + q->y * m->y.y + q->z * m->z.y + m->w.y;
        C[0][i][2] = q->x * m->x.w + q->y * m->y.w + q->z * m->z.w + m->w.w;
    }
    for (int k = 0; k < 5; k++) {
        float d[12];
        int anyout = 0;
        for (int i = 0; i < n; i++) {
            const float *q = C[cur][i];
            switch (k) {
            case 0: d[i] = q[2] - NAT_WMIN; break;
            case 1: d[i] = q[0] + 1500.0f * q[2]; break;
            case 2: d[i] = (480.0f + 1500.0f) * q[2] - q[0]; break;
            case 3: d[i] = q[1] + 1500.0f * q[2]; break;
            default: d[i] = (272.0f + 1500.0f) * q[2] - q[1]; break;
            }
            anyout |= d[i] < 0;
        }
        if (!anyout) continue;
        int nn = 0, nx = cur ^ 1;
        for (int i = 0; i < n && nn < 11; i++) {
            int j = i + 1 == n ? 0 : i + 1;
            if (d[i] >= 0) { P[nx][nn] = P[cur][i]; memcpy(C[nx][nn], C[cur][i], 12); nn++; }
            if ((d[i] >= 0) != (d[j] >= 0) && nn < 12) {
                float f = d[i] / (d[i] - d[j]);
                P[nx][nn] = nat_lerp(&P[cur][i], &P[cur][j], f);
                for (int t = 0; t < 3; t++) C[nx][nn][t] = C[cur][i][t] + f * (C[cur][j][t] - C[cur][i][t]);
                nn++;
            }
        }
        n = nn; cur = nx;
        if (n < 3) return;
    }
    d3_shout += (uint64_t)(n - 2);
    for (int i = 2; i < n; i++) {
        if (nat_cn + 3 > NAT_CLIP_MAX) return;
        nat_cv[nat_cn++] = P[cur][0]; nat_cv[nat_cn++] = P[cur][i - 1]; nat_cv[nat_cn++] = P[cur][i];
    }
}
/* draws n vertices of GS primitive prim (3 list, 4 strip, 5 fan) already in list memory v: as they are when every
   vertex is inside the band, else cut as above */
static void nat_draw3_(int prim, GeV3 *v, uint32_t n);
static uint64_t d3_calls[12], d3_verts, d3_bad, d3_badv, d3_clipus;
static void dl_3d(const char *what, const GeV3 *v, uint32_t n, uint32_t stride);
static void nat_draw3(int prim, GeV3 *v, uint32_t n)
{
    if (nat_prog != 4 || !mc_seg_rec) dl_3d("draw3", v, n, (uint32_t)sizeof(GeV3));
    PH_ENTER(PH_DRAW3);
    d3_calls[nat_prog < 12 ? nat_prog : 0]++; d3_verts += n;
    uint32_t t0 = GE_TF();
    nat_draw3_(prim, v, n);
    ge_draw3_us += GE_TF() - t0;
    PH_LEAVE();
}
static void nat_draw3_(int prim, GeV3 *v, uint32_t n)
{
    static int on = -1;
    if (on < 0) on = ge_opt("gs_clip", 1);
    int bad = 0;
    static int oc[4096], hc[4096];                    /* each vertex's out code and plane bits, once */
    const int sides = 64 | 128 | 256 | 512;
    int all = -1, hall = -1, codes = on && prim >= 3 && prim <= 5 && n <= 4096;
    if (codes)
        for (uint32_t i = 0; i < n; i++) { oc[i] = nat_out2(&v[i], &hc[i]); bad |= oc[i] & 63; all &= oc[i]; hall &= hc[i]; }
    else if (on && prim >= 3 && prim <= 5)
        for (uint32_t i = 0; i < n && !bad; i++) bad = nat_out(&v[i]) & 63;
    if (!bad) {
        sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)n, NULL, v);
        return;
    }
    d3_bad++; d3_badv += n;
    if (codes && (all & sides)) { d3_srej++; return; }   /* every vertex off one side of the screen */
    if (codes && hall) { d3_hrej++; return; }                        /* every vertex outside one cutting plane: nothing left */
    nat_cn = 0;
    static int runs = -1;                             /* gs_clip_runs (default 1): runs of triangles inside the band
                                                         drawn straight from the batch (no copy), in order with the
                                                         cut ones */
    if (runs < 0) runs = ge_opt("gs_clip_runs", 0);
    const uint32_t fmt3 = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    if (runs && codes && (prim == 3 || prim == 4)) {
        const uint32_t step = prim == 3 ? 3 : 1;
        uint32_t i = 2;
        while (i < n) {
            uint32_t j = i;                           /* the run of in-band triangles from i */
            while (j < n && !((oc[j - 2] | oc[j - 1] | oc[j]) & 63)) j += step;
            uint32_t ntri = (j - i) / step;
            if (ntri >= 4) {
                if (nat_cn) {                         /* the cut ones before it first */
                    GeV3 *o = ge_getmem(nat_cn * sizeof(GeV3));
                    memcpy(o, nat_cv, nat_cn * sizeof(GeV3));
                    sceGuDrawArray(GU_TRIANGLES, fmt3, (int)nat_cn, NULL, o);
                    nat_cn = 0;
                }
                sceGuDrawArray(prim, fmt3, (int)(prim == 3 ? j - i : j - i + 2), NULL, v + i - 2);
                i = j;
                continue;
            }
            for (; i < j; i += step) {                /* short run: into the list */
                if (nat_cn + 3 > NAT_CLIP_MAX) continue;
                nat_cv[nat_cn++] = v[i - 2]; nat_cv[nat_cn++] = v[i - 1]; nat_cv[nat_cn++] = v[i];
            }
            if (i >= n) break;
            {                                         /* triangle i: outside the band */
                int oa = oc[i - 2], ob = oc[i - 1], ocx = oc[i];
                if (!(oa & ob & ocx & sides)) nat_tri_sh(&v[i - 2], &v[i - 1], &v[i]);
                i += step;
            }
        }
    } else
    for (uint32_t i = 2; i < n; i++) {
        if (prim == 3 && i % 3 != 2) continue;
        uint32_t ia = prim == 5 ? 0 : i - 2;
        const GeV3 *a = &v[ia], *b = &v[i - 1], *c = &v[i];
        if (codes) {                                  /* (as nat_tri_clip / nat_tri at depth 0, from the codes) */
            int oa = oc[ia], ob = oc[i - 1], ocx = oc[i];
            if (oa & ob & ocx & sides) continue;
            if (!((oa | ob | ocx) & 63)) {
                if (nat_cn + 3 > NAT_CLIP_MAX) continue;
                nat_cv[nat_cn++] = *a; nat_cv[nat_cn++] = *b; nat_cv[nat_cn++] = *c;
                continue;
            }
        }
        static int shon = -1;
        if (shon < 0) shon = ge_opt("gs_clip_sh", 1);
        static int hrej = -1;                         /* gs_clip_hrej (default 1): the trivial rejection */
        if (hrej < 0) hrej = ge_opt("gs_clip_hrej", 1);
        if (shon && hrej && (codes ? (hc[ia] & hc[i - 1] & hc[i]) : (nat_hout(a) & nat_hout(b) & nat_hout(c)))) continue;
        if (shon) nat_tri_sh(a, b, c); else nat_tri_clip(a, b, c);
    }
    if (!nat_cn) return;
    GeV3 *o = ge_getmem(nat_cn * sizeof(GeV3));
    memcpy(o, nat_cv, nat_cn * sizeof(GeV3));
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)nat_cn,
                   NULL, o);
}

static int nat_begin_(uint32_t sqa, const GeTex **t);
/* the GE state of the last batch is kept when nothing it depends on changed: no GS state register changed value
   (reg_gen), no GS memory or texture mark changed (ge_epoch), same display list (ge_list_seq: textures stay
   referenced), same screen matrix in VU1 memory - consecutive batches of a mesh mostly share all of it */
static struct { uint32_t gen, epoch, seq, sqa, setup; float s[16]; const GeTex *t; int prim, valid; } nat_bm;
static uint64_t nat_bm_hits, nat_bm_calls[12], nat_bm_miss[6], nat_bm_rej;
static uint64_t nat_rej_f, nat_rej_z, nat_rej_s, nat_rej_sk, *nat_rej_slot[3];
static uint32_t *nat_rej_gen;
static int nat_begin__(uint32_t sqa, const GeTex **t);
static int nat_begin(uint32_t sqa, const GeTex **t) { PH_ENTER(PH_BEGIN); int r = nat_begin__(sqa, t); PH_LEAVE(); return r; }
static int nat_begin__(uint32_t sqa, const GeTex **t)
{
    uint32_t t0 = GE_TF();
    nat_bm_calls[nat_prog < 12 ? nat_prog : 0]++;
    {   /* a target the GE does not draw (the shadow page of program 2b...): rejected again without the set-up */
        static uint64_t rf, rz, rs;
        static uint32_t rgen;
        uint64_t pp = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
        int cx = (int)((pp >> 9) & 1);
        if (rgen && reg[0x4C + cx] == rf && reg[0x4E + cx] == rz && reg[0x40 + cx] == rs && rgen == ge_setup_gen) {
            nat_bm_rej++;
            ge_begin_us += GE_TF() - t0;
            return -1;
        }
        uint64_t sk = ge_st_skip_target;              /* nat_begin_ below: a rejection for the target is noted */
        nat_rej_f = reg[0x4C + cx]; nat_rej_z = reg[0x4E + cx]; nat_rej_s = reg[0x40 + cx]; nat_rej_sk = sk;
        nat_rej_slot[0] = &rf; nat_rej_slot[1] = &rz; nat_rej_slot[2] = &rs; nat_rej_gen = &rgen;
    }
    const float *m = nat_mq(sqa);
    if (nat_bm.valid && nat_bm.setup == ge_setup_gen && nat_bm.gen == reg_gen && nat_bm.epoch == clut_gen && nat_bm.seq == ge_list_seq
        && nat_bm.sqa == sqa && ge_weq(nat_bm.s, m, 16) && (sqa == NAT_SQA_K || (sqa & 0x3FF) + 4 <= 0x400)) {
        ge_flush_batch();                             /* (a 2D batch queued in between still goes first) */
        *t = nat_bm.t;
        if (mc_seg_rec == 1) mc_seg_note(nat_bm.t, nat_bm.t ? ge_tclut : NULL);
        nat_bm_hits++;
        ge_begin_us += GE_TF() - t0;
        return nat_bm.prim;
    }
    if (nat_bm.valid) {                               /* (bench) which condition failed */
        nat_bm_miss[0] += nat_bm.setup != ge_setup_gen; nat_bm_miss[1] += nat_bm.gen != reg_gen;
        nat_bm_miss[2] += nat_bm.epoch != clut_gen; nat_bm_miss[3] += nat_bm.seq != ge_list_seq;
        nat_bm_miss[4] += nat_bm.sqa != sqa || memcmp(nat_bm.s, m, sizeof nat_bm.s) != 0;
    } else nat_bm_miss[5]++;
    int r = nat_begin_(sqa, t);
    {   static int dbl = -1;                          /* (A/B cost check: gs_dbl=1 runs each miss twice) */
        if (dbl < 0) dbl = ge_opt("gs_dbl", 0);
        if (dbl && r >= 0 && (dbl == 2 || nat_prog == 1)) { ge_setup_gen++; r = nat_begin_(sqa, t); }
    }
    if (r < 0 && ge_st_skip_target != nat_rej_sk) {  /* rejected for its target: remember it */
        *nat_rej_slot[0] = nat_rej_f; *nat_rej_slot[1] = nat_rej_z; *nat_rej_slot[2] = nat_rej_s;
        *nat_rej_gen = ge_setup_gen;
    }
    nat_bm.valid = r >= 0;
    nat_bm.gen = reg_gen; nat_bm.setup = ge_setup_gen; nat_bm.epoch = clut_gen; nat_bm.seq = ge_list_seq; nat_bm.sqa = sqa;
    memcpy(nat_bm.s, m, sizeof nat_bm.s);
    nat_bm.t = *t; nat_bm.prim = r;
    ge_begin_us += GE_TF() - t0;
    return r;
}
/* nat_begin would keep the last batch's state (its memo holds): nat_proj is this batch's projection already */
static int nat_bm_holds(uint32_t sqa)
{
    return nat_bm.valid && nat_bm.setup == ge_setup_gen && nat_bm.gen == reg_gen && nat_bm.epoch == clut_gen
           && nat_bm.seq == ge_list_seq && nat_bm.sqa == sqa && ((sqa & 0x3FF) + 4 <= 0x400)
           && ge_weq(nat_bm.s, nat_mq(sqa), 16);
}
static int nat_begin_(uint32_t sqa, const GeTex **t)
{
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    ge_flush_batch();
    ge_hint_lo = 0;                                   /* 3D: every row of the texture (no hint from a 2D primitive) */
    ge_hint_hi = 0x7FFFFFFF;
    if (ge_setup(p, ctx, t, 100) < 0) return -1;      /* (the key's primitive kind 100 keeps 2D and 3D states apart) */
    uint64_t xyo = reg[0x18 + ctx];
    float ofx = (float)(xyo & 0xFFFF) / 16.0f, ofy = (float)((xyo >> 32) & 0xFFFF) / 16.0f;
    float zs = 1.0f / (float)(1u << ge_zshift(ctx));
    ScePspFMatrix4 m;
    ScePspFVector4 *col[4] = {&m.x, &m.y, &m.z, &m.w};
    for (int i = 0; i < 4; i++) {
        const float *s = sqa == NAT_SQA_K ? nat_k7 + 4 * i : nat_qw(sqa + (uint32_t)i);
        col[i]->x = ge_sx * (s[0] - ofx * s[3]);
        col[i]->y = ge_sy * (s[1] - ofy * s[3]);
        col[i]->z = (s[2] * zs - 32767.5f * s[3]) / 32767.5f * nat_zk();
        col[i]->w = s[3];
    }
    if (!ge_weq(&m, &nat_proj_last, 16)) { sceGuSetMatrix(GU_PROJECTION, &m); nat_proj_last = m; }
    nat_proj = m;
    return (int)(p & 7);
}

/* ---- stage mesh cache (ps2rc.cfg gs_meshcache, default 1): the stage draws each visible mesh as MSCALF + a DMA call
   of the mesh's chain (Vu1Pkt_CallProg4), whose VIF data is unpacked into VU1 memory and read back by
   nat_prog4_batch every frame. The first time a chain is drawn its batches (the A+D register, the strip tag and the
   GE vertices) are recorded here; afterwards the DMA walk does not follow the call but puts a REPLAY marker in the
   VIF stream (gs_soft.c, hle_pack_gfx_dma), and the batches are drawn from the record with the current projection:
   no unpack and no conversion. A chain whose recording saw anything but program 4 batches is never cached; a chain
   whose first 64 bytes changed (another stage at that address) empties the cache at the next present. */
#define MC_SLOTS 4096                                 /* power of two */
/* Fighters (program 0, Vu1Pkt_CallProg0: the bone matrices, light and colours are unpacked by the call packet itself,
   then a DMA call of the part's chain) are cached the same way: per batch the static half of the vertices (positions,
   weights, texture coordinates, normals); the shading ramp coordinate and the bones come from VU1 memory at replay.
   The key is the chain AND the microprogram the stream loaded last (a ref into .vutext): the same part chain is also
   drawn by the fade and shadow programs, which are not cached. */
typedef struct { uint32_t addr, ref, check, off, len, seg; uint8_t state, tries, kind, segtries; float cb[6]; } McEnt;   /* cb: the
                                                         chain's box (program 4) */   /* state 0 free, 1 valid, 2 bad */
typedef struct { uint64_t ad, tag; uint32_t reg, n; float bmin[3], bmax[3]; } McBatch;   /* program 4: then n GeV3;
                                                         bmin / bmax: the vertices' box (the guard-band test) */
typedef struct { uint64_t ad0, ad1, tag0, tag1; uint32_t reg0, reg1, n, c0, c1, pad[3]; } McBatch0;   /* program 0:
                                                                    then n GeVW (layer 0) and n GeVN (layer 1) */
static int mc_rec_kind;
static uint8_t *mc_seg_arena;                         /* recorded GE call lists of whole chains */
static uint32_t mc_seg_used, mc_seg_bytes;
static uint32_t mc_last_off = 0xFFFFFFFFu;            /* the last batch of the open record (merging) */
static McEnt *mc_tab;
static uint8_t *mc_arena;
static uint32_t mc_arena_bytes, mc_used, mc_full_logged;
static int mc_on = -1, mc_rec = -1, mc_rec_bad, mc_reset_pending;
static uint32_t mc_rec_start;
static uint64_t mc_st_precull, mc_st_r2;
static uint64_t mc_st_hits, mc_st_recs, mc_st_bad, mc_st_boxin, mc_st_m0, mc_st_m0_big, mc_st_r0, mc_st_c0cp, mc_st_c1cpu, mc_st_r0v;
static int mc_enabled(void)
{
    if (mc_on < 0) {
        mc_on = ge_opt("gs_meshcache", 1);
        uint32_t kb = (uint32_t)ge_opt("gs_meshcache_kb", 2048);
        if (mc_on) {
            mc_tab = ge_kalloc("mesh_cache_tab", MC_SLOTS * sizeof(McEnt));
            mc_arena = ge_kalloc("mesh_cache", kb << 10);
            mc_seg_bytes = (uint32_t)ge_opt("gs_mc_seg_kb", 512) << 10;
            mc_seg_arena = ge_kalloc("mesh_cache_lists", mc_seg_bytes);
            if (!mc_seg_arena) mc_seg_bytes = 0;
            if (!mc_tab || !mc_arena) mc_on = 0;
            else { memset(mc_tab, 0, MC_SLOTS * sizeof(McEnt)); mc_arena_bytes = kb << 10; }
            LOG("gs_ge: stage mesh cache %s (%u KB)\n", mc_on ? "on" : "OFF (no memory)", kb);
        }
    }
    return mc_on;
}
static void mc_reset(void)                            /* only with the GE idle: the records are vertex sources */
{
    memset(mc_tab, 0, MC_SLOTS * sizeof(McEnt));
    mc_used = 0;
    mc_seg_used = 0;
    mc_rec = -1;
    mc_reset_pending = 0;
    LOG("gs_ge: stage mesh cache emptied (%llu replays, %llu records, %llu not cacheable so far)\n",
        (unsigned long long)mc_st_hits, (unsigned long long)mc_st_recs, (unsigned long long)mc_st_bad);
}
static void mc_present(void) { if (mc_reset_pending && mc_tab) mc_reset(); }
static uint32_t mc_check(uint32_t addr)               /* FNV-1a of the chain's first 64 bytes */
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t q = 0; q < 4; q++) {
        const uint8_t *p = gdma_qw(addr + 16 * q);
        if (!p) return 0;
        for (int k = 0; k < 16; k++) h = (h ^ p[k]) * 16777619u;
    }
    return h;
}
static void nat_bone(int idx, uint32_t qa, uint32_t ofs);
static void nat_bone_forget(void);
/* the DMA walk at a call of `addr` with the microprogram `ref` loaded: >= 0 = replay that record; -1 = follow the
   call (and *rec = a slot to record into, or -1) */
static int mc_lookup(uint32_t addr, uint32_t ref, int *rec)
{
    *rec = -1;
    if (!mc_enabled() || mc_reset_pending || bench_ff || !ref) return -1;
    int skipping = gs_skipdraw;                       /* (a skipped frame records nothing) */
    if (addr >= hle_ram_end) return -1;
    {                                                 /* not the game's two DMA buffers (gDmaBuf / gDmaBufEnd at
                                                         0x2FE880 / 0x2FE888): their packets change every frame */
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t b0 = gw(0x2FE880u + 4 * k), b1 = gw(0x2FE888u + 4 * k);
            if (addr >= b0 && addr < b1) return -1;
        }
    }
    uint32_t chk = mc_check(addr);
    uint32_t s = ((addr >> 4) ^ (ref >> 4)) * 2654435761u >> 20 & (MC_SLOTS - 1);
    for (int probe = 0; probe < 16; probe++, s = (s + 1) & (MC_SLOTS - 1)) {
        McEnt *e = &mc_tab[s];
        if (e->addr == addr && e->ref == ref) {
            if (e->check != chk) {                    /* another file at that address: start again */
                if (e->state == 1) { mc_reset_pending = 1; return -1; }
                e->check = chk; e->state = 0; e->tries = 0;
            }
            if (e->state == 1) return (int)s;
            if (e->state == 2 || skipping) return -1;  /* never cached */
            if (e->tries >= 3) { e->state = 2; mc_st_bad++; return -1; }
            if (mc_used + 4096 > mc_arena_bytes) return -1;
            *rec = (int)s;                            /* recorded before without success: again */
            return -1;
        }
        if (!e->addr) {
            if (skipping) return -1;
            if (mc_used + 4096 > mc_arena_bytes) {
                if (!mc_full_logged++) LOG("gs_ge: mesh cache full (%u KB)\n", mc_used >> 10);
                return -1;
            }
            e->addr = addr; e->ref = ref; e->check = chk; e->state = 0; e->tries = 0; e->kind = 0;
            *rec = (int)s;
            return -1;
        }
    }
    return -1;
}
static int mc_rec_has_direct;
static uint32_t mc_why_n[8];
static void mc_why(int k) { mc_why_n[k & 7]++; }
#define MC_DIRECT_TAG 0xD1D1D1D1D1D1D1D1ull               /* a kind 2 record entry: n qwords of DIRECT data follow */
static void mc_rec_direct(const uint8_t *q, uint32_t k)    /* (vif_stream, recording: a whole DIRECT) */
{
    uint32_t need = (uint32_t)sizeof(McBatch) + 16 * k;
    if (mc_used + need > mc_arena_bytes || k > 4096) { mc_rec_bad = 2; mc_why(1); return; }
    McBatch *b = (McBatch *)(mc_arena + mc_used);
    memset(b, 0, sizeof *b);
    b->tag = MC_DIRECT_TAG; b->n = k;
    memcpy(b + 1, q, 16 * k);
    mc_used += need;
    mc_rec_has_direct = 1;
    mc_last_off = 0xFFFFFFFFu;
}
static void mc_rec_begin(int s)
{
    mc_rec_has_direct = 0;
    if (s < 0 || s >= MC_SLOTS || !mc_tab) return;
    mc_rec = s;
    mc_rec_bad = 0;
    mc_last_off = 0xFFFFFFFFu;
    mc_rec_kind = 0;
    mc_rec_start = mc_used;
}
/* program 0 record: consecutive batches with the same registers, tags (but NLOOP), colours and primitive joined into
   one batch (both layers: the GeVW and GeVN halves rebuilt); built past the record, then moved down over it */
static void mc_merge0(void)
{
    static int on = -1;
    if (on < 0) on = ge_opt("gs_mc_merge0", 1);
    if (!on) return;
    const uint64_t tm = ~0xFFFFull;
    uint32_t src = mc_rec_start, end = mc_used, dst = mc_used;
    McBatch0 *cur = NULL;
    uint32_t cur_off = 0;
    #define B0SZ(n) ((((uint32_t)sizeof(McBatch0) + (n) * ((uint32_t)sizeof(GeVW) + (uint32_t)sizeof(GeVN))) + 15u) & ~15u)
    /* pass 1: count the merged size (groups) */
    uint32_t total = 0, gn = 0;
    const McBatch0 *g0 = NULL;
    for (uint32_t o = src; o < end;) {
        const McBatch0 *b = (const McBatch0 *)(mc_arena + o);
        o += B0SZ(b->n);
        uint32_t pk = (uint32_t)((b->tag0 >> 47) & 7);
        if (g0 && g0->ad0 == b->ad0 && g0->ad1 == b->ad1 && g0->reg0 == b->reg0 && g0->reg1 == b->reg1
            && (g0->tag0 & tm) == (b->tag0 & tm) && (g0->tag1 & tm) == (b->tag1 & tm) && g0->c0 == b->c0
            && g0->c1 == b->c1 && (pk == 3 || pk == 4) && ((b->tag1 >> 47) & 7) == pk && b->n && gn + b->n + 3 < 4000) {
            gn += (pk == 4 ? 2u + (gn & 1u) : 0u) + b->n;
        } else {
            if (g0) total += B0SZ(gn);
            g0 = b; gn = b->n;
        }
    }
    if (g0) total += B0SZ(gn);
    if (total > end - src) mc_st_m0_big++;
    if (dst + total > mc_arena_bytes) return;
    /* pass 2: build at dst */
    static GeVW tw[4096];
    static GeVN tn[4096];
    for (uint32_t o = src; o <= end;) {
        const McBatch0 *b = o < end ? (const McBatch0 *)(mc_arena + o) : NULL;
        uint32_t pk = b ? (uint32_t)((b->tag0 >> 47) & 7) : 0;
        int join = b && cur && cur->ad0 == b->ad0 && cur->ad1 == b->ad1 && cur->reg0 == b->reg0 && cur->reg1 == b->reg1
            && (cur->tag0 & tm) == (b->tag0 & tm) && (cur->tag1 & tm) == (b->tag1 & tm) && cur->c0 == b->c0
            && cur->c1 == b->c1 && (pk == 3 || pk == 4) && ((b->tag1 >> 47) & 7) == pk && b->n && cur->n + b->n + 3 < 4000;
        const GeVW *bw = b ? (const GeVW *)(b + 1) : NULL;
        const GeVN *bn = b ? (const GeVN *)(bw + b->n) : NULL;
        if (join) {
            uint32_t k = cur->n;
            if (pk == 4) {
                tw[k] = tw[k - 1]; tn[k] = tn[k - 1]; k++;
                tw[k] = bw[0]; tn[k] = bn[0]; k++;
                if (cur->n & 1) { tw[k] = bw[0]; tn[k] = bn[0]; k++; }
            }
            memcpy(tw + k, bw, b->n * sizeof(GeVW));
            memcpy(tn + k, bn, b->n * sizeof(GeVN));
            cur->n = k + b->n;
        } else {
            if (cur) {                                 /* flush the group */
                McBatch0 *d = (McBatch0 *)(mc_arena + cur_off);
                GeVW *dw = (GeVW *)(d + 1);
                memcpy(dw, tw, cur->n * sizeof(GeVW));
                memcpy(dw + cur->n, tn, cur->n * sizeof(GeVN));
                dst = cur_off + B0SZ(cur->n);
            }
            if (!b) break;
            cur_off = dst;
            cur = (McBatch0 *)(mc_arena + cur_off);
            McBatch0 hb = *b;
            *cur = hb;
            memcpy(tw, bw, b->n * sizeof(GeVW));
            memcpy(tn, bn, b->n * sizeof(GeVN));
        }
        o += B0SZ(b->n);
    }
    #undef B0SZ
    memmove(mc_arena + src, mc_arena + end, dst - end);
    mc_used = src + (dst - end);
    mc_st_m0++;
}
static void mc_rec_end(void)
{
    if (mc_rec < 0) return;
    McEnt *e = &mc_tab[mc_rec];
    e->tries++;
    if (mc_rec_has_direct && mc_rec_kind != 2) mc_rec_bad = 2;
    if (!mc_rec_bad && mc_used > mc_rec_start && mc_rec_kind == 1) mc_merge0();
    if (!mc_rec_bad && mc_used > mc_rec_start && mc_rec_kind) {
        e->off = mc_rec_start;
        e->len = mc_used - mc_rec_start;
        e->kind = (uint8_t)mc_rec_kind;
        e->state = 1;
        if (mc_rec_kind == 4) {                       /* the chain's box: every batch's */
            e->cb[0] = e->cb[1] = e->cb[2] = 3.4e38f; e->cb[3] = e->cb[4] = e->cb[5] = -3.4e38f;
            for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
                const McBatch *b = (const McBatch *)(mc_arena + o);
                o += (uint32_t)sizeof(McBatch) + b->n * (uint32_t)sizeof(GeV3);
                if (!b->n) continue;
                for (int k = 0; k < 3; k++) {
                    if (b->bmin[k] < e->cb[k]) e->cb[k] = b->bmin[k];
                    if (b->bmax[k] > e->cb[3 + k]) e->cb[3 + k] = b->bmax[k];
                }
            }
        }
        mc_st_recs++;
        sceKernelDcacheWritebackRange(mc_arena + e->off, e->len);
    } else {
        mc_used = mc_rec_start;                       /* nothing kept */
        if (mc_rec_bad == 2) { e->state = 2; mc_st_bad++; }
    }
    mc_rec = -1;
}
static int mc_rec_kind_ok(int kind)                   /* one program per record */
{
    if (mc_rec_kind && mc_rec_kind != kind) { mc_rec_bad = 2; mc_why(5); return 0; }
    mc_rec_kind = kind;
    return 1;
}
/* consecutive batches of one chain with the same register, texture and primitive are joined into one draw (the
   per-batch GE set-up was most of the replay's cost): lists are appended, strips stitched with degenerate
   triangles (two repeated vertices, a third when the first strip's length is odd: the winding stays) */
static void mc_box(McBatch *b, const GeV3 *v, uint32_t n, int fresh)
{
    for (uint32_t i = 0; i < n; i++) {
        const float p[3] = {v[i].x, v[i].y, v[i].z};
        for (int k = 0; k < 3; k++) {
            if ((fresh && i == 0) || p[k] < b->bmin[k]) b->bmin[k] = p[k];
            if ((fresh && i == 0) || p[k] > b->bmax[k]) b->bmax[k] = p[k];
        }
    }
}
/* the batch's box wholly inside the guard band under the current projection: so is every vertex (the band is
   the intersection of half-spaces linear in (x, y, z, 1): its 8 corners decide) - drawn without the per-vertex test */
static int nat_out2(const GeV3 *a, int *h);
static int mc_box_inside(const McBatch *b)   /* 1 = inside the band, -1 = wholly outside one cutting plane or off
                                                one side of the screen (the clipper would leave nothing), 0 = else */
{
    int any = 0, all = -1, hall = -1;
    for (int k = 0; k < 8; k++) {
        GeV3 q;
        q.x = (k & 1) ? b->bmax[0] : b->bmin[0];
        q.y = (k & 2) ? b->bmax[1] : b->bmin[1];
        q.z = (k & 4) ? b->bmax[2] : b->bmin[2];
        int h, o = nat_out2(&q, &h);
        any |= o & 63; all &= o; hall &= h;
    }
    if (!any) return 1;
    static int cull = -1;
    if (cull < 0) cull = ge_opt("gs_box_cull", 1);
    return cull && (hall || (all & (64 | 128 | 256 | 512))) ? -1 : 0;
}
static void mc_rec_add(uint64_t ad, uint32_t reg, uint64_t tag, const GeV3 *v, uint32_t n)
{
    if (!mc_rec_kind_ok(4)) return;
    static int merge = -1;
    if (merge < 0) merge = ge_opt("gs_mc_merge", 0);
    uint32_t pk = (uint32_t)((tag >> 47) & 7);
    if (merge && mc_last_off != 0xFFFFFFFFu && mc_last_off >= mc_rec_start && n && (pk == 3 || pk == 4)) {
        McBatch *l = (McBatch *)(mc_arena + mc_last_off);
        const uint64_t tm = ~0xFFFFull;               /* the tag without NLOOP / EOP */
        if (l->ad == ad && l->reg == reg && (l->tag & tm) == (tag & tm) && l->n && l->n + n + 3 < 4096) {
            uint32_t extra = pk == 4 ? 2u + (l->n & 1u) : 0u;
            uint32_t need = (extra + n) * (uint32_t)sizeof(GeV3);
            if (mc_used + need > mc_arena_bytes) { mc_rec_bad = 1; return; }
            GeV3 *o = (GeV3 *)(mc_arena + mc_used), *lv = (GeV3 *)(l + 1);
            if (pk == 4) {
                *o++ = lv[l->n - 1];
                *o++ = v[0];
                if (l->n & 1) *o++ = v[0];
            }
            memcpy(o, v, n * sizeof(GeV3));
            mc_box(l, v, n, 0);
            l->n += extra + n;
            mc_used += need;
            return;
        }
    }
    uint32_t need = (uint32_t)sizeof(McBatch) + n * (uint32_t)sizeof(GeV3);
    if (mc_used + need > mc_arena_bytes) { mc_rec_bad = 1; return; }
    McBatch *b = (McBatch *)(mc_arena + mc_used);
    mc_last_off = mc_used;
    b->ad = ad; b->tag = tag; b->reg = reg; b->n = n;
    memcpy(b + 1, v, n * sizeof(GeV3));
    mc_box(b, v, n, 1);
    mc_used += need;
}
/* program 0: the batch at VU `top` (see nat_prog0_batch), layer 0's vertices with colour c0 */
static void mc_rec_add0(uint32_t top, uint32_t n, uint32_t c0, uint32_t c1)
{
    if (!mc_rec_kind_ok(1)) return;
    uint32_t need = (uint32_t)sizeof(McBatch0) + n * ((uint32_t)sizeof(GeVW) + (uint32_t)sizeof(GeVN));
    need = (need + 15u) & ~15u;
    if (mc_used + need > mc_arena_bytes) { mc_rec_bad = 1; return; }
    McBatch0 *b = (McBatch0 *)(mc_arena + mc_used);
    memcpy(&b->ad0, vu1_mem + 16 * ((top + 1) & 0x3FF), 8);
    memcpy(&b->ad1, vu1_mem + 16 * ((top + 2) & 0x3FF), 8);
    memcpy(&b->tag0, vu1_mem + 16 * ((top + 3) & 0x3FF), 8);
    memcpy(&b->tag1, vu1_mem + 16 * ((top + 4) & 0x3FF), 8);
    b->reg0 = nat_w(top + 1, 2);
    b->reg1 = nat_w(top + 2, 2);
    b->n = n;
    b->c0 = c0;
    b->c1 = c1;
    GeVW *v = (GeVW *)(b + 1);
    GeVN *vn = (GeVN *)(v + n);
    for (uint32_t i = 0; i < n; i++) {
        const float *pos = nat_qw(top + 5 + 3 * i), *nrm = nat_qw(top + 6 + 3 * i), *st = nat_qw(top + 7 + 3 * i);
        v[i].w0 = pos[3]; v[i].w1 = 1.0f - pos[3];
        v[i].u = st[0]; v[i].v = st[1]; v[i].c = c0;
        v[i].x = pos[0]; v[i].y = pos[1]; v[i].z = pos[2];
        vn[i].w0 = pos[3]; vn[i].w1 = 1.0f - pos[3]; vn[i].c = c1;
        vn[i].nx = nrm[0]; vn[i].ny = nrm[1]; vn[i].nz = nrm[2];
        vn[i].x = pos[0]; vn[i].y = pos[1]; vn[i].z = pos[2];
    }
    mc_used += need;
}
static void nat_tagv(uint64_t t) { if ((t >> 46) & 1) reg_write(0x00, (t >> 47) & 0x7FF); }
/* ---- GE call lists of whole stage chains (ps2rc.cfg gs_mc_seg, default 1). The first replay of a program 4 chain
   whose batches all lie inside the guard band is drawn into the main list as usual, after the GE state shadow was
   forgotten (so the chain sets every state it needs) and the projection was sent outside it; those commands are
   copied out, closed with RET, and later replays of the chain are one sceGuCallList when everything the commands
   depend on is as it was: the GS registers the set-up reads, the draw slot, the textures / palettes (pointer and
   version) and the chain's box inside the band under the current projection. After the call the GE state shadow,
   the batch key and the GS registers the chain writes are what the recorded replay left. */
#define MC_SEG_REGS 26
static const uint8_t mc_seg_regs[MC_SEG_REGS] = {0x4C, 0x4D, 0x4E, 0x4F, 0x47, 0x48, 0x42, 0x43, 0x06, 0x07, 0x14, 0x15,
    0x08, 0x09, 0x40, 0x41, 0x18, 0x19, 0x3B, 0x1C, 0x1A, 0x1B, 0x4A, 0x4B, 0x45, 0x46};
typedef struct {
    uint32_t len, nb, nverts;
    int slot, ctx, nt, nc;
    uint64_t regs[MC_SEG_REGS], last_ad, last_tag, bkey[10], last_ad0;
    uint32_t last_reg, last_reg0, c0, c1, tfbp[2], tfbp_end[2];
    int tlru, tlru_end;
    unsigned ctxmask;   /* tfbp: the GE target slots' GS pages when recorded */
    float bmin[3], bmax[3];
    const GeTex *t[6]; uint32_t tv[6];
    const GeClut *cl[6]; uint32_t cv[6];
    uint8_t sh[sizeof(ge_sh)];
} McSeg;                                               /* then len bytes of GE commands (16-aligned) */
static McSeg mc_seg_cur;                               /* the one being recorded */
static uint64_t mc_st_seg_calls, mc_st_seg_recs;
static void mc_seg_note(const GeTex *t, const GeClut *cl)   /* ge_setup_: what the recorded commands read */
{
    McSeg *g = &mc_seg_cur;
    if (t) {
        int k = 0;
        while (k < g->nt && g->t[k] != t) k++;
        if (k == g->nt) { if (g->nt == 6) { mc_seg_rec = 2; return; } g->t[g->nt] = t; g->tv[g->nt++] = t->ver; }
    }
    if (cl) {
        int k = 0;
        while (k < g->nc && g->cl[k] != cl) k++;
        if (k == g->nc) { if (g->nc == 6) { mc_seg_rec = 2; return; } g->cl[g->nc] = cl; g->cv[g->nc++] = cl->ver; }
    }
}
static int mc_seg_slot(void)                           /* the draw slot ge_setup would pick for the current FRAME */
{
    uint32_t fbp = (uint32_t)reg[0x4C + mc_seg_cur.ctx] & 0x1FF;
    return ge_tfbp[0] == fbp ? 0 : ge_tfbp[1] == fbp ? 1 : -1;
}
static int mc_seg_box_inside(const float *bmin, const float *bmax)
{
    for (int k = 0; k < 8; k++) {
        GeV3 q;
        q.x = (k & 1) ? bmax[0] : bmin[0];
        q.y = (k & 2) ? bmax[1] : bmin[1];
        q.z = (k & 4) ? bmax[2] : bmin[2];
        if (nat_out(&q) & 63) return 0;
    }
    return 1;
}
static void nat_proj_send(uint32_t sqa, int ctx)       /* the projection of nat_begin_, sent now */
{
    uint64_t xyo = reg[0x18 + ctx];
    float ofx = (float)(xyo & 0xFFFF) / 16.0f, ofy = (float)((xyo >> 32) & 0xFFFF) / 16.0f;
    float zs = 1.0f / (float)(1u << ge_zshift(ctx));
    ScePspFMatrix4 m;
    ScePspFVector4 *col[4] = {&m.x, &m.y, &m.z, &m.w};
    for (int i = 0; i < 4; i++) {
        const float *q = sqa == NAT_SQA_K ? nat_k7 + 4 * i : nat_qw(sqa + (uint32_t)i);
        col[i]->x = ge_sx * (q[0] - ofx * q[3]);
        col[i]->y = ge_sy * (q[1] - ofy * q[3]);
        col[i]->z = (q[2] * zs - 32767.5f * q[3]) / 32767.5f * nat_zk();
        col[i]->w = q[3];
    }
    if (!ge_weq(&m, &nat_proj_last, 16)) { sceGuSetMatrix(GU_PROJECTION, &m); nat_proj_last = m; }
    nat_proj = m;
}
static int mc_seg_one_proj(void)                       /* program 0: both contexts project alike */
{
    return reg[0x18] == reg[0x19] && ge_zshift(0) == ge_zshift(1)
        && ((reg[0x4C] >> 16) & 63) == ((reg[0x4D] >> 16) & 63);
}
/* a recorded program 0 chain usable now: bones, light matrix and projection sent, then one call; 1 = done */
static int mc_seg_try0_(McEnt *e, uint32_t c0, uint32_t c1, const ScePspFMatrix4 *tm, int dry);
static int mc_seg_try0(McEnt *e, uint32_t c0, uint32_t c1, const ScePspFMatrix4 *tm) { return mc_seg_try0_(e, c0, c1, tm, 0); }
static int mc_seg_try0_(McEnt *e, uint32_t c0, uint32_t c1, const ScePspFMatrix4 *tm, int dry)
{
    if (!e->seg) return 0;
    McSeg *g = (McSeg *)(mc_seg_arena + e->seg - 1);
    if (g->c0 != c0 || g->c1 != c1) return 0;
    for (int k = 0; k < MC_SEG_REGS; k++) if (reg[mc_seg_regs[k]] != g->regs[k]) return 0;
    if (!mc_seg_one_proj()) return 0;
    mc_seg_cur.ctx = g->ctx;
    if (mc_seg_slot() != g->slot || ge_tfbp[0] != g->tfbp[0] || ge_tfbp[1] != g->tfbp[1] || ge_tlru != g->tlru) return 0;
    for (int k = 0; k < g->nt; k++) if (g->t[k]->ver != g->tv[k] || !g->t[k]->gepix) return 0;
    for (int k = 0; k < g->nc; k++) if (g->cl[k]->ver != g->cv[k]) return 0;
    if (dry) return 1;
    ge_flush_batch();
    ge_sx = 480.0f / (float)((((uint32_t)(reg[0x4C + g->ctx] >> 16) & 63) ? ((uint32_t)(reg[0x4C + g->ctx] >> 16) & 63) : 8) * 64);
    static int force = -1;
    if (force < 0) force = ge_opt("gs_seg0_force", 0);
    if (force) memset(&nat_proj_last, 0xFF, sizeof nat_proj_last);
    nat_proj_send(14, g->ctx);
    if (force) nat_bone_forget();
    nat_bone(0, 0, 8);
    nat_bone(1, 4, 9);
    sceGuSetMatrix(GU_TEXTURE, tm);
    if (g->ctxmask & 1) ge_mark_ctx_pages(0);         /* (what the skipped set-ups marked) */
    if (g->ctxmask & 2) ge_mark_ctx_pages(1);
    sceGuCallList((uint8_t *)(g + 1));
    for (int k = 0; k < g->nt; k++) ((GeTex *)g->t[k])->seq = ge_list_seq;
    for (int k = 0; k < g->nc; k++) ((GeClut *)g->cl[k])->seq = ge_list_seq;
    memcpy(&ge_sh, g->sh, sizeof ge_sh);
    ge_tfbp[0] = g->tfbp_end[0]; ge_tfbp[1] = g->tfbp_end[1]; ge_tlru = g->tlru_end;
    if (ge_sh.slot == 0 || ge_sh.slot == 1)          /* libgu keeps the draw buffer and sends it again at a list
                                                         restart: it must be the one the list left */
        sceGuDrawBuffer(GU_PSM_8888, (void *)GE_T_OFF(ge_sh.slot), GE_FB_STRIDE);
    memcpy(ge_bkey, g->bkey, sizeof ge_bkey);
    ge_setup_gen++;
    for (int pass = 0; pass < 2; pass++)              /* every register write of the replay, in its order: each
                                                         TEX0 may load a palette into its own part of the CLUT */
        for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
            const McBatch0 *b = (const McBatch0 *)(mc_arena + o);
            o += ((uint32_t)sizeof(McBatch0) + b->n * ((uint32_t)sizeof(GeVW) + (uint32_t)sizeof(GeVN)) + 15u) & ~15u;
            reg_write((pass ? b->reg1 : b->reg0) & 0xFF, pass ? b->ad1 : b->ad0);
            nat_tagv(pass ? b->tag1 : b->tag0);
        }
    nat_batches += g->nb;
    ge_dirty = 1;
    ge_st_nat_batches += g->nb;
    ge_st_nat_verts += 2 * g->nverts;
    mc_st_seg_calls++;
    return 1;
}
/* a recorded chain usable now: drawn with one call; 1 = done */
static int mc_seg_try(McEnt *e)
{
    if (!e->seg) return 0;
    McSeg *g = (McSeg *)(mc_seg_arena + e->seg - 1);
    for (int k = 0; k < MC_SEG_REGS; k++) if (reg[mc_seg_regs[k]] != g->regs[k]) return 0;
    mc_seg_cur.ctx = g->ctx;
    if (mc_seg_slot() != g->slot || ge_tfbp[0] != g->tfbp[0] || ge_tfbp[1] != g->tfbp[1] || ge_tlru != g->tlru) return 0;
    for (int k = 0; k < g->nt; k++) if (g->t[k]->ver != g->tv[k] || !g->t[k]->gepix) return 0;
    for (int k = 0; k < g->nc; k++) if (g->cl[k]->ver != g->cv[k]) return 0;
    ge_flush_batch();
    ge_sx = 480.0f / (float)((((uint32_t)(reg[0x4C + g->ctx] >> 16) & 63) ? ((uint32_t)(reg[0x4C + g->ctx] >> 16) & 63) : 8) * 64);
    nat_proj_send(0, g->ctx);
    if (!mc_seg_box_inside(g->bmin, g->bmax)) return 0;
    if (g->ctxmask & 1) ge_mark_ctx_pages(0);
    if (g->ctxmask & 2) ge_mark_ctx_pages(1);
    sceGuCallList((uint8_t *)(g + 1));
    for (int k = 0; k < g->nt; k++) ((GeTex *)g->t[k])->seq = ge_list_seq;
    for (int k = 0; k < g->nc; k++) ((GeClut *)g->cl[k])->seq = ge_list_seq;
    memcpy(&ge_sh, g->sh, sizeof ge_sh);
    ge_tfbp[0] = g->tfbp_end[0]; ge_tfbp[1] = g->tfbp_end[1]; ge_tlru = g->tlru_end;
    if (ge_sh.slot == 0 || ge_sh.slot == 1)          /* libgu keeps the draw buffer and sends it again at a list
                                                         restart: it must be the one the list left */
        sceGuDrawBuffer(GU_PSM_8888, (void *)GE_T_OFF(ge_sh.slot), GE_FB_STRIDE);
    memcpy(ge_bkey, g->bkey, sizeof ge_bkey);
    ge_setup_gen++;                                    /* (nat_begin's memo: the GE state came from the list) */
    if ((g->last_reg & 0xFF) != 0 || !((g->last_tag >> 46) & 1)) reg_write(g->last_reg & 0xFF, g->last_ad);
    nat_tagv(g->last_tag);
    nat_batches += g->nb + 1;
    ge_dirty = 1;
    ge_st_nat_batches += g->nb;
    ge_st_nat_verts += g->nverts;
    mc_st_seg_calls++;
    return 1;
}

static void mc_seg_begin(int ctx, uint32_t sqa)        /* recording: GE state forgotten, projection sent first */
{
    memset(&mc_seg_cur, 0, sizeof mc_seg_cur);
    mc_seg_cur.ctx = ctx;
    mc_seg_cur.tfbp[0] = ge_tfbp[0]; mc_seg_cur.tfbp[1] = ge_tfbp[1]; mc_seg_cur.tlru = ge_tlru; mc_seg_ctxmask = 0;
    for (int k = 0; k < MC_SEG_REGS; k++) mc_seg_cur.regs[k] = reg[mc_seg_regs[k]];
    ge_flush_batch();
    ge_sx = 480.0f / (float)((((uint32_t)(reg[0x4C + ctx] >> 16) & 63) ? ((uint32_t)(reg[0x4C + ctx] >> 16) & 63) : 8) * 64);
    nat_proj_send(sqa, ctx);
    ge_sh.valid = 0;
    memset(ge_bkey, 0xFF, sizeof ge_bkey);
    ge_setup_gen++;
}
static void mc_seg_finish(McEnt *e, uint32_t seg_start, uint32_t seq0)
{
    int ok = mc_seg_rec == 1 && ge_list_seq == seq0 && mc_seg_cur.nb;
    mc_seg_rec = 0;
    uint32_t seg_end = (uint32_t)sceGuCheckList();
    mc_seg_cur.slot = mc_seg_slot();
    uint32_t len = seg_end > seg_start ? seg_end - seg_start : 0;
    uint32_t need = ((uint32_t)sizeof(McSeg) + 15u) / 16u * 16u + ((len + 4u + 15u) & ~15u);
    e->segtries++;
    if (!ok || !len || mc_seg_cur.slot < 0 || mc_seg_used + need > mc_seg_bytes) return;
    McSeg *g = (McSeg *)(mc_seg_arena + mc_seg_used);
    mc_seg_cur.len = len + 4;
    mc_seg_cur.tfbp_end[0] = ge_tfbp[0]; mc_seg_cur.tfbp_end[1] = ge_tfbp[1]; mc_seg_cur.tlru_end = ge_tlru;
    mc_seg_cur.ctxmask = mc_seg_ctxmask;
    memcpy(mc_seg_cur.sh, &ge_sh, sizeof ge_sh);
    memcpy(mc_seg_cur.bkey, ge_bkey, sizeof ge_bkey);
    *g = mc_seg_cur;
    uint8_t *cmd = (uint8_t *)(g + 1);
    memcpy(cmd, (uint8_t *)ge_list + seg_start, len);
    uint32_t ret = 0x0B000000u;
    memcpy(cmd + len, &ret, 4);
    sceKernelDcacheWritebackRange(g, need);
    {   /* (debug) the GE command opcodes of the first lists: matrix uploads would be baked state */
        static int nd;
        if (nd < 6 && mc_seg_cur.c1) {
            nd++;
            uint32_t hist[256] = {0};
            for (uint32_t i = 0; i + 4 <= len; i += 4) { uint32_t w; memcpy(&w, cmd + i, 4); hist[w >> 24]++; }
            char b[400]; int k = 0;
            for (int op = 0; op < 256 && k < 380; op++) if (hist[op]) k += snprintf(b + k, sizeof b - k, " %02x:%u", op, hist[op]);
            LOG("mcseg0 list %u bytes:%s\n", len, b);
        }
    }
    e->seg = mc_seg_used + 1;
    mc_seg_used += need;
    mc_st_seg_recs++;
}

static int mc_seg_try0(McEnt *e, uint32_t c0, uint32_t c1, const ScePspFMatrix4 *tm);
static void mc_seg_finish(McEnt *e, uint32_t seg_start, uint32_t seq0);
static void mc_seg_begin(int ctx, uint32_t sqa);
/* layer 1 (the toon ramp): u = 0.5 + 0.5 (L . n' + l3), n' = the normal blended by the two bones - what the GE's
   texture-matrix mapping from the (skinned) normal gives; ps2rc.cfg gs_gpu_ramp=0: computed on the CPU instead */
static void mc_replay0_(const McEnt *e);
static void mc_replay0(const McEnt *e) { PH_ENTER(PH_REPLAY0); mc_replay0_(e); PH_LEAVE(); }
static void mc_replay0_(const McEnt *e)
{
    uint32_t tpre = GE_TF();
    static int gpu = -1;
    if (gpu < 0) gpu = ge_opt("gs_gpu_ramp", 1);
    float L[3] = {nat_qw(10)[0], nat_qw(11)[0], nat_qw(12)[0]}, l3 = nat_qw(13)[0];
    float la[4], lb[4];                               /* the CPU path: as nat_prog0_batch */
    for (int j = 0; j < 3; j++) {
        la[j] = lb[j] = 0;
        for (int k = 0; k < 3; k++) { la[j] += L[k] * nat_qw((uint32_t)j)[k]; lb[j] += L[k] * nat_qw(4 + (uint32_t)j)[k]; }
    }
    la[3] = lb[3] = l3;
    const float *k0 = nat_qw(22), *k1 = nat_qw(23);
    uint32_t c0 = nat_col(k0[0], k0[1], k0[2], k0[3]), c1 = nat_col(k1[0], k1[1], k1[2], k1[3]);
    const uint32_t fmt0 = GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                          | GU_TRANSFORM_3D;
    const uint32_t fmt1 = GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF
                          | GU_TRANSFORM_3D;
    ScePspFMatrix4 tm;
    memset(&tm, 0, sizeof tm);
    tm.x.x = 0.5f * L[0]; tm.y.x = 0.5f * L[1]; tm.z.x = 0.5f * L[2];
    tm.w.x = 0.5f + 0.5f * l3; tm.w.z = 1.0f;          /* v = 0, q = 1 */
    int tm_set = 0;
    static int bylayer = -1;                          /* gs_mc_layers (default 1): layer 0 of every batch of the
                                                         part, then layer 1 - consecutive draws share the GE state */
    if (bylayer < 0) bylayer = ge_opt("gs_mc_layers", 1);
    static int segon0 = -1;
    if (segon0 < 0) segon0 = ge_opt("gs_mc_seg", 1) && ge_opt("gs_mc_seg0", 0);   /* (fighters: a later effect line
                                                         moved in the A/B picture - off until understood) */
    static int dry = -1;
    if (dry < 0) dry = ge_opt("gs_seg0_dry", 0);
    McSeg *dryg = NULL;
    if (segon0 && gpu && dry && e->seg && mc_seg_try0_((McEnt *)e, c0, c1, &tm, 1)) dryg = (McSeg *)(mc_seg_arena + e->seg - 1);
    static int side = -1;
    if (side < 0) side = ge_opt("gs_seg0_side", 0);
    int side_on = 0;
    static uint32_t *scratch;                         /* (gs_seg0_side debug only: 256 KB, allocated when used) */
    if (segon0 && gpu && !dry && mc_seg_try0((McEnt *)e, c0, c1, &tm)) {
        if (!side) return;
        if (!scratch && !(scratch = ge_kalloc("seg0_side", 256u << 10))) return;
        sceGuStart(GU_CALL, scratch);                  /* (debug: the normal replay's side effects, its commands
                                                         thrown away) */
        side_on = 1;
    }
    static int lim = -2, nrec0;
    if (lim == -2) lim = ge_opt("gs_seg0_max", -1);   /* (debug: at most this many fighter lists) */
    int rec = segon0 && gpu && (!e->seg || dryg) && e->segtries < 3 + (dryg ? 1000 : 0) && mc_seg_arena
              && mc_seg_one_proj() && (lim < 0 || nrec0 < lim);
    if (rec) {
        nrec0++;
        if (nrec0 >= 38 && nrec0 <= 46)
            LOG("mcseg0 rec #%d: chain %08x ref %08x len %u c0 %08x c1 %08x L %08x %08x %08x l3 %08x FRAME %llx TEST %llx\n", nrec0, e->addr, e->ref, e->len, c0, c1,
                f2u(L[0]), f2u(L[1]), f2u(L[2]), f2u(l3), (unsigned long long)reg[0x4C], (unsigned long long)reg[0x47]);
    }
    uint32_t seg_start = 0, seq0 = ge_list_seq;
    if (rec) {
        McBatch0 *b0 = (McBatch0 *)(mc_arena + e->off);
        mc_seg_begin((int)((b0->tag0 >> 56) & 1), 14);
        nat_bone(0, 0, 8);
        nat_bone(1, 4, 9);
        sceGuSetMatrix(GU_TEXTURE, &tm);
        tm_set = 1;
        mc_seg_cur.c0 = c0;
        mc_seg_cur.c1 = c1;
        seg_start = (uint32_t)sceGuCheckList();
        mc_seg_rec = 1;
    }
    static int bone1 = -1;                            /* gs_bone_once (default 1): the bones read once per chain */
    if (bone1 < 0) bone1 = ge_opt("gs_bone_once", 1);
    if (bone1) { nat_bone(0, 0, 8); nat_bone(1, 4, 9); }
    ge_rp0_pre_us += GE_TF() - tpre;
    for (int pass = 0; pass < (bylayer ? 2 : 1); pass++)
    for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
        McBatch0 *b = (McBatch0 *)(mc_arena + o);
        uint32_t n = b->n;
        GeVW *src = (GeVW *)(b + 1);
        GeVN *sn = (GeVN *)(src + n);
        o += ((uint32_t)sizeof(McBatch0) + n * ((uint32_t)sizeof(GeVW) + (uint32_t)sizeof(GeVN)) + 15u) & ~15u;
        if (!pass) { nat_batches++; mc_st_r0v += n; }
        for (int layer = bylayer ? pass : 0; layer < (bylayer ? pass + 1 : 2); layer++) {
            uint32_t tlk = GE_TF();
            reg_write((layer ? b->reg1 : b->reg0) & 0xFF, layer ? b->ad1 : b->ad0);
            nat_tagv(layer ? b->tag1 : b->tag0);
            ge_rp0_lk_us += GE_TF() - tlk;
            if (rec) {
                if (layer) { mc_seg_cur.last_reg = b->reg1; mc_seg_cur.last_ad = b->ad1; mc_seg_cur.last_tag = b->tag1; }
                else { mc_seg_cur.last_reg0 = b->reg0; mc_seg_cur.last_ad0 = b->ad0; }
            }
            const GeTex *t;
            int prim = nat_begin(14, &t);
            if (prim < 0) continue;
            if (!bone1) { nat_bone(0, 0, 8); nat_bone(1, 4, 9); }
            if (!layer) {
                GeVW *v = src;
                if (c0 != b->c0) {
                    mc_st_c0cp++;
                    if (mc_seg_rec) mc_seg_rec = 2;
                    v = ge_getmem((int)(n * sizeof(GeVW)));
                    for (uint32_t i = 0; i < n; i++) { v[i] = src[i]; v[i].c = c0; }
                }
                sceGuDrawArray(prim, fmt0, (int)n, NULL, v);
            } else if (gpu && c1 == b->c1) {
                if (!tm_set) { sceGuSetMatrix(GU_TEXTURE, &tm); tm_set = 1; }
                sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
                sceGuTexProjMapMode(GU_NORMAL);
                sceGuDrawArray(prim, fmt1, (int)n, NULL, sn);
                sceGuTexProjMapMode(GU_POSITION);              /* (the TMAP command carries the projection
                                                         source too: back to the default before UV mode) */
                sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
            } else {
                mc_st_c1cpu++;
                if (mc_seg_rec) mc_seg_rec = 2;
                GeVW *v = ge_getmem((int)(n * sizeof(GeVW)));
                for (uint32_t i = 0; i < n; i++) {
                    float w = sn[i].w0, nx = sn[i].nx, ny = sn[i].ny, nz = sn[i].nz;
                    float lx = w * (la[0] * nx + la[1] * ny + la[2] * nz + la[3])
                               + (1.0f - w) * (lb[0] * nx + lb[1] * ny + lb[2] * nz + lb[3]);
                    v[i].w0 = sn[i].w0; v[i].w1 = sn[i].w1;
                    v[i].u = 0.5f + 0.5f * lx; v[i].v = 0; v[i].c = c1;
                    v[i].x = sn[i].x; v[i].y = sn[i].y; v[i].z = sn[i].z;
                }
                sceGuDrawArray(prim, fmt0, (int)n, NULL, v);
            }
            ge_dirty = 1;
            ge_st_nat_verts += n;
        }
        ge_st_nat_batches++;
        if (rec && pass == 0) { mc_seg_cur.nb++; mc_seg_cur.nverts += n; }
    }
    if (side_on) { sceGuFinish(); return; }
    if (rec && dryg) {                                 /* compare the fresh commands with the recorded list */
        mc_seg_rec = 0;
        uint32_t seg_end = (uint32_t)sceGuCheckList(), len = seg_end - seg_start;
        const uint8_t *a = (const uint8_t *)ge_list + seg_start, *b = (const uint8_t *)(dryg + 1);
        static int nlog;
        if (nlog < 12 && (len + 4 != dryg->len || memcmp(a, b, len))) {
            nlog++;
            uint32_t off = 0;
            while (off + 4 <= len && off + 4 <= dryg->len && !memcmp(a + off, b + off, 4)) off += 4;
            uint32_t wa = 0, wb = 0;
            if (off + 4 <= len) memcpy(&wa, a + off, 4);
            if (off + 4 <= dryg->len) memcpy(&wb, b + off, 4);
            LOG("mcseg0 dry chain %08x: now %u bytes, list %u; first difference at %u: now %08x list %08x\n", e->addr, len + 4,
                dryg->len, off, wa, wb);
        }
        ((McEnt *)e)->segtries++;
    } else if (rec) mc_seg_finish((McEnt *)e, seg_start, seq0);
}

static int map_take(void);                            /* gs_ge_map.h */
static void mc_replay_(int s);
static void mc_replay(int s) { PH_ENTER(PH_REPLAY4); mc_replay_(s); PH_LEAVE(); }
/* (debug) ps2rc.cfg gs_drawlog=VBLANK,X,Y: in that frame every 3D draw whose screen box holds (X, Y) is logged */
static int dl_on = -1, dl_vb, dl_x, dl_y;
static unsigned dl_vblanks(void);
static int dl_active(void)
{
    if (dl_on < 0) {
        const char *e = hle_cfg_get("gs_drawlog");
        dl_on = 0;
        if (e && sscanf(e, "%d,%d,%d", &dl_vb, &dl_x, &dl_y) == 3) dl_on = 1;
    }
    return dl_on && (int)dl_vblanks() == dl_vb;
}
int gs_dl_active2(int *x, int *y) { int r = dl_active(); *x = dl_x; *y = dl_y; return r; }
static void dl_3d(const char *what, const GeV3 *v, uint32_t n, uint32_t stride)
{
    if (!dl_active()) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int behind = 0;
    for (uint32_t i = 0; i < n; i++) {
        const GeV3 *a = (const GeV3 *)((const uint8_t *)v + (size_t)i * stride);
        const ScePspFMatrix4 *m = &nat_proj;
        float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
        float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
        float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
        if (cw <= 0.01f) { behind++; continue; }
        float sx = cx / cw, sy = cy / cw;
        if (sx < x0) x0 = sx; if (sx > x1) x1 = sx; if (sy < y0) y0 = sy; if (sy > y1) y1 = sy;
    }
    if (dl_x >= 0 && !(dl_x >= x0 && dl_x <= x1 && dl_y >= y0 && dl_y <= y1) && !behind) return;   /* X < 0: every draw */
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    LOG("drawlog %s prog %d n %u box %.0f,%.0f-%.0f,%.0f behind %d prim %llx tex0 %llx alpha %llx test %llx frame %llx c0 %08x\n",
        what, nat_prog, n, x0, y0, x1, y1, behind, (unsigned long long)p, (unsigned long long)reg[0x06 + ctx],
        (unsigned long long)reg[0x42 + ctx], (unsigned long long)reg[0x47 + ctx], (unsigned long long)reg[0x4C + ctx], v->c);
}
static void mc_replay_(int s)
{
    if (s < 0 || s >= MC_SLOTS || !mc_tab || mc_tab[s].state != 1) return;
    const McEnt *e = &mc_tab[s];
    if (e->kind == 4 && map_take()) return;           /* the native map draws the stage (gs_ge_map.h) */
    if (nat_prog != e->kind) {
        static int nl;
        if (nl++ < 5) LOG("gs_ge: mesh cache replay of a program %d record with program %d loaded\n", e->kind, nat_prog);
        return;
    }
    mc_st_hits++;
    if (gs_skipdraw && e->kind != 2) {                                 /* skipped frame: only the registers the replay writes */
        if (e->kind == 1) {
            for (int pass = 0; pass < 2; pass++)
                for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
                    const McBatch0 *b = (const McBatch0 *)(mc_arena + o);
                    o += ((uint32_t)sizeof(McBatch0) + b->n * ((uint32_t)sizeof(GeVW) + (uint32_t)sizeof(GeVN)) + 15u) & ~15u;
                    reg_write((pass ? b->reg1 : b->reg0) & 0xFF, pass ? b->ad1 : b->ad0);
                    nat_tagv(pass ? b->tag1 : b->tag0);
                }
        } else {
            for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
                const McBatch *b = (const McBatch *)(mc_arena + o);
                o += (uint32_t)sizeof(McBatch) + b->n * (uint32_t)sizeof(GeV3);
                if ((b->reg & 0xFF) != 0 || !((b->tag >> 46) & 1)) reg_write(b->reg & 0xFF, b->ad);
                nat_tagv(b->tag);
            }
        }
        return;
    }
    uint32_t tr = GE_TF();
    if (e->kind == 2) {                               /* program 2, every batch rejected: the register writes */
        for (uint32_t o = e->off, end = e->off + e->len; o < end;) {
            const McBatch *b = (const McBatch *)(mc_arena + o);
            if (b->tag == MC_DIRECT_TAG) {
                o += (uint32_t)sizeof(McBatch) + 16 * b->n;
                gif_block((const uint8_t *)(b + 1), b->n, 0);
                continue;
            }
            o += (uint32_t)sizeof(McBatch);
            reg_write(b->reg & 0xFF, b->ad);
            nat_tagv(b->tag);
            const GeTex *t;
            if (nat_begin(11, &t) >= 0) {             /* drawn now after all: recorded again next time */
                ((McEnt *)e)->state = 0; ((McEnt *)e)->tries = 0;
                break;
            }
        }
        mc_st_r2++;
        ge_replay_us += GE_TF() - tr;
        return;
    }
    if (e->kind == 1) { mc_st_r0++; uint32_t b0 = ge_begin_us; mc_replay0(e); ge_rp0_b_us += ge_begin_us - b0; uint32_t dt = GE_TF() - tr; ge_replay_us += dt; ge_rp0_us += dt; return; }
    static int segon = -1;
    if (segon < 0) segon = ge_opt("gs_mc_seg", 1);
    if (segon && mc_seg_try((McEnt *)e)) { ge_replay_us += GE_TF() - tr; return; }
    int rec = segon && !e->seg && e->segtries < 3 && mc_seg_arena;
    uint32_t seg_start = 0, seq0 = ge_list_seq;
    if (rec) {
        McBatch *b0 = (McBatch *)(mc_arena + e->off);
        memset(&mc_seg_cur, 0, sizeof mc_seg_cur);
        mc_seg_cur.ctx = (int)((b0->tag >> 56) & 1);
        mc_seg_cur.tfbp[0] = ge_tfbp[0]; mc_seg_cur.tfbp[1] = ge_tfbp[1]; mc_seg_cur.tlru = ge_tlru; mc_seg_ctxmask = 0;  /* (PRIM bit 9 = tag bit 47 + 9) */
        for (int k = 0; k < MC_SEG_REGS; k++) mc_seg_cur.regs[k] = reg[mc_seg_regs[k]];
        ge_flush_batch();
        ge_sx = 480.0f / (float)((((uint32_t)(reg[0x4C + mc_seg_cur.ctx] >> 16) & 63) ? ((uint32_t)(reg[0x4C + mc_seg_cur.ctx] >> 16) & 63) : 8) * 64);
        nat_proj_send(0, mc_seg_cur.ctx);
        ge_sh.valid = 0;                              /* every state the chain needs goes into its commands */
        memset(ge_bkey, 0xFF, sizeof ge_bkey);
        ge_setup_gen++;
        seg_start = (uint32_t)sceGuCheckList();
        mc_seg_rec = 1;
        mc_seg_cur.bmin[0] = mc_seg_cur.bmin[1] = mc_seg_cur.bmin[2] = 3.4e38f;
        mc_seg_cur.bmax[0] = mc_seg_cur.bmax[1] = mc_seg_cur.bmax[2] = -3.4e38f;
    }
    uint32_t o = e->off, end = e->off + e->len;
    int chain_bx = 2;                                 /* 2 = not tested yet (after the first set-up: the projection) */
    nat_batches++;                                    /* the chain's header MSCNT */
    while (o < end) {
        McBatch *b = (McBatch *)(mc_arena + o);
        GeV3 *v = (GeV3 *)(b + 1);
        o += (uint32_t)sizeof(McBatch) + b->n * (uint32_t)sizeof(GeV3);
        nat_batches++;
        if ((b->reg & 0xFF) != 0 || !((b->tag >> 46) & 1)) reg_write(b->reg & 0xFF, b->ad);   /* PRIM = 0 by A+D,
                                                         then the tag's PRIM: only the tag's counts */
        nat_tagv(b->tag);
        static int precull = -1;                      /* gs_mc_precull (default 1): a batch whose box lies outside one
                                                         cutting plane (or off one side) under the projection in force
                                                         is dropped before the set-up - the clipper would drop it */
        if (precull < 0) precull = ge_opt("gs_mc_precull", 0);
        if (precull && nat_bm_holds(0)) {
            int ha = -1, oa = -1;
            for (int k = 0; k < 8; k++) {
                GeV3 q;
                q.x = (k & 1) ? b->bmax[0] : b->bmin[0];
                q.y = (k & 2) ? b->bmax[1] : b->bmin[1];
                q.z = (k & 4) ? b->bmax[2] : b->bmin[2];
                int h;
                oa &= nat_out2(&q, &h);
                ha &= h;
            }
            if (ha || (oa & (64 | 128 | 256 | 512))) {
                if (mc_seg_rec) mc_seg_rec = 2;
                mc_st_precull++;
                ge_st_nat_batches++;
                continue;
            }
        }
        if (chain_bx == -1) {                         /* the whole chain outside: nothing to set up or draw */
            if (mc_seg_rec) mc_seg_rec = 2;
            ge_dirty = 1; ge_st_nat_batches++;
            continue;
        }
        const GeTex *t;
        int prim = nat_begin(0, &t);
        if (prim < 0) continue;
        static int boxon = -1;
        if (boxon < 0) boxon = ge_opt("gs_mc_box", 1);
        static int cbon = -1;                         /* gs_chain_box (default 1): the chain's box decides for every
                                                         batch when it is wholly inside the band (or wholly out) */
        if (cbon < 0) cbon = ge_opt("gs_chain_box", 1);
        if (cbon && boxon && chain_bx == 2 && e->cb[0] <= e->cb[3]) {
            McBatch cbb;
            memset(&cbb, 0, sizeof cbb);
            for (int k = 0; k < 3; k++) { cbb.bmin[k] = e->cb[k]; cbb.bmax[k] = e->cb[3 + k]; }
            chain_bx = mc_box_inside(&cbb);
        }
        int bx = !boxon ? 0 : chain_bx == 1 || chain_bx == -1 ? chain_bx : mc_box_inside(b);
        dl_3d("stage", v, b->n, (uint32_t)sizeof(GeV3));
        {   static int sa0 = -1;                       /* (debug) gs_skip_a0=1: stage batches of vertex alpha 0 not drawn */
            if (sa0 < 0) sa0 = ge_opt("gs_skip_a0", 0);
            if (sa0 && b->n && !(v[0].c >> 24)) continue;
        }
        {   static int st = -1; static uint32_t stx;           /* (debug) gs_skip_tex=XXXXXXXX: stage batches of that TEX0 (low word) not drawn */
            if (st < 0) { const char *e = hle_cfg_get("gs_skip_tex"); st = e ? 1 : 0; if (e) stx = (uint32_t)strtoul(e, NULL, 16); }
            if (st) { uint64_t pp = (reg[0x1A] & 1) ? PRIM : reg[0x1B]; if ((uint32_t)reg[0x06 + ((pp >> 9) & 1)] == stx) continue; }
        }
        if (bx > 0) {
            sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)b->n, NULL, v);
            mc_st_boxin++;
        } else if (bx < 0) { mc_st_precull++; if (mc_seg_rec) mc_seg_rec = 2; }
        else { nat_draw3(prim, v, b->n); if (mc_seg_rec) mc_seg_rec = 2; }
        if (mc_seg_rec == 1) {
            for (int k = 0; k < 3; k++) {
                if (b->bmin[k] < mc_seg_cur.bmin[k]) mc_seg_cur.bmin[k] = b->bmin[k];
                if (b->bmax[k] > mc_seg_cur.bmax[k]) mc_seg_cur.bmax[k] = b->bmax[k];
            }
            mc_seg_cur.nb++;
            mc_seg_cur.nverts += b->n;
        }
        ge_dirty = 1;
        ge_st_nat_batches++;
        ge_st_nat_verts += b->n;
        if (rec) { mc_seg_cur.last_ad = b->ad; mc_seg_cur.last_tag = b->tag; mc_seg_cur.last_reg = b->reg; }
    }
    if (rec) {
        int ok = mc_seg_rec == 1 && ge_list_seq == seq0 && mc_seg_cur.nb;
        mc_seg_rec = 0;
        uint32_t seg_end = (uint32_t)sceGuCheckList();
        mc_seg_cur.slot = mc_seg_slot();
        uint32_t len = seg_end > seg_start ? seg_end - seg_start : 0;
        uint32_t need = ((uint32_t)sizeof(McSeg) + 15u) / 16u * 16u + ((len + 4u + 15u) & ~15u);
        ((McEnt *)e)->segtries++;
        if (ok && len && mc_seg_cur.slot >= 0 && mc_seg_used + need <= mc_seg_bytes) {
            McSeg *g = (McSeg *)(mc_seg_arena + mc_seg_used);
            mc_seg_cur.len = len + 4;
            mc_seg_cur.tfbp_end[0] = ge_tfbp[0]; mc_seg_cur.tfbp_end[1] = ge_tfbp[1]; mc_seg_cur.tlru_end = ge_tlru;
            mc_seg_cur.ctxmask = mc_seg_ctxmask;
            memcpy(mc_seg_cur.sh, &ge_sh, sizeof ge_sh);
            memcpy(mc_seg_cur.bkey, ge_bkey, sizeof ge_bkey);
            *g = mc_seg_cur;
            uint8_t *cmd = (uint8_t *)(g + 1);
            memcpy(cmd, (uint8_t *)ge_list + seg_start, len);
            uint32_t ret = 0x0B000000u;
            memcpy(cmd + len, &ret, 4);
            sceKernelDcacheWritebackRange(g, need);
            ((McEnt *)e)->seg = mc_seg_used + 1;
            mc_seg_used += need;
            mc_st_seg_recs++;
        }
    }
    ge_replay_us += GE_TF() - tr;
}

#include "gs_ge_map.h"                                /* the native stage map with LOD (map_native) */

/* ---- stage extraction (dev, ps2rc.cfg stage_dump=V): in the frame whose list is sent at vblank V every program-4
   batch (the stage's static geometry) is written to ms0:/PSP/GAME/PS2RC_BT3/stage_dump.bin - its texture registers,
   the VU matrices it is drawn with (screen VU 0..3, clip VU 4..7) and its vertices (position, colour, s t q) - and
   every texture it uses once, as RGBA (PS2 alpha 0..0x80), named by a hash of its pixels (the stage streams its
   textures through one place of GS memory). From vblank V - 8 on the stage octree's cube test answers "inside"
   (nat_pose.c, gs_stage_dump_all): the whole stage is drawn, not only what the camera sees. The file is made on the
   player's own machine from their disc (work/stage_export.py: glTF + PNG). Records, little endian:
   "BT3S" u32 version | 'TEX ' u32 hash, w, h, w*h u32 | 'BAT ' u64 tex0, tex1, clamp, alpha, test, prim,
   u32 texhash, 32 f32 (VU 0..7), u32 n, n * 12 f32 (x y z w, r g b a, s t q -). */
static int sd_v = -2, sd_fd = -1, sd_done, sd_fv = -1;   /* sd_fv: the vblank of the frame being written */
static uint32_t sd_seen[4096], sd_nseen, sd_batches, sd_tex;
static uint32_t *sd_px;
int gs_stage_dump_all(void)
{
    if (sd_v == -2) { const char *e = hle_cfg_get("stage_dump"); sd_v = e ? atoi(e) : -1; }
    return sd_v >= 0 && (int)vblanks >= sd_v - 8 && (int)vblanks <= sd_v + 2;
}
static void sd_write(const void *p, uint32_t n) { if (sd_fd >= 0 && sceIoWrite(sd_fd, p, n) != (int)n) LOG("sdtrace: short write\n"); }
static int sd_active(void)
{
    if (sd_v >= 0 && !sd_done && sd_fv < 0 && (int)vblanks >= sd_v) sd_fv = (int)vblanks;   /* the first frame from V */
    if (sd_v < 0 || sd_done || sd_fv < 0 || (int)vblanks != sd_fv) {
        if (sd_fd >= 0 && sd_fv >= 0 && (int)vblanks > sd_fv) {
            sceIoClose(sd_fd); sd_fd = -3; sd_done = 1;
            LOG("stage_dump: %u batches, %u textures written\n", sd_batches, sd_tex);
        }
        return 0;
    }
    if (sd_fd == -1) {
        sd_fd = sceIoOpen("ms0:/PSP/GAME/PS2RC_BT3/stage_dump.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (sd_fd < 0) { LOG("stage_dump: cannot write the file (%08x)\n", (unsigned)sd_fd); sd_fd = -3; sd_done = 1; return 0; }
        const uint32_t h[2] = {0x53335442u, 1};       /* "BT3S", version 1 */
        sd_write(h, 8);
        LOG("stage_dump: writing the frame of vblank %d\n", sd_fv);
    }
    return sd_fd >= 0;
}
static uint32_t sd_texture(int ctx)                  /* the bound texture as RGBA, written once; its hash */
{
    const uint64_t tex0 = reg[0x06 + ctx];
    const uint32_t w = 1u << ((tex0 >> 26) & 15), h = 1u << ((tex0 >> 30) & 15);
    if (w > 1024 || h > 1024) return 0;
    if (!sd_px && !(sd_px = ge_kalloc("stage_dump_px", 1024u * 1024u * 4u))) return 0;
    uint32_t p0, p1;
    ge_pages((uint32_t)tex0 & 0x3FFF, ((uint32_t)(tex0 >> 14) & 63) ? (uint32_t)(tex0 >> 14) & 63 : 1,
             (uint32_t)(tex0 >> 20) & 63, h, &p0, &p1);
    ge_materialise(p0, p1);
    const uint64_t cl = cur_clamp;
    cur_clamp = 0;                                   /* (the whole texture, REPEAT) */
    /* a texture already written (same TEX0, same 1024 sampled texels): its hash without reading it all again (the
       stage streams its textures through one place: each batch binds one) */
    static struct { uint64_t tex0; uint32_t sh, hs; } memo[512];
    static uint32_t nmemo;
    uint32_t sh = 0x811C9DC5u;
    for (uint32_t k = 0; k < 1024; k++) {
        const uint32_t x = (k * 37u) & (w - 1), y = ((k * 97u) / 32u + k) & (h - 1);
        sh = (sh ^ tex_fetch(tex0, (int32_t)x, (int32_t)y)) * 0x01000193u;
    }
    for (uint32_t i = 0; i < nmemo; i++)
        if (memo[i].tex0 == tex0 && memo[i].sh == sh) { cur_clamp = cl; return memo[i].hs; }
    uint32_t hs = 0x811C9DC5u;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = tex_fetch(tex0, (int32_t)x, (int32_t)y);
            sd_px[y * w + x] = c;
            hs = (hs ^ c) * 0x01000193u;
        }
    cur_clamp = cl;
    hs = (hs ^ w ^ (h << 16)) | 1u;
    if (nmemo < 512) { memo[nmemo].tex0 = tex0; memo[nmemo].sh = sh; memo[nmemo].hs = hs; nmemo++; }
    for (uint32_t i = 0; i < sd_nseen; i++) if (sd_seen[i] == hs) return hs;
    if (sd_nseen < 4096) sd_seen[sd_nseen++] = hs;
    const uint32_t rec[4] = {0x20584554u, hs, w, h};  /* 'TEX ' */
    sd_write(rec, 16);
    sd_write(sd_px, w * h * 4);
    sd_tex++;
    return hs;
}
static void sd_batch(uint32_t top, uint32_t n)
{
    if (!sd_active()) return;
    const uint64_t prim = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    const int ctx = (int)((prim >> 9) & 1);
    const uint32_t th = ((prim >> 4) & 1) ? sd_texture(ctx) : 0;
    uint32_t hdr[14];
    hdr[0] = 0x20544142u;                            /* 'BAT ' */
    const uint64_t r6[6] = {reg[0x06 + ctx], reg[0x14 + ctx], reg[0x08 + ctx], reg[0x42 + ctx], reg[0x47 + ctx], prim};
    memcpy(hdr + 1, r6, 48);
    hdr[13] = th;
    sd_write(hdr, sizeof hdr);
    sd_write(vu1_mem, 8 * 16);                       /* VU 0..7: screen and clip matrices */
    sd_write(&n, 4);
    for (uint32_t i = 0; i < n; i++) sd_write(vu1_mem + 16 * ((top + 3 + 3 * i) & 0x3FF), 48);
    sd_batches++;
}

/* program 4: header +0 / +1 (A+D: the texture), +2 strip tag, n x (position, colour floats, s t q) */
static void nat_prog4_batch_(uint32_t top);
static void nat_prog4_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog4_batch_(top); PH_LEAVE(); }
static void nat_prog4_batch_(uint32_t top)
{
    nat_ad(top + 1);
    uint32_t n = nat_tag(top + 2);
    if (sd_v != -1 && n && n <= 80) { sd_batch(top, n); if (gs_stage_dump_all()) return; }   /* (the whole stage: written, not drawn) */
    if (map_take()) return;                           /* the native map draws the stage (gs_ge_map.h) */
    const GeTex *t;
    int prim = nat_begin(0, &t);
    if (prim < 0 || !n || n > 80) { if (mc_rec >= 0) mc_rec_bad = 1; return; }
    GeV3 *v = ge_getmem((int)(n * sizeof(GeV3)));
    for (uint32_t i = 0; i < n; i++) {
        const float *pos = nat_qw(top + 3 + 3 * i), *c = nat_qw(top + 4 + 3 * i), *st = nat_qw(top + 5 + 3 * i);
        float iq = st[2] != 0.0f ? 1.0f / st[2] : 1.0f;
        v[i].u = st[0] * iq;
        v[i].v = st[1] * iq;
        v[i].c = nat_col(c[0], c[1], c[2], c[3]);
        v[i].x = pos[0];
        v[i].y = pos[1];
        v[i].z = pos[2];
    }
    if (mc_rec >= 0) {
        uint64_t ad, tag;
        memcpy(&ad, vu1_mem + 16 * ((top + 1) & 0x3FF), 8);
        memcpy(&tag, vu1_mem + 16 * ((top + 2) & 0x3FF), 8);
        mc_rec_add(ad, nat_w(top + 1, 2), tag, v, n);
    }
    nat_draw3(prim, v, n);
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* a bone matrix of program 0: M (pos - ofs) as an affine 4x3 (columns at VU qa, pivot at VU ofs) */
static ScePspFMatrix4 nat_bone_last[2];               /* unchanged bones are not sent again */
static void nat_bone_forget(void) { memset(nat_bone_last, 0xFF, sizeof nat_bone_last); }
static void nat_bone(int idx, uint32_t qa, uint32_t ofs)
{
    const float *c0 = nat_qw(qa), *c1 = nat_qw(qa + 1), *c2 = nat_qw(qa + 2), *c3 = nat_qw(qa + 3), *o = nat_qw(ofs);
    ScePspFMatrix4 m;
    m.x.x = c0[0]; m.x.y = c0[1]; m.x.z = c0[2]; m.x.w = 0;
    m.y.x = c1[0]; m.y.y = c1[1]; m.y.z = c1[2]; m.y.w = 0;
    m.z.x = c2[0]; m.z.y = c2[1]; m.z.z = c2[2]; m.z.w = 0;
    m.w.x = c3[0] - (c0[0] * o[0] + c1[0] * o[1] + c2[0] * o[2]);
    m.w.y = c3[1] - (c0[1] * o[0] + c1[1] * o[1] + c2[1] * o[2]);
    m.w.z = c3[2] - (c0[2] * o[0] + c1[2] * o[1] + c2[2] * o[2]);
    m.w.w = 1;

    if (!ge_weq(&m, &nat_bone_last[idx & 1], 16)) { sceGuBoneMatrix((unsigned)idx, &m); nat_bone_last[idx & 1] = m; }
}

/* program 0: header +0 tag, +1 / +2 A+D of layers 0 / 1, +3 / +4 their strip tags; n x (position with w = weight
   of A, normal, s t 1). Layer 0: model texture, colour = constant 22. Layer 1: ramp texture at u = 0.5 + 0.5 l.x,
   l = w (light A)(normal, 1) + (1 - w)(light B)(normal, 1), colour = constant 23. */
static void nat_prog0_batch_(uint32_t top);
static void nat_prog0_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog0_batch_(top); PH_LEAVE(); }
static void nat_prog0_batch_(uint32_t top)
{
    uint32_t n0 = nat_w(top + 3, 0) & 0x7FFF;
    if (!n0 || n0 > 40) { if (mc_rec >= 0) mc_rec_bad = 1; return; }
    float la[4], lb[4];                               /* the light's x row through A / B (rotation) + its x shift */
    for (int j = 0; j < 3; j++) {
        la[j] = lb[j] = 0;
        for (int k = 0; k < 3; k++) {
            la[j] += nat_qw(10 + (uint32_t)k)[0] * nat_qw((uint32_t)j)[k];
            lb[j] += nat_qw(10 + (uint32_t)k)[0] * nat_qw(4 + (uint32_t)j)[k];
        }
    }
    la[3] = lb[3] = nat_qw(13)[0];
    const float *k0 = nat_qw(22), *k1 = nat_qw(23);
    uint32_t c0 = nat_col(k0[0], k0[1], k0[2], k0[3]), c1 = nat_col(k1[0], k1[1], k1[2], k1[3]);
    if (mc_rec >= 0) mc_rec_add0(top, n0, c0, c1);
    for (int layer = 0; layer < 2; layer++) {
        nat_ad(top + 1 + (uint32_t)layer);
        (void)nat_tag(top + 3 + (uint32_t)layer);
        uint32_t n = n0;
        const GeTex *t;
        int prim = nat_begin(14, &t);
        {
            static int nl;
            extern int gs_bench_logging(void);
            if (gs_bench_logging() && nl < 12) {
                nl++;
                int c = (int)((PRIM >> 9) & 1);
                LOG("prog0 layer %d: PRIM %llx TEX0 %llx ALPHA %llx TEST %llx FRAME %llx col %08x -> %s tex %ux%u gpsm %d\n",
                    layer, (unsigned long long)PRIM, (unsigned long long)reg[0x06 + c],
                    (unsigned long long)reg[0x42 + c], (unsigned long long)reg[0x47 + c],
                    (unsigned long long)reg[0x4C + c], layer ? c1 : c0, prim < 0 ? "skipped" : "drawn", t ? t->w : 0,
                    t ? t->h : 0, t ? t->gpsm : -1);
            }
        }
        if (prim < 0) continue;
        nat_bone(0, 0, 8);
        nat_bone(1, 4, 9);
        GeVW *v = ge_getmem((int)(n * sizeof(GeVW)));
        for (uint32_t i = 0; i < n; i++) {
            const float *pos = nat_qw(top + 5 + 3 * i), *nrm = nat_qw(top + 6 + 3 * i), *st = nat_qw(top + 7 + 3 * i);
            v[i].w0 = pos[3];
            v[i].w1 = 1.0f - pos[3];
            if (layer == 0) { v[i].u = st[0]; v[i].v = st[1]; v[i].c = c0; }
            else {
                float lx = pos[3] * (la[0] * nrm[0] + la[1] * nrm[1] + la[2] * nrm[2] + la[3])
                           + (1.0f - pos[3]) * (lb[0] * nrm[0] + lb[1] * nrm[1] + lb[2] * nrm[2] + lb[3]);
                v[i].u = 0.5f + 0.5f * lx;
                v[i].v = 0;
                v[i].c = c1;
            }
            v[i].x = pos[0];
            v[i].y = pos[1];
            v[i].z = pos[2];
        }
        sceGuDrawArray(prim, GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                       | GU_TRANSFORM_3D, (int)n, NULL, v);
        ge_dirty = 1;
        ge_st_nat_verts += n;
    }
    ge_st_nat_batches++;
}

/* programs 2a / 2b (shadows: the fighter in one flat colour; 2b into the 256 x 256 shadow page, which the GE path
   does not draw): batch as program 0's (+0 tag, +1 A+D TEX0_1, +2 / +3 unused, +4 strip tag with the count; n x
   (position with w = weight of A, normal, s t 1)); colour = constant 10 (a forced to 128), S at 11..14 */
static void nat_prog2_batch_(uint32_t top);
static void nat_prog2_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog2_batch_(top); PH_LEAVE(); }
static void nat_prog2_batch_(uint32_t top)
{
    uint32_t n = nat_w(top + 4, 0) & 0x7FFF;
    if (!n || n > 80) return;
    nat_ad(top + 1);
    (void)nat_tag(top + 4);
    const GeTex *t;
    int prim = nat_begin(11, &t);
    if (mc_rec >= 0) {                                /* program 2 chains whose every batch is rejected (the
                                                         shadow page the GE does not draw): only their register
                                                         writes are recorded (mesh cache kind 2) */
        static int on2 = -1;
        if (on2 < 0) on2 = ge_opt("gs_mc_prog2", 1);
        if (!on2 || prim >= 0 || !mc_rec_kind_ok(2)) { mc_rec_bad = 1; mc_why(prim >= 0 ? 3 : 4); }
        else {
            uint64_t ad, tag;
            memcpy(&ad, vu1_mem + 16 * ((top + 1) & 0x3FF), 8);
            memcpy(&tag, vu1_mem + 16 * ((top + 4) & 0x3FF), 8);
            uint32_t need = (uint32_t)sizeof(McBatch);
            if (mc_used + need > mc_arena_bytes) mc_rec_bad = 1;
            else {
                McBatch *b = (McBatch *)(mc_arena + mc_used);
                memset(b, 0, sizeof *b);
                b->ad = ad; b->tag = tag; b->reg = nat_w(top + 1, 2); b->n = 0;
                mc_used += need;
            }
        }
    }
    if (prim < 0) return;
    nat_bone(0, 0, 8);
    nat_bone(1, 4, 9);
    const float *k = nat_qw(10);
    uint32_t c = nat_col(k[0], k[1], k[2], 128.0f);
    GeVW *v = ge_getmem((int)(n * sizeof(GeVW)));
    for (uint32_t i = 0; i < n; i++) {
        const float *pos = nat_qw(top + 5 + 3 * i), *st = nat_qw(top + 7 + 3 * i);
        v[i].w0 = pos[3];
        v[i].w1 = 1.0f - pos[3];
        v[i].u = st[0];
        v[i].v = st[1];
        v[i].c = c;
        v[i].x = pos[0];
        v[i].y = pos[1];
        v[i].z = pos[2];
    }
    sceGuDrawArray(prim, GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                   | GU_TRANSFORM_3D, (int)n, NULL, v);
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* program 1 (fade pass, decomp prog0_prog1.md): program 0's batch and skinning; a batch whose first normal has 0 in
   the low 16 bits of its w is not drawn; layer 1 = the fade texture of constant 26 (A+D) at
   (u, v) = 0.5 + 0.5 (L (normal, 1)).xy with L = constants 10..13 (one matrix, not blended) */
static void nat_prog1_batch_(uint32_t top);
static void nat_prog1_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog1_batch_(top); PH_LEAVE(); }
static void nat_prog1_batch_(uint32_t top)
{
    uint32_t n0 = nat_w(top + 3, 0) & 0x7FFF;
    if (!n0 || n0 > 40) return;
    if (!(nat_w(top + 6, 3) & 0xFFFF)) return;          /* batch without the fade layer */
    const float *L0 = nat_qw(10), *L1 = nat_qw(11), *L2 = nat_qw(12), *L3 = nat_qw(13);
    const float *k0 = nat_qw(22), *k1 = nat_qw(23);
    uint32_t c0 = nat_col(k0[0], k0[1], k0[2], k0[3]), c1 = nat_col(k1[0], k1[1], k1[2], k1[3]);
    for (int layer = 0; layer < 2; layer++) {
        nat_ad(layer == 0 ? top + 1 : 26);
        (void)nat_tag(top + 3 + (uint32_t)layer);
        const GeTex *t;
        int prim = nat_begin(14, &t);
        if (prim < 0) continue;
        nat_bone(0, 0, 8);
        nat_bone(1, 4, 9);
        GeVW *v = ge_getmem((int)(n0 * sizeof(GeVW)));
        for (uint32_t i = 0; i < n0; i++) {
            const float *pos = nat_qw(top + 5 + 3 * i), *nrm = nat_qw(top + 6 + 3 * i), *st = nat_qw(top + 7 + 3 * i);
            v[i].w0 = pos[3];
            v[i].w1 = 1.0f - pos[3];
            if (layer == 0) { v[i].u = st[0]; v[i].v = st[1]; v[i].c = c0; }
            else {
                float lx = L0[0] * nrm[0] + L1[0] * nrm[1] + L2[0] * nrm[2] + L3[0];
                float ly = L0[1] * nrm[0] + L1[1] * nrm[1] + L2[1] * nrm[2] + L3[1];
                v[i].u = 0.5f + 0.5f * lx;
                v[i].v = 0.5f + 0.5f * ly;
                v[i].c = c1;
            }
            v[i].x = pos[0];
            v[i].y = pos[1];
            v[i].z = pos[2];
        }
        sceGuDrawArray(prim, GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                       | GU_TRANSFORM_3D, (int)n0, NULL, v);
        ge_dirty = 1;
        ge_st_nat_verts += n0;
    }
    ge_st_nat_batches++;
}

/* program 7 (0x2C2D34): its MSCAL setup makes K = S M and P = C M (above); each MSCNT batch: header +0 (copied),
   +1 A+D, +2 strip tag (NLOOP = count), then per vertex position (xyz; w = an integer flag), colour, s t q. A vertex's
   triangle is not drawn (ADC) when its flag is set or one of the last three vertices is outside the frustum
   (CLIPW against P p); colour alpha = VU 12.w; xyz = K p / w, as program 4's batch */
static void nat_prog7_setup(void)
{
    for (int i = 0; i < 4; i++) {
        const float *mi = nat_qw((uint32_t)i);
        for (int r = 0; r < 4; r++) {
            float k = 0, p = 0;
            for (int j = 0; j < 4; j++) { k += nat_qw(4 + (uint32_t)j)[r] * mi[j]; p += nat_qw(8 + (uint32_t)j)[r] * mi[j]; }
            nat_k7[4 * i + r] = k; nat_p7[4 * i + r] = p;
        }
    }
    nat_a7 = nat_qw(12)[3];
}
static void nat_prog7_batch_(uint32_t top);
static void nat_prog7_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog7_batch_(top); PH_LEAVE(); }
static void nat_prog7_batch_(uint32_t top)
{
    nat_ad(top + 1);
    uint32_t n = nat_tag(top + 2) & 0x7FFF;
    const GeTex *t;
    int prim = nat_begin(NAT_SQA_K, &t);
    p7_n++;
    if (prim < 0 || !n || n > 200) return;
    p7_drawn++;
    GeV3 *v = ge_getmem((int)(n * sizeof(GeV3)));
    static uint8_t adc[256];
    uint32_t c0 = 0, c1 = 0, any = 0;                 /* the clip flags of the two vertices before */
    for (uint32_t i = 0; i < n; i++) {
        const float *pos = nat_qw(top + 3 + 3 * i), *c = nat_qw(top + 4 + 3 * i), *st = nat_qw(top + 5 + 3 * i);
        float iq = st[2] != 0.0f ? 1.0f / st[2] : 1.0f;
        v[i].u = st[0] * iq;
        v[i].v = st[1] * iq;
        v[i].c = nat_col(c[0], c[1], c[2], nat_a7);
        v[i].x = pos[0]; v[i].y = pos[1]; v[i].z = pos[2];
        float px = nat_p7[0] * pos[0] + nat_p7[4] * pos[1] + nat_p7[8] * pos[2] + nat_p7[12];
        float py = nat_p7[1] * pos[0] + nat_p7[5] * pos[1] + nat_p7[9] * pos[2] + nat_p7[13];
        float pz = nat_p7[2] * pos[0] + nat_p7[6] * pos[1] + nat_p7[10] * pos[2] + nat_p7[14];
        float pw = nat_p7[3] * pos[0] + nat_p7[7] * pos[1] + nat_p7[11] * pos[2] + nat_p7[15];
        float aw = pw < 0 ? -pw : pw;
        uint32_t cf = px > aw || px < -aw || py > aw || py < -aw || pz > aw || pz < -aw;
        adc[i] = (uint8_t)((cf | c0 | c1) || nat_w(top + 3 + 3 * i, 3));
        any |= adc[i];
        c1 = c0; c0 = cf;
    }
    if (!any || prim < 3 || prim > 5) nat_draw3(prim, v, n);
    else {                                            /* only the triangles whose last vertex has no ADC */
        GeV3 *o = ge_getmem((int)(3 * n * sizeof(GeV3)));
        uint32_t m = 0;
        for (uint32_t i = 2; i < n; i++) {
            if (adc[i] || (prim == 3 && i % 3 != 2)) continue;
            o[m++] = prim == 5 ? v[0] : v[i - 2]; o[m++] = v[i - 1]; o[m++] = v[i];
        }
        if (m) nat_draw3(3, o, m);
    }
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* program 8 (0x2C3084): per vertex (position, w = weight; colour; s t q) the point blended between two rigid
   transforms, B + (A - B) w with A = M1 (p - c1) (M1 = VU 0..3, c1 = VU 8) and B = M2 (p - c2) (M2 = VU 4..7,
   c2 = VU 9), then projected with VU 10..13 (here: the GE, nat_begin(10)) and tested against the frustum VU 14..17
   (CLIPW): a triangle with a vertex outside is not drawn (ADC over the last three, as its FCAND). Header +0 copied,
   +1 A+D, +2 strip tag (count) */
static uint64_t p8_n;
static void nat_prog8_batch_(uint32_t top);
static void nat_prog8_batch(uint32_t top) { PH_ENTER(PH_PROG); nat_prog8_batch_(top); PH_LEAVE(); }
static void nat_prog8_batch_(uint32_t top)
{
    nat_ad(top + 1);
    uint32_t n = nat_tag(top + 2) & 0x7FFF;
    const GeTex *t;
    int prim = nat_begin(10, &t);
    p8_n++;
    if (prim < 0 || !n || n > 200) return;
    const float *m1[4] = {nat_qw(0), nat_qw(1), nat_qw(2), nat_qw(3)}, *m2[4] = {nat_qw(4), nat_qw(5), nat_qw(6), nat_qw(7)};
    const float *c1 = nat_qw(8), *c2 = nat_qw(9);
    const float *pc[4] = {nat_qw(14), nat_qw(15), nat_qw(16), nat_qw(17)};
    GeV3 *v = ge_getmem((int)(n * sizeof(GeV3)));
    static uint8_t adc[256];
    uint32_t f0 = 0, f1 = 0, any = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float *pos = nat_qw(top + 3 + 3 * i), *c = nat_qw(top + 4 + 3 * i), *st = nat_qw(top + 5 + 3 * i);
        float a[3], b[3], d1[3] = {pos[0] - c1[0], pos[1] - c1[1], pos[2] - c1[2]};
        float d2[3] = {pos[0] - c2[0], pos[1] - c2[1], pos[2] - c2[2]};
        for (int r = 0; r < 3; r++) {
            a[r] = m1[0][r] * d1[0] + m1[1][r] * d1[1] + m1[2][r] * d1[2] + m1[3][r];
            b[r] = m2[0][r] * d2[0] + m2[1][r] * d2[1] + m2[2][r] * d2[2] + m2[3][r];
        }
        float w = pos[3], x = b[0] + (a[0] - b[0]) * w, y = b[1] + (a[1] - b[1]) * w, z = b[2] + (a[2] - b[2]) * w;
        float iq = st[2] != 0.0f ? 1.0f / st[2] : 1.0f;
        v[i].u = st[0] * iq; v[i].v = st[1] * iq;
        v[i].c = nat_col(c[0], c[1], c[2], c[3]);
        v[i].x = x; v[i].y = y; v[i].z = z;
        float q[4];
        for (int r = 0; r < 4; r++) q[r] = pc[0][r] * x + pc[1][r] * y + pc[2][r] * z + pc[3][r];
        float aw = q[3] < 0 ? -q[3] : q[3];
        uint32_t cf = q[0] > aw || q[0] < -aw || q[1] > aw || q[1] < -aw || q[2] > aw || q[2] < -aw;
        adc[i] = (uint8_t)(cf | f0 | f1);
        any |= adc[i];
        f1 = f0; f0 = cf;
    }
    if (!any || prim < 3 || prim > 5) nat_draw3(prim, v, n);
    else {
        GeV3 *o = ge_getmem((int)(3 * n * sizeof(GeV3)));
        uint32_t m = 0;
        for (uint32_t i = 2; i < n; i++) {
            if (adc[i] || (prim == 3 && i % 3 != 2)) continue;
            o[m++] = prim == 5 ? v[0] : v[i - 2]; o[m++] = v[i - 1]; o[m++] = v[i];
        }
        if (m) nat_draw3(3, o, m);
    }
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* vu_start's hook: 1 = handled natively (the interpreter is not run) */
static int ge_vu1_native(int cont, uint32_t pair, uint32_t top)
{
    if (ge_on <= 0) return 0;
    if (ge_native < 0) { const char *e = hle_cfg_get("gs_native"); ge_native = e ? atoi(e) : 1; }
    if (!ge_native) return 0;
    if (!cont) {                                      /* MSCAL / MSCALF: which program; its setup is not needed */
        uint32_t h = pair == 0 ? vu1_micro_hash() : 0;
        int np = h == VU1H_PROG0 ? 1 : h == VU1H_PROG4 ? 4 : h == VU1H_PROG2 ? 2 : h == VU1H_PROG1 ? 11 : 0;
        {   static int n4 = -1;                        /* gs_native4=0: program 4 interpreted (a reference picture) */
            if (n4 < 0) n4 = ge_opt("gs_native4", 1);
            if (!n4 && np == 4) np = 0;
        }
        if (h == VU1H_PROG8) {
            static int on8 = -1;
            if (on8 < 0) on8 = ge_opt("gs_prog8", 1);
            if (on8) np = 8;
        }
        if (h == VU1H_PROG7) {
            static int on7 = -1;
            if (on7 < 0) on7 = ge_opt("gs_prog7", 1);
            if (on7) { np = 7; nat_prog7_setup(); }
        }
        if (h == VU1H_PROG6) {
            static int run6 = -1;
            if (run6 < 0) run6 = ge_opt("gs_prog6", 0);
            if (!run6) np = 6;
        }
        static uint32_t last_h;
        static int nlog;
        if (h != last_h && nlog < 12 && !bench_on) {
            nlog++;
            LOG("gs_ge: VU1 MSCAL pair %u, microcode hash %08x -> %s\n", pair, h, np ? "native" : "interpreted");
        }
        last_h = h;
        if (mc_rec >= 0) mc_rec_bad = 2;              /* an MSCAL inside a recorded chain: not a plain mesh */
        nat_prog = np;
        nat_batches = 0;
        return nat_prog != 0;
    }
    if (!nat_prog) return 0;
    if (mc_rec >= 0 && nat_prog != 4 && nat_prog != 1 && nat_prog != 2) mc_rec_bad = 2;   /* only programs 4, 0 and 2 (kind 2) are cached */
    {                                                 /* ps2rc.cfg gs_skipnat: bits 1 prog0, 2 prog1, 4 prog2, 8 prog4 not drawn */
        static int skip = -1;
        if (skip < 0) skip = ge_opt("gs_skipnat", 0);
        int bit = nat_prog == 1 ? 1 : nat_prog == 11 ? 2 : nat_prog == 2 ? 4 : 8;
        if (skip & bit) { if (nat_prog == 4) nat_batches++; return 1; }
    }
    if (ge_st_nat_batches % 500000 == 1)
        LOG("gs_ge: native VU1: %llu batches, %llu vertices (program %d now)\n", (unsigned long long)ge_st_nat_batches,
            (unsigned long long)ge_st_nat_verts, nat_prog == 1 ? 0 : nat_prog);
    if (nat_prog == 6) return 1;                      /* ground shadow: nothing it draws would show */
    if (nat_prog == 4) {
        if (nat_batches++ == 0) return 1;             /* the first MSCNT: the object header (fan tag), not drawn */
        uint32_t t0 = GE_TF();
        nat_prog4_batch(top);
        ge_nat_us += GE_TF() - t0;
    } else if (nat_prog == 8) {
        nat_batches++;
        nat_prog8_batch(top);
    } else if (nat_prog == 7) {
        nat_batches++;
        nat_prog7_batch(top);
    } else if (nat_prog == 2) {
        nat_batches++;
        nat_prog2_batch(top);
    } else if (nat_prog == 11) {
        nat_batches++;
        nat_prog1_batch(top);
    } else {
        nat_batches++;
        uint32_t t0 = GE_TF();
        nat_prog0_batch(top);
        ge_p0_us += GE_TF() - t0;
    }
    return 1;
}
