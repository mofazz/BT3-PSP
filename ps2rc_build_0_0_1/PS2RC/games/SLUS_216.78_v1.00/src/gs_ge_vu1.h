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
#define VU1H_PROG2 0x392445A8u                        /* 2a and 2b (same first 64 pairs) */
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

typedef struct { float u, v; uint32_t c; float x, y, z; } GeV3;
typedef struct { float w0, w1, u, v; uint32_t c; float x, y, z; } GeVW;

static const float *nat_qw(uint32_t qa) { return (const float *)(vu1_mem + 16 * (qa & 0x3FF)); }
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
static const GeTex *nat_tex;                          /* the open batch's texture (gs_3d2d) */
static ScePspFMatrix4 nat_proj;
static ScePspFMatrix4 nat_bm[2];                    /* the two bone matrices of the open batch */                       /* the projection of the open batch (CPU clipping) */

/* the GE drops a whole triangle when a vertex lands outside its 4096-pixel guard band (the stage's ground near the
   camera: a black wedge). Such triangles are cut in four, again and again (object-space midpoints: the texture
   coordinates and colours are linear there), pieces wholly off one side of the screen dropped. */
#define NAT_CLIP_MAX 6144
#define NAT_WMIN 1.0f
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
/* exact clipping (0.0.2): the triangle is clipped in object space (clip coordinates are linear there, and so are the
   texture coordinates and colours) against the near plane w >= NAT_WMIN and the four sides of the GE's guard band,
   then drawn as a fan. The old way cut a triangle in four up to 3 times and dropped the pieces still outside the
   band: the black wedge of missing ground near the camera. Fewer vertices too (at most 8 per triangle, not 64 x 3). */
static float nat_plane(int k, const GeV3 *a)
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    if (k == 0) return cw - NAT_WMIN;
    if (k <= 2) {
        float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
        return k == 1 ? cx + 1500.0f * cw : (480.0f + 1500.0f) * cw - cx;
    }
    if (k <= 4) {
        float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
        return k == 3 ? cy + 1500.0f * cw : (272.0f + 1500.0f) * cw - cy;
    }
    float cz = a->x * m->x.z + a->y * m->y.z + a->z * m->z.z + m->w.z;
    return k == 5 ? cz + cw : cw - cz;                    /* GS depth 0 .. 65535 (after the Z shift) */
}
/* ps2rc.cfg gs_zclip=1 (experiment, 0.0.7): triangles are also clipped to the depth range, so no vertex the GE gets
   lies outside z 0..65535 - the real GE may drop such primitives where PPSSPP clamps them */
static int nat_zclip = -1;
static int nat_zout(const GeV3 *a)
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cz = a->x * m->x.z + a->y * m->y.z + a->z * m->z.z + m->w.z;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    return cz < -cw || cz > cw;
}
static void nat_tri_clip(const GeV3 *a, const GeV3 *b, const GeV3 *c)
{
    int oa = nat_out(a), ob = nat_out(b), oc = nat_out(c);
    if ((oa & ob & oc) & (64 | 128 | 256 | 512)) return;     /* wholly off one side of the screen */
    if (nat_zclip < 0) nat_zclip = ge_opt("gs_zclip", 0);
    int zo = nat_zclip ? (nat_zout(a) | nat_zout(b) | nat_zout(c)) : 0;
    if (!((oa | ob | oc) & (63 | 16)) && !zo) {               /* inside the band and in front: as it is */
        if (nat_cn + 3 > NAT_CLIP_MAX) return;
        nat_cv[nat_cn++] = *a; nat_cv[nat_cn++] = *b; nat_cv[nat_cn++] = *c;
        return;
    }
    GeV3 pa[9], pb[9], *in = pa, *out = pb;
    int n = 3;
    pa[0] = *a; pa[1] = *b; pa[2] = *c;
    for (int k = 0; k < (nat_zclip ? 7 : 5) && n >= 3; k++) {
        float d[9];
        int m = 0, all_in = 1;
        for (int i = 0; i < n; i++) { d[i] = nat_plane(k, &in[i]); if (d[i] < 0.0f) all_in = 0; }
        if (all_in) continue;
        for (int i = 0; i < n; i++) {
            int j = i + 1 == n ? 0 : i + 1;
            int ii = d[i] >= 0.0f, ij = d[j] >= 0.0f;
            if (ii) out[m++] = in[i];
            if (ii != ij && m < 9) out[m++] = nat_lerp(&in[i], &in[j], d[i] / (d[i] - d[j]));
        }
        GeV3 *t = in; in = out; out = t;
        n = m;
    }
    for (int k = 2; k < n; k++) {
        if (nat_cn + 3 > NAT_CLIP_MAX) return;
        nat_cv[nat_cn++] = in[0]; nat_cv[nat_cn++] = in[k - 1]; nat_cv[nat_cn++] = in[k];
    }
}
/* draws n vertices of GS primitive prim (3 list, 4 strip, 5 fan) already in list memory v: as they are when every
   vertex is inside the band, else cut as above */
