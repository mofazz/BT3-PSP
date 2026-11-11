/*
 * PSP GPU back end for the GS front end (gs_core.c in its GPU mode): the game's GS primitives go to the PSP's GE through
 * sceGu instead of the software rasteriser. Modelled on the PC/Vita back ends (port/src/gs/gs_gpu.c, port/vita/vita_vgl.c).
 *
 * Switched on by a file `gu.txt` in ms0:/PSP/GAME/BT3PSP/ (psp_main.c); without it the software renderer runs, as before.
 *
 *  - Only the frame's own buffer (gGsMainFbp) is drawn: the effect buffers (glow, blur, shadow work) are left out, which is
 *    also where most of the cost of a PS2 frame goes. Textures that are such buffers come out empty.
 *  - Sprites, triangles, lines and points go as they are. The vertex programs' own "shader" draws (GsGpu_DrawVu*) are not
 *    done: with gVglVuReady = 0 gs_vu1.c runs the VU1 programs on the CPU and the triangles arrive here.
 *  - Textures are read out of the emulated GS memory (Gs_VramReadIdx8 / Gs_VramReadRect), kept as 8-bit indices + a 256
 *    entry palette (the PSP's own CLUT) or as 8888, in a small cache that is dropped when it is full or the GS memory under
 *    a texture was written (gGsPageGen).
 *  - The game's frame is 512 x 448 GS pixels (not 640): it becomes the PSP's 480 x 272.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_internal.h"

extern int gGsMainFbp;
extern volatile int gPspPicture;
extern void Psp_DebugOverlay(int vramOffset);
extern volatile int gPspDebug;

int gVglSafe = 32; /* gs_core.c: 32 = every upload written into GS memory at once (so that the textures can be read) */
volatile int gPspGuActive;
unsigned gPspGuRtt, gPspGuTextured, gPspGuPlain, gPspGuFrames, gPspGuPrims, gPspGuTexDecodes, gPspGuTexHits, gPspGuOtherTarget, gPspGuVuIgnored, gPspGuBigTex;
unsigned long long gPspGuTexUs;

#define LIST_BYTES (192 * 1024)
#define ARENA_BYTES (2u * 1024u * 1024u)
#define NTEX 40
#define FLUSH_EVERY 250

static unsigned int sList[LIST_BYTES / 4] __attribute__((aligned(64)));
static uint8_t *sArena;
static unsigned sArenaUsed;
static unsigned sPrimsInList;
static int sDrawOff, sDispOff = 0x88000;

typedef struct {
    uint64_t key, texa;
    unsigned gen, clutGen;
    uint8_t *px;
    uint32_t *clut; /* 256 entries for an indexed texture, else NULL */
    int w, h, valid;
} Tex;
static Tex sTex[NTEX];
static int sTexCount;

typedef struct { float u, v; unsigned c; float x, y, z; } GuV;
#define GUVT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* the state the GE is in, so that a draw only sends what changed (cleared after a restart of the list) */
static struct {
    int valid, sx0, sy0, sx1, sy1, zte, zfunc, zmask, ate, afunc, aref, abe, bop, bsf, bdf;
    unsigned bsfix, bdfix;
    int tex, tfxrep, tcc, wu, wv, mag;
    const void *texptr, *clutptr;
} cur;

static void arena_reset(void) {
    sArenaUsed = 0;
    sTexCount = 0;
    memset(sTex, 0, sizeof(sTex));
    cur.valid = 0;
}

static void list_restart(void) {
    sceGuStart(GU_DIRECT, sList);
    sPrimsInList = 0;
    cur.valid = 0;
}

static void list_flush(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    list_restart();
}