/* ps2rc.cfg gs_3d2d=1 (experiment, 0.0.6): the stage's 3D triangles projected by the CPU and drawn in through mode
   (GU_TRANSFORM_2D: no GE transform, no GE clipping - the path the 2D sky and HUD already take). Textures are affine
   there (no perspective correction), so big ground triangles warp; the point is to learn whether the real PSP's GE
   drops the 3D batches (black stage on the PSP-3000, fine in PPSSPP) */
/* screen position of an object-space vertex (gs_3d2d) */
static void nat_scr(const GeV3 *a, float *x, float *y)
{
    const ScePspFMatrix4 *m = &nat_proj;
    float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
    float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
    float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
    float iw = cw > 1e-6f ? 1.0f / cw : 1e6f;
    *x = cx * iw; *y = cy * iw;
}
/* through mode has no perspective correction: a triangle bigger than ~40 pixels on screen is cut in four (object-space
   midpoints), up to 3 times, so the texture bends less on the ground near the camera */
static GeV3 nat_sub[NAT_CLIP_MAX];
static uint32_t nat_subn;
static void nat_split2d(const GeV3 *a, const GeV3 *b, const GeV3 *c, int depth)
{
    if (depth < 3) {
        float ax, ay, bx, by, cx, cy;
        nat_scr(a, &ax, &ay); nat_scr(b, &bx, &by); nat_scr(c, &cx, &cy);
        float e = fabsf(ax - bx) + fabsf(ay - by), f = fabsf(bx - cx) + fabsf(by - cy), g = fabsf(cx - ax) + fabsf(cy - ay);
        float big = e > f ? (e > g ? e : g) : (f > g ? f : g);
        if (big > 48.0f && nat_subn + 12 <= NAT_CLIP_MAX) {
            GeV3 ab = nat_mid(a, b), bc = nat_mid(b, c), ca = nat_mid(c, a);
            nat_split2d(a, &ab, &ca, depth + 1);
            nat_split2d(&ab, b, &bc, depth + 1);
            nat_split2d(&ca, &bc, c, depth + 1);
            nat_split2d(&ab, &bc, &ca, depth + 1);
            return;
        }
    }
    if (nat_subn + 3 > NAT_CLIP_MAX) return;
    nat_sub[nat_subn++] = *a; nat_sub[nat_subn++] = *b; nat_sub[nat_subn++] = *c;
}
static void nat_draw_2d(GeV3 *v, uint32_t n)
{
    static int sub = -1;
    if (sub < 0) sub = ge_opt("gs_3d2d_split", 1);
    if (sub) {
        nat_subn = 0;
        for (uint32_t i = 0; i + 2 < n; i += 3) nat_split2d(&v[i], &v[i + 1], &v[i + 2], 0);
        v = nat_sub;
        n = nat_subn;
        if (!n) return;
    }
    const ScePspFMatrix4 *m = &nat_proj;
    float tw = nat_tex ? (float)nat_tex->w : 1.0f, th = nat_tex ? (float)nat_tex->h : 1.0f;
    GeV *o = ge_getmem(n * sizeof(GeV));
    for (uint32_t i = 0; i < n; i++) {
        const GeV3 *a = &v[i];
        float cx = a->x * m->x.x + a->y * m->y.x + a->z * m->z.x + m->w.x;
        float cy = a->x * m->x.y + a->y * m->y.y + a->z * m->z.y + m->w.y;
        float cz = a->x * m->x.z + a->y * m->y.z + a->z * m->z.z + m->w.z;
        float cw = a->x * m->x.w + a->y * m->y.w + a->z * m->z.w + m->w.w;
        float iw = cw > 1e-6f ? 1.0f / cw : 1e6f;
        float z = 32767.5f + 32767.5f * cz * iw;
        o[i].u = a->u * tw; o[i].v = a->v * th; o[i].c = a->c;
        o[i].x = cx * iw; o[i].y = cy * iw; o[i].z = z < 0.0f ? 0.0f : z > 65535.0f ? 65535.0f : z;
    }
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, (int)n, NULL, o);
}
static void nat_draw3(int prim, GeV3 *v, uint32_t n)
{
    static int on = -1, d2 = -1;
    if (on < 0) on = ge_opt("gs_clip", 1);
    if (d2 < 0) d2 = ge_opt("gs_3d2d", 0);
    if (d2) {                                         /* every triangle through the CPU clipper, then 2D */
        nat_cn = 0;
        for (uint32_t i = 2; i < n; i++) {
            if (prim == 3 && i % 3 != 2) continue;
            const GeV3 *a = prim == 5 ? &v[0] : &v[i - 2], *b = &v[i - 1], *c = &v[i];
            if (prim == 4 && (i & 1)) { const GeV3 *t = a; a = b; b = t; }
            nat_tri_clip(a, b, c);
        }
        if (nat_cn) nat_draw_2d(nat_cv, nat_cn);
        return;
    }
    int bad = 0;
    if (on && prim >= 3 && prim <= 5)
        for (uint32_t i = 0; i < n && !bad; i++) bad = (nat_out(&v[i]) & 63) | (nat_zclip > 0 && nat_zout(&v[i]));
    if (!bad) {
        sceGuDrawArray(prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)n, NULL, v);
        return;
    }
    nat_cn = 0;
    for (uint32_t i = 2; i < n; i++) {
        if (prim == 3 && i % 3 != 2) continue;
        const GeV3 *a = prim == 5 ? &v[0] : &v[i - 2], *b = &v[i - 1], *c = &v[i];
        nat_tri_clip(a, b, c);
    }
    if (!nat_cn) return;
    GeV3 *o = ge_getmem(nat_cn * sizeof(GeV3));
    memcpy(o, nat_cv, nat_cn * sizeof(GeV3));
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)nat_cn,
                   NULL, o);
}

/* a weighted (2-bone) batch: drawn by the GE's skinning, or with gs_3d2d=1 blended on the CPU and sent through the
   2D path (the real PSP-3000 GE drops these 3D batches; PPSSPP draws them) */
static void nat_draw3(int prim, GeV3 *v, uint32_t n);
static void nat_draw_w(int prim, GeVW *v, uint32_t n)
{
    static int d2 = -1;
    if (d2 < 0) d2 = ge_opt("gs_3d2d", 0);
    if (!d2) {
        nat_draw_w(prim, v, n);
        return;
    }
    GeV3 t[80];
    if (n > 80) n = 80;
    const ScePspFMatrix4 *A = &nat_bm[0], *B = &nat_bm[1];
    for (uint32_t i = 0; i < n; i++) {
        float x = v[i].x, y = v[i].y, z = v[i].z, w0 = v[i].w0, w1 = v[i].w1;
        t[i].x = w0 * (x * A->x.x + y * A->y.x + z * A->z.x + A->w.x) + w1 * (x * B->x.x + y * B->y.x + z * B->z.x + B->w.x);
        t[i].y = w0 * (x * A->x.y + y * A->y.y + z * A->z.y + A->w.y) + w1 * (x * B->x.y + y * B->y.y + z * B->z.y + B->w.y);
        t[i].z = w0 * (x * A->x.z + y * A->y.z + z * A->z.z + A->w.z) + w1 * (x * B->x.z + y * B->y.z + z * B->z.z + B->w.z);
        t[i].u = v[i].u; t[i].v = v[i].v; t[i].c = v[i].c;
    }
    nat_draw3(prim, t, n);
}
static int nat_begin(uint32_t sqa, const GeTex **t)
{
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    ge_flush_batch();
    ge_hint_lo = 0;                                   /* 3D: every row of the texture (no hint from a 2D primitive) */
    ge_hint_hi = 0x7FFFFFFF;
    if (ge_setup(p, ctx, t, 100) < 0) return -1;      /* (the key's primitive kind 100 keeps 2D and 3D states apart) */
    nat_tex = *t;
    uint64_t xyo = reg[0x18 + ctx];
    float ofx = (float)(xyo & 0xFFFF) / 16.0f, ofy = (float)((xyo >> 32) & 0xFFFF) / 16.0f;
    float zs = 1.0f / (float)(1u << ge_zshift(ctx));
    ScePspFMatrix4 m;
    ScePspFVector4 *col[4] = {&m.x, &m.y, &m.z, &m.w};
    for (int i = 0; i < 4; i++) {
        const float *s = nat_qw(sqa + (uint32_t)i);
        col[i]->x = ge_sx * (s[0] - ofx * s[3]);
        col[i]->y = ge_sy * (s[1] - ofy * s[3]);
        col[i]->z = (s[2] * zs - 32767.5f * s[3]) / 32767.5f;
        col[i]->w = s[3];
    }
    static ScePspFMatrix4 last;                       /* the same projection as the last batch: not sent again */
    if (memcmp(&m, &last, sizeof m)) { sceGuSetMatrix(GU_PROJECTION, &m); last = m; }
    nat_proj = m;
    return (int)(p & 7);
}