int GsGpu_Init(void) {
    SceUID id;

    if (gPspGuActive) {
        return 1;
    }
    id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "bt3_tex", PSP_SMEM_Low, ARENA_BYTES + 64, NULL);
    if (id < 0) {
        return 0; /* gs_core.c then runs the software renderer */
    }
    sArena = (uint8_t *)(((uintptr_t)sceKernelGetBlockHeadAddr(id) + 63u) & ~63u);
    sceGuInit();
    sceGuStart(GU_DIRECT, sList);
    sceGuDrawBuffer(GU_PSM_8888, (void *)(intptr_t)sDrawOff, 512);
    sceGuDispBuffer(480, 272, (void *)(intptr_t)sDispOff, 512);
    sceGuDepthBuffer((void *)0x110000, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    list_restart();
    arena_reset();
    gPspPicture = 1; /* the debug screen's text stays off the picture */
    gPspGuActive = 1;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- textures */

static uint8_t *arena_alloc(unsigned n) {
    uint8_t *p;
    n = (n + 63u) & ~63u;
    if (sArenaUsed + n > ARENA_BYTES) {
        return NULL;
    }
    p = sArena + sArenaUsed;
    sArenaUsed += n;
    return p;
}

static unsigned pages_gen(uint32_t bp, unsigned bytes) {
    unsigned p0 = bp >> 5, n = (bytes + 8191u) / 8192u, i, g = 0;
    if (n == 0) {
        n = 1;
    }
    for (i = 0; i < n; i++) {
        g += gGsPageGen[(p0 + i) & 511];
    }
    return g;
}

static uint32_t fix_alpha(uint32_t c) { /* the PS2's alpha 0x80 is 1.0: 0..255 for the GE */
    uint32_t a = c >> 24;
    a = a >= 0x80 ? 255 : a * 2;
    return (c & 0x00FFFFFFu) | (a << 24);
}

static Tex *tex_get(uint64_t t0, uint64_t texa) {
    uint32_t tbp = (uint32_t)(t0 & 0x3FFF), tbw = (uint32_t)((t0 >> 14) & 0x3F), psm = (uint32_t)((t0 >> 20) & 0x3F);
    int w = 1 << ((t0 >> 26) & 15), h = 1 << ((t0 >> 30) & 15), idx = psm == 0x13 || psm == 0x14, i;
    uint64_t key = t0 & ~((7ULL << 34) | (7ULL << 61));
    unsigned bits = (unsigned)Gs_PsmBits(psm), gen, cgen = 0;
    uint32_t cbp = (uint32_t)((t0 >> 37) & 0x3FFF), cpsm = (uint32_t)((t0 >> 51) & 15), csa = (uint32_t)((t0 >> 56) & 31);
    Tex *t;
    unsigned long long t_start;

    if (w > 512 || h > 512 || w < 16) {
        gPspGuBigTex++;
        return NULL;
    }
    if (tbp == (uint32_t)(gGs.frame[0] & 0x1FF) * 32u || tbp == (uint32_t)(gGs.frame[1] & 0x1FF) * 32u) {
        gPspGuRtt++; /* the game draws with a frame buffer as the texture (an effect pass): not handled, the texture is whatever the GS memory holds */
    }
    gen = pages_gen(tbp, (unsigned)w * (unsigned)h * bits / 8u);
    if (idx) {
        cgen = pages_gen(cbp, 1024);
    }
    for (i = 0; i < sTexCount; i++) {
        if (sTex[i].key == key && sTex[i].texa == texa && sTex[i].gen == gen && sTex[i].clutGen == cgen) {
            gPspGuTexHits++;
            return sTex[i].valid ? &sTex[i] : NULL;
        }
    }
    t_start = sceKernelGetSystemTimeWide();
    if (sTexCount == NTEX) {
        list_flush();
        arena_reset();
    }
    t = &sTex[sTexCount];
    memset(t, 0, sizeof(*t));
    t->key = key; t->texa = texa; t->gen = gen; t->clutGen = cgen; t->w = w; t->h = h;
    t->px = arena_alloc((unsigned)w * (unsigned)h * (idx ? 1u : 4u));
    if (t->px == NULL) { /* full: the GE must be through with the old ones first */
        list_flush();
        arena_reset();
        t = &sTex[0];
        memset(t, 0, sizeof(*t));
        t->key = key; t->texa = texa; t->gen = gen; t->clutGen = cgen; t->w = w; t->h = h;
        t->px = arena_alloc((unsigned)w * (unsigned)h * (idx ? 1u : 4u));
    }
    sTexCount = (int)(t - sTex) + 1;
    if (t->px == NULL) {
        t->valid = 0;
        return NULL;
    }
    if (idx) {
        uint32_t raw[256], n;
        t->clut = (uint32_t *)arena_alloc(1024);
        if (t->clut == NULL || !Gs_VramReadIdx8(tbp, tbw, psm, (uint32_t)w, (uint32_t)h, t->px, (uint32_t)w)) {
            return NULL;
        }
        memset(raw, 0, sizeof(raw));
        Gs_VramReadRect(cbp, 1, cpsm == 0 ? 0 : cpsm, 16, 16, raw);
        for (n = 0; n < 256; n++) {
            uint32_t k = psm == 0x14 ? (csa * 16 + (n & 15)) & 255 : n;
            uint32_t p = cpsm == 0 ? ((k & 0xE7u) | ((k & 8u) << 1) | ((k & 16u) >> 1)) : k;
            t->clut[n] = fix_alpha(Gs_Expand(raw[p], cpsm));
        }
        sceKernelDcacheWritebackRange(t->clut, 1024);
        sceKernelDcacheWritebackRange(t->px, (unsigned)w * (unsigned)h);
    } else {
        uint32_t *o = (uint32_t *)t->px, n, total = (uint32_t)w * (uint32_t)h;
        Gs_VramReadRect(tbp, tbw, psm, (uint32_t)w, (uint32_t)h, o);
        for (n = 0; n < total; n++) {
            o[n] = fix_alpha(Gs_Expand(o[n], psm));
        }
        sceKernelDcacheWritebackRange(o, total * 4u);
    }
    t->valid = 1;
    gPspGuTexDecodes++;
    gPspGuTexUs += sceKernelGetSystemTimeWide() - t_start;
    return t;
}

/* ---------------------------------------------------------------------------------------------- state */

static int gu_cmp(int atst) {
    switch (atst) {
    case 0: return GU_NEVER;
    case 1: return GU_ALWAYS;
    case 2: return GU_LESS;
    case 3: return GU_LEQUAL;
    case 4: return GU_EQUAL;
    case 5: return GU_GEQUAL;
    case 6: return GU_GREATER;
    default: return GU_NOTEQUAL;
    }
}

/* the blend of ALPHA as the GE has it (the model: gs_gpu.c blend_of): coefficient of Cs and of Cd as 0 / 1 / C / -C / 1-C */
static void blend_state(uint64_t alpha, int *op, int *sf, int *df, unsigned *sfix, unsigned *dfix) {
    int A = (int)(alpha & 3), B = (int)((alpha >> 2) & 3), C = (int)((alpha >> 4) & 3), D = (int)((alpha >> 6) & 3), i, k[2];
    unsigned fixv = (unsigned)((alpha >> 32) & 0xFF) * 2u;
    int fc = C == 0 ? GU_SRC_ALPHA : C == 1 ? GU_DST_ALPHA : GU_FIX, fi = C == 0 ? GU_ONE_MINUS_SRC_ALPHA : C == 1 ? GU_ONE_MINUS_DST_ALPHA : GU_FIX;
    int f[2];
    unsigned fx[2];

    if (fixv > 255) {
        fixv = 255;
    }
    for (i = 0; i < 2; i++) {
        int c = (A == i) - (B == i), one = D == i;
        k[i] = c == 0 ? one : c > 0 ? (one ? 5 : 2) : (one ? 4 : 3);
        fx[i] = 0;
        if (k[i] == 0) { f[i] = GU_FIX; fx[i] = 0; }
        else if (k[i] == 1 || k[i] == 5) { f[i] = GU_FIX; fx[i] = 0xFFFFFFu; }
        else if (k[i] == 4) { f[i] = fi; fx[i] = (0xFFu - fixv) * 0x010101u; }
        else { f[i] = fc; fx[i] = fixv * 0x010101u; }
    }
    *sf = f[0]; *df = f[1]; *sfix = fx[0]; *dfix = fx[1];
    *op = k[0] == 3 ? GU_REVERSE_SUBTRACT : k[1] == 3 ? GU_SUBTRACT : GU_ADD;
}

#define CHG(field, val) ((cur.valid && cur.field == (val)) ? 0 : (cur.field = (val), 1))

/* Sets the GE up for the primitive; 0 = leave it out (drawn into a buffer other than the frame's own, or no usable texture). */
static int set_state(int ctx, int *textured, float *tw, float *th) {
    uint64_t prim = gGs.prim, t0 = gGs.tex0[ctx], test = gGs.test[ctx], scis = gGs.scissor[ctx], zbuf = gGs.zbuf[ctx];
    int tme = (int)((prim >> 4) & 1), abe = (int)((prim >> 6) & 1), zte = (int)((test >> 16) & 1), ate = (int)(test & 1);
    int fbp = (int)(gGs.frame[ctx] & 0x1FF);
    int x0, y0, x1, y1;
    Tex *tx = NULL;

    *textured = 0;
    if (gGsMainFbp >= 0 && fbp != gGsMainFbp) {
        gPspGuOtherTarget++;
        return 0;
    }
    if (!cur.valid) {
        sceGuEnable(GU_SCISSOR_TEST);
    }
    x0 = (int)(scis & 0x7FF) * 480 / 512; y0 = (int)((scis >> 32) & 0x7FF) * 272 / 448;
    x1 = ((int)((scis >> 16) & 0x7FF) + 1) * 480 / 512; y1 = ((int)((scis >> 48) & 0x7FF) + 1) * 272 / 448;
    if (x1 > 480) { x1 = 480; }
    if (y1 > 272) { y1 = 272; }
    if (cur.sx0 != x0 || cur.sy0 != y0 || cur.sx1 != x1 || cur.sy1 != y1 || !cur.valid) {
        cur.sx0 = x0; cur.sy0 = y0; cur.sx1 = x1; cur.sy1 = y1;
        sceGuScissor(x0, y0, x1 - x0 > 0 ? x1 - x0 : 1, y1 - y0 > 0 ? y1 - y0 : 1);
    }
    if (tme) {
        tx = tex_get(t0, gGs.texa);
        if (tx == NULL) {
            return 0;
        }
        *tw = (float)tx->w;
        *th = (float)tx->h;
    }
    /* depth */
    {
        int zt = (int)((test >> 17) & 3), zf = zt == 3 ? GU_GREATER : zt == 2 ? GU_GEQUAL : zt == 1 ? GU_ALWAYS : GU_NEVER, zm = (int)((zbuf >> 32) & 1);
        if (CHG(zte, zte)) { if (zte) { sceGuEnable(GU_DEPTH_TEST); } else { sceGuDisable(GU_DEPTH_TEST); } }
        if (zte) {
            if (CHG(zfunc, zf)) { sceGuDepthFunc(zf); }
            if (CHG(zmask, zm)) { sceGuDepthMask(zm); }
        }
    }
    /* alpha test */
    {
        int af = gu_cmp((int)((test >> 1) & 7)), ar = (int)((test >> 4) & 0xFF) * 2;
        if (ar > 255) { ar = 255; }
        if (CHG(ate, ate)) { if (ate) { sceGuEnable(GU_ALPHA_TEST); } else { sceGuDisable(GU_ALPHA_TEST); } }
        if (ate) {
            int chg = !cur.valid || cur.afunc != af || cur.aref != ar;
            if (chg) { cur.afunc = af; cur.aref = ar; sceGuAlphaFunc(af, ar, 0xFF); }
        }
    }
    /* blend */
    if (CHG(abe, abe)) { if (abe) { sceGuEnable(GU_BLEND); } else { sceGuDisable(GU_BLEND); } }
    if (abe) {
        int op, sf, df;
        unsigned sfix, dfix;
        blend_state(gGs.alpha[ctx], &op, &sf, &df, &sfix, &dfix);
        if (!cur.valid || cur.bop != op || cur.bsf != sf || cur.bdf != df || cur.bsfix != sfix || cur.bdfix != dfix) {
            cur.bop = op; cur.bsf = sf; cur.bdf = df; cur.bsfix = sfix; cur.bdfix = dfix;
            sceGuBlendFunc(op, sf, df, sfix, dfix);
        }
    }
    /* texture */
    if (CHG(tex, tme)) { if (tme) { sceGuEnable(GU_TEXTURE_2D); } else { sceGuDisable(GU_TEXTURE_2D); } }
    if (tme) {
        int tfx = (int)((t0 >> 35) & 3), tcc = (int)((t0 >> 34) & 1);
        uint64_t cl = gGs.clamp[ctx], t1 = gGs.tex1[ctx];
        int wu = (cl & 3) == 0 ? GU_REPEAT : GU_CLAMP, wv = ((cl >> 2) & 3) == 0 ? GU_REPEAT : GU_CLAMP, mag = (int)((t1 >> 5) & 1);
        if (tx->clut != NULL) {
            if (cur.texptr != tx->px || cur.clutptr != tx->clut || !cur.valid) {
                sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
                sceGuClutLoad(32, tx->clut);
                sceGuTexMode(GU_PSM_T8, 0, 0, 0);
                sceGuTexImage(0, tx->w, tx->h, tx->w, tx->px);
                cur.texptr = tx->px; cur.clutptr = tx->clut;
            }
        } else if (cur.texptr != tx->px || cur.clutptr != NULL || !cur.valid) {
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, tx->w, tx->h, tx->w, tx->px);
            cur.texptr = tx->px; cur.clutptr = NULL;
        }
        if (CHG(tfxrep, tfx == 1) | CHG(tcc, tcc)) { sceGuTexFunc(tfx == 1 ? GU_TFX_REPLACE : GU_TFX_MODULATE, tcc ? GU_TCC_RGBA : GU_TCC_RGB); }
        if (CHG(wu, wu) | CHG(wv, wv)) { sceGuTexWrap(wu, wv); }
        if (CHG(mag, mag)) { sceGuTexFilter(mag ? GU_LINEAR : GU_NEAREST, mag ? GU_LINEAR : GU_NEAREST); }
        *textured = 1;
    }
    cur.valid = 1;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- primitives */

static unsigned pack(const GsVertex *v, int textured, int decal) {
    unsigned r = v->r, g = v->g, b = v->b, a = v->a * 2u;
    if (textured) { /* modulate: Ct * Cc / 128 */
        r = decal ? 255 : r * 2u; g = decal ? 255 : g * 2u; b = decal ? 255 : b * 2u;
    }
    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }
    if (a > 255) { a = 255; }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static float zf(uint32_t z, uint64_t zbuf) {
    unsigned f = (unsigned)((zbuf >> 24) & 15);
    return f == 0 ? (float)(z >> 16) : f == 1 ? (float)(z >> 8) : (float)(z & 0xFFFF);
}

void GsGpu_Draw(int type, int ctx, const GsVertex *v) {
    int n = type == 6 ? 2 : type == 3 ? 3 : type == 1 ? 2 : 1, textured, i, fst = (int)((gGs.prim >> 8) & 1), gouraud = (int)((gGs.prim >> 3) & 1);
    int decal = (int)((gGs.tex0[ctx] >> 35) & 3) == 1;
    float tw = 1.0f, th = 1.0f, sx = 480.0f / 512.0f, sy = 272.0f / 448.0f;
    GuV *o;
    const GsVertex *flat = &v[n - 1];

    if (!set_state(ctx, &textured, &tw, &th)) {
        return;
    }
    if (type == 0 || type == 1) {
        textured = 0;
    }
    o = (GuV *)sceGuGetMemory((int)(sizeof(GuV) * (unsigned)n));
    for (i = 0; i < n; i++) {
        const GsVertex *c = (gouraud && type != 6) ? &v[i] : flat;
        o[i].x = v[i].x * sx;
        o[i].y = v[i].y * sy;
        o[i].z = zf(v[i].z, gGs.zbuf[ctx]);
        o[i].c = pack(c, textured, decal);
        if (textured) {
            if (fst) {
                o[i].u = (float)v[i].u / 16.0f / tw;
                o[i].v = (float)v[i].v / 16.0f / th;
            } else {
                float q = v[i].q != 0.0f ? v[i].q : 1.0f;
                o[i].u = v[i].s / q;
                o[i].v = v[i].t / q;
            }
        } else {
            o[i].u = o[i].v = 0.0f;
        }
    }
    if (type == 6) { /* two corners: the sprite is axis aligned, order them */
        if (o[0].x > o[1].x) { float t = o[0].x; o[0].x = o[1].x; o[1].x = t; t = o[0].u; o[0].u = o[1].u; o[1].u = t; }
        if (o[0].y > o[1].y) { float t = o[0].y; o[0].y = o[1].y; o[1].y = t; t = o[0].v; o[0].v = o[1].v; o[1].v = t; }
        sceGuDrawArray(GU_SPRITES, GUVT, 2, 0, o);
    } else if (type == 3) {
        sceGuDrawArray(GU_TRIANGLES, GUVT, 3, 0, o);
    } else if (type == 1) {
        sceGuDrawArray(GU_LINES, GUVT, 2, 0, o);
    } else {
        sceGuDrawArray(GU_POINTS, GUVT, 1, 0, o);
    }
    gPspGuPrims++;
    if (textured) { gPspGuTextured++; } else { gPspGuPlain++; }
    if (++sPrimsInList >= FLUSH_EVERY) {
        list_flush();
    }
}

/* The vertex programs as shaders (the Vita's and the PC's way): not here; gs_vu1.c interprets them while gVglVuReady is 0. */
void GsGpu_DrawVu0(int layer, int ctx, const float *vertices, uint32_t count, const float *consts) { (void)layer; (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_DrawVu4(int ctx, const float *vertices, uint32_t count, const float *consts) { (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_DrawVu6(int ctx, const float *vertices, uint32_t count, const float *consts) { (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_FbUpload(int second) { (void)second; }
void GsGpu_Native(int effect) { (void)effect; }

/* The frame is finished: the GE through, the debug overlay on the buffer about to be shown, the buffers swapped. */
void GsGpu_FrameEnd(void) {
    if (!gPspGuActive) {
        return;
    }
    sceGuFinish();
    sceGuSync(0, 0);
    if (gPspDebug) {
        Psp_DebugOverlay(sDrawOff);
    }
    sceGuSwapBuffers();
    { int t = sDrawOff; sDrawOff = sDispOff; sDispOff = t; }
    list_restart();
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    gPspGuFrames++;
}