/* program 4: header +0 / +1 (A+D: the texture), +2 strip tag, n x (position, colour floats, s t q) */
static void nat_prog4_batch(uint32_t top)
{
    nat_ad(top + 1);
    uint32_t n = nat_tag(top + 2);
    const GeTex *t;
    int prim = nat_begin(0, &t);
    if (prim < 0 || !n || n > 80) return;
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
    nat_draw3(prim, v, n);
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* a bone matrix of program 0: M (pos - ofs) as an affine 4x3 (columns at VU qa, pivot at VU ofs) */
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
    nat_bm[idx & 1] = m;                              /* (gs_3d2d: CPU skinning) */
    static ScePspFMatrix4 last[2];                    /* unchanged bones are not sent again */
    if (memcmp(&m, &last[idx & 1], sizeof m)) { sceGuBoneMatrix((unsigned)idx, &m); last[idx & 1] = m; }
}

/* program 0: header +0 tag, +1 / +2 A+D of layers 0 / 1, +3 / +4 their strip tags; n x (position with w = weight
   of A, normal, s t 1). Layer 0: model texture, colour = constant 22. Layer 1: ramp texture at u = 0.5 + 0.5 l.x,
   l = w (light A)(normal, 1) + (1 - w)(light B)(normal, 1), colour = constant 23. */
static void nat_prog0_batch(uint32_t top)
{
    uint32_t n0 = nat_w(top + 3, 0) & 0x7FFF;
    if (!n0 || n0 > 40) return;
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
        nat_draw_w(prim, v, n);
        ge_dirty = 1;
        ge_st_nat_verts += n;
    }
    ge_st_nat_batches++;
}

/* programs 2a / 2b (shadows: the fighter in one flat colour; 2b into the 256 x 256 shadow page, which the GE path
   does not draw): batch as program 0's (+0 tag, +1 A+D TEX0_1, +2 / +3 unused, +4 strip tag with the count; n x
   (position with w = weight of A, normal, s t 1)); colour = constant 10 (a forced to 128), S at 11..14 */
static void nat_prog2_batch(uint32_t top)
{
    uint32_t n = nat_w(top + 4, 0) & 0x7FFF;
    if (!n || n > 80) return;
    nat_ad(top + 1);
    (void)nat_tag(top + 4);
    const GeTex *t;
    int prim = nat_begin(11, &t);
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
    nat_draw_w(prim, v, n);
    ge_dirty = 1;
    ge_st_nat_batches++;
    ge_st_nat_verts += n;
}

/* program 1 (fade pass, decomp prog0_prog1.md): program 0's batch and skinning; a batch whose first normal has 0 in
   the low 16 bits of its w is not drawn; layer 1 = the fade texture of constant 26 (A+D) at
   (u, v) = 0.5 + 0.5 (L (normal, 1)).xy with L = constants 10..13 (one matrix, not blended) */
static void nat_prog1_batch(uint32_t top)
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
        nat_draw_w(prim, v, n0);
        ge_dirty = 1;
        ge_st_nat_verts += n0;
    }
    ge_st_nat_batches++;
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
        static uint32_t last_h;
        static int nlog;
        if (h != last_h && nlog < 60) {
            nlog++;
            LOG("gs_ge: VU1 MSCAL pair %u, microcode hash %08x -> %s\n", pair, h, np ? "native" : "interpreted");
        }
        last_h = h;
        nat_prog = np;
        nat_batches = 0;
        return nat_prog != 0;
    }
    if (!nat_prog) return 0;
    {                                                 /* ps2rc.cfg gs_skipnat: bits 1 prog0, 2 prog1, 4 prog2, 8 prog4 not drawn */
        static int skip = -1;
        if (skip < 0) skip = ge_opt("gs_skipnat", 0);
        int bit = nat_prog == 1 ? 1 : nat_prog == 11 ? 2 : nat_prog == 2 ? 4 : 8;
        if (skip & bit) { if (nat_prog == 4) nat_batches++; return 1; }
    }
    if (ge_st_nat_batches % 100000 == 1)
        LOG("gs_ge: native VU1: %llu batches, %llu vertices (program %d now)\n", (unsigned long long)ge_st_nat_batches,
            (unsigned long long)ge_st_nat_verts, nat_prog == 1 ? 0 : nat_prog);
    if (nat_prog == 4) {
        if (nat_batches++ == 0) return 1;             /* the first MSCNT: the object header (fan tag), not drawn */
        nat_prog4_batch(top);
    } else if (nat_prog == 2) {
        nat_batches++;
        nat_prog2_batch(top);
    } else if (nat_prog == 11) {
        nat_batches++;
        nat_prog1_batch(top);
    } else {
        nat_batches++;
        nat_prog0_batch(top);
    }
    return 1;
}
