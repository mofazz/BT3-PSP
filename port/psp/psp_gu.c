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
#include "psp_pack.h"

extern int gGsMainFbp;
extern uint32_t gGsPageSig[512]; /* gs_core.c: bumped only when a page's content really changes */
extern volatile int gPspPicture;
extern void Psp_DebugOverlay(int vramOffset);
extern void Psp_RamMeter(int vramOffset);
extern int gPspRamMeter;
extern volatile int gPspDebug;

int gVglSafe = 32; /* gs_core.c: 32 = every upload written into GS memory at once (so that the textures can be read) */
volatile int gPspGuActive;
unsigned gPspGuRtt, gPspGuTextured, gPspGuPlain, gPspGuFrames, gPspGuPrims, gPspGuTexDecodes, gPspGuTexHits, gPspGuOtherTarget, gPspGuVuIgnored, gPspGuBigTex;
unsigned long long gPspGuTexUs, gPspGeWaitUs; /* (GE: the CPU waiting for the GPU to finish a list) */

#define LIST_BYTES (192 * 1024)
#define ARENA_BYTES (1024u * 1024u) /* 512 KB of it from the slack the log shows unused */
#define NTEX 384
#define FLUSH_EVERY 250

static unsigned int sList[LIST_BYTES / 4] __attribute__((aligned(64)));
static uint8_t *sArena;
extern void *Psp_VolatileAlloc(uint32_t n);
static unsigned sArenaUsed;
static unsigned sPrimsInList, sListUsed; /* draws and bytes in the GE list since it started */
static int sDrawOff, sDispOff = 0x88000;

typedef struct {
    uint64_t key, texa;
    unsigned gen, clutGen;
    uint8_t *px;
    uint32_t *clut; /* 256 entries for an indexed texture, else NULL */
    int w, h, valid, swz; /* swz: stored in the GE's swizzled order (16-byte x 8-row blocks) */
    uint32_t tbp, bytes, cbp;
    unsigned kgen;  /* what the entry is found by, with key: 0xFFFFFFFF = key names the content (an upload's number), else gen */
    uint32_t ckey;  /* the palette's place and format it was read with */
    int gsclut;     /* a pack texture whose palette is the GS's (read again when that changes) */
    int used, chunk; /* the entry is taken; the piece of texture memory its pixels are in */
    int t4, uw, uh; /* t4: 4-bit indices; uw, uh: the size the UVs address (a pack texture is smaller than the PS2's) */
    float su, sv;   /* UV scale: uw / the PS2 width */
} Tex;
static float sSU = 1.0f, sSV = 1.0f;

/* bt3psp.tex: "the texture uploaded at block dbp is pack entry idx" (GsGpu_Native), valid while its pages stay as
   they were then */
static struct { uint32_t dbp, sig, bytes; int idx, valid; } sBind[128];
static int sBindNext;
/* "the texture uploaded at block dbp has the number hash" (GsGpu_UploadNote, from gs_core.c), valid while its pages stay
   as they were when it was first drawn with (sig, taken then: the note comes before the upload is written) */
static struct { uint32_t dbp, bytes, sig; uint64_t hash; int valid, fresh; } sUpBind[128];
static int sUpBindNext;
unsigned gPspGuClutRefresh, gPspGuTexRemade;

/* Optimisations measured on real PSPs by the NFSU2 port (its docs): a swizzled texture reads ~4x faster through the GE's
   8 KB texture cache, one draw of many vertices costs far less than many draws of three, and a 16-bit picture halves
   the fill cost and the VRAM. nofast.txt in the game's folder switches the first two off, fb16.txt the third on. */
static int sSwizzle = 1, sBatching = 1, sFb16;
int gPspUpMemo = 1; /* gs_core.c: unchanged uploads are not written into GS memory again (upfull.txt: every one is, as before) */
extern volatile int gPspSect; /* psp_main.c: the profile (10 drawing, 11 a texture made, 12 the pack read, 13 the GE waited for) */
unsigned gPspGuArenaResets;
static unsigned sFbBytes = 0x88000;
unsigned gPspGuDraws, gPspGsKicks, gPspGuNoTex, gPspGuFbUploads;
static Tex sTex[NTEX];
static int sTexCount;

typedef struct { float u, v; unsigned c; float x, y, z; } GuV;
#define GUVT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* the state the GE is in, so that a draw only sends what changed (cleared after a restart of the list) */
static struct {
    int valid, sx0, sy0, sx1, sy1, zte, zfunc, zmask, ate, afunc, aref, abe, bop, bsf, bdf;
    unsigned bsfix, bdfix;
    int tex, tfxrep, tcc, wu, wv, mag;
    unsigned fbmsk;
    const void *texptr, *clutptr;
} cur;

/* ---- texture memory. Several pieces ("chunks"): the first is the port's own megabyte, the second the VRAM the frame
   buffers leave free (688 KB on a PSP-1000), the others 512 KB each of free game-heap memory, taken while the game has
   room to spare and given back when it asks (Psp_GuTrim, from Heap_Alloc). When everything is full only the piece used
   least recently is emptied: one megabyte emptied whole, several times a picture, had the pack's textures read from
   the Memory Stick again and again (40-58% of the CPU in the menus with many textures). */
#define NCHUNK 10
#define HCHUNK (512u << 10)
#define HCHUNK_MAX 6
#define HRESERVE (2048u << 10) /* the largest free block the game keeps after a piece is taken */
typedef struct { uint8_t *base; unsigned size, used, stamp; void *hblk; } Chunk;
static Chunk sChunk[NCHUNK];
static int sChunkN, sChunkFixed; /* sChunkFixed: the pieces that are not the game heap's (they come first) */
static unsigned sTick;
static int sHeapChunksOff;
unsigned gPspGuTexBytes, gPspGuHeapChunks;
extern void *Heap_AllocBlock(int size, unsigned align, int fromTail, int heap);
extern void Heap_FreeBlock(void *block);
extern unsigned Heap_GetLargestFree(void);
static void list_flush(void);

static void arena_reset(void) {
    int c;
    gPspGuArenaResets++;
    for (c = 0; c < sChunkN; c++) {
        sChunk[c].used = 0;
    }
    sTexCount = 0;
    memset(sTex, 0, sizeof(sTex));
    cur.valid = 0;
}

/* Empties one piece: the GE through with it first, then its textures forgotten. */
static void chunk_evict(int c) {
    int i;
    list_flush();
    for (i = 0; i < sTexCount; i++) {
        if (sTex[i].used && sTex[i].chunk == c) {
            memset(&sTex[i], 0, sizeof(sTex[i]));
        }
    }
    sChunk[c].used = 0;
    gPspGuArenaResets++;
}

static int lru_chunk(unsigned minSize) {
    int c, best = -1;
    for (c = 0; c < sChunkN; c++) {
        if (sChunk[c].size >= minSize && (best < 0 || sChunk[c].stamp < sChunk[best].stamp)) {
            best = c;
        }
    }
    return best;
}

static int heap_chunk_add(void) {
    void *b;
    if (sHeapChunksOff || sChunkN >= NCHUNK || sChunkN - sChunkFixed >= HCHUNK_MAX || Heap_GetLargestFree() < HCHUNK + HRESERVE) {
        return 0;
    }
    b = Heap_AllocBlock((int)HCHUNK, 0x40, 1, 2);
    if (b == NULL) {
        return 0;
    }
    sChunk[sChunkN].base = (uint8_t *)(uintptr_t)((uint32_t *)b)[5]; /* HeapBlock.data */
    sChunk[sChunkN].size = HCHUNK;
    sChunk[sChunkN].used = 0;
    sChunk[sChunkN].stamp = 0;
    sChunk[sChunkN].hblk = b;
    sChunkN++;
    gPspGuHeapChunks = (unsigned)(sChunkN - sChunkFixed);
    return 1;
}

/* Heap_Alloc (src/sys/heap.c) could not serve the game: the game heap's pieces go back. Returns how many. */
int Psp_GuTrim(void) {
    int n = sChunkN - sChunkFixed, i;
    if (!gPspGuActive || n <= 0) {
        return 0;
    }
    list_flush();
    while (sChunkN > sChunkFixed) {
        int c = sChunkN - 1;
        for (i = 0; i < sTexCount; i++) {
            if (sTex[i].used && sTex[i].chunk == c) {
                memset(&sTex[i], 0, sizeof(sTex[i]));
            }
        }
        Heap_FreeBlock(sChunk[c].hblk);
        memset(&sChunk[c], 0, sizeof(sChunk[c]));
        sChunkN--;
    }
    gPspGuHeapChunks = 0;
    return n;
}

static void list_restart(void) {
    sceGuStart(GU_DIRECT, sList);
    sPrimsInList = 0;
    sListUsed = 0;
    cur.valid = 0;
}

/* ---------------------------------------------------------------------------------------------- batching */

#define BATCH_MAX 384 /* vertices; a multiple of 2 and of 3 */
static GuV sBatch[BATCH_MAX];
static int sBatchN, sBatchPrim = -1;
/* what the batch was drawn with: a primitive may join only while every register it reads is the same */
static struct { uint64_t prim, tex0, test, scis, zbuf, alpha, clamp, tex1, texa, frame; int ctx, type; const void *tex; } sBkey;

static void batch_flush(void) {
    void *o;
    if (sBatchN == 0) {
        return;
    }
    o = sceGuGetMemory((int)(sizeof(GuV) * (unsigned)sBatchN));
    memcpy(o, sBatch, sizeof(GuV) * (unsigned)sBatchN);
    sceGuDrawArray(sBatchPrim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, sBatchN, 0, o);
    gPspGuDraws++;
    sPrimsInList++;
    sListUsed += (unsigned)(sizeof(GuV) * (unsigned)sBatchN) + 256u; /* the vertices and, roughly, the state before them */
    sBatchN = 0;
    sBatchPrim = -1;
}

static void list_flush(void) {
    batch_flush();
    sceGuFinish();
    { unsigned long long w0 = sceKernelGetSystemTimeWide(); int s_ = gPspSect; gPspSect = 13; sceGuSync(0, 0); gPspSect = s_; gPspGeWaitUs += sceKernelGetSystemTimeWide() - w0; }
    list_restart();
}

int GsGpu_Init(void) {
    SceUID id;

    if (gPspGuActive) {
        return 1;
    }
    sArena = (uint8_t *)Psp_VolatileAlloc(ARENA_BYTES); /* only if the volatile memory has room after the GS memory */
    if (sArena == NULL) {
        id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "bt3_tex", PSP_SMEM_Low, ARENA_BYTES + 64, NULL);
        if (id < 0) {
            return 0; /* gs_core.c then runs the software renderer */
        }
        sArena = (uint8_t *)(((uintptr_t)sceKernelGetBlockHeadAddr(id) + 63u) & ~63u);
    }
    {
        SceUID f = sceIoOpen("ms0:/PSP/GAME/BT3PSP/nofast.txt", PSP_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); sSwizzle = 0; sBatching = 0; }
        f = sceIoOpen("ms0:/PSP/GAME/BT3PSP/fb16.txt", PSP_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); sFb16 = 1; }
        f = sceIoOpen("ms0:/PSP/GAME/BT3PSP/upfull.txt", PSP_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); gPspUpMemo = 0; }
    }
    sFbBytes = sFb16 ? 0x44000u : 0x88000u; /* 512 x 272 pixels of 2 or 4 bytes */
    sChunk[0].base = sArena; sChunk[0].size = ARENA_BYTES;
    sChunkN = 1;
    {   /* the VRAM behind the two frame buffers and the depth buffer (novram.txt: not used) */
        SceUID f = sceIoOpen("ms0:/PSP/GAME/BT3PSP/novram.txt", PSP_O_RDONLY, 0);
        unsigned off = sFbBytes * 2u + 0x44000u, size = sceGeEdramGetSize();
        if (f >= 0) {
            sceIoClose(f);
        } else if (size >= off + 0x20000u) {
            sChunk[1].base = (uint8_t *)sceGeEdramGetAddr() + off;
            sChunk[1].size = (size - off) & ~63u;
            sChunkN = 2;
        }
        f = sceIoOpen("ms0:/PSP/GAME/BT3PSP/noheaptex.txt", PSP_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); sHeapChunksOff = 1; }
    }
    sChunkFixed = sChunkN;
    sDrawOff = 0;
    sDispOff = (int)sFbBytes;
    sceGuInit();
    sceGuStart(GU_DIRECT, sList);
    sceGuDrawBuffer(sFb16 ? GU_PSM_5650 : GU_PSM_8888, (void *)(intptr_t)sDrawOff, 512);
    sceGuDispBuffer(480, 272, (void *)(intptr_t)sDispOff, 512);
    sceGuDepthBuffer((void *)(intptr_t)(sFbBytes * 2u), 512);
    if (sFb16) {
        static ScePspIMatrix4 dith = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
        sceGuSetDither(&dith);
        sceGuEnable(GU_DITHER); /* 16 bits without bands in the PS2's smooth gradients */
    }
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
    { unsigned long long w0 = sceKernelGetSystemTimeWide(); int s_ = gPspSect; gPspSect = 13; sceGuSync(0, 0); gPspSect = s_; gPspGeWaitUs += sceKernelGetSystemTimeWide() - w0; }
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    list_restart();
    arena_reset();
    gPspPicture = 1; /* the debug screen's text stays off the picture */
    gPspGuActive = 1;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- textures */

static uint8_t *arena_alloc(unsigned n, int *chunk) {
    uint8_t *p;
    int c;
    n = (n + 63u) & ~63u;
    for (c = 0; c < sChunkN; c++) {
        if (sChunk[c].used + n <= sChunk[c].size) {
            break;
        }
    }
    if (c == sChunkN) {
        if (heap_chunk_add() && n <= sChunk[sChunkN - 1].size) {
            c = sChunkN - 1;
        } else {
            c = lru_chunk(n);
            if (c < 0) {
                return NULL; /* larger than any piece */
            }
            chunk_evict(c);
        }
    }
    p = sChunk[c].base + sChunk[c].used;
    sChunk[c].used += n;
    *chunk = c;
    return p;
}

/* A new texture of `bytes` bytes: an entry of the table and its memory, or NULL. */
static Tex *tex_new(unsigned bytes) {
    Tex *t = NULL;
    uint8_t *p;
    int i, c = 0, pass;
    for (pass = 0; pass < 2 && t == NULL; pass++) {
        for (i = 0; i < sTexCount; i++) {
            if (!sTex[i].used) {
                t = &sTex[i];
                break;
            }
        }
        if (t == NULL && sTexCount < NTEX) {
            t = &sTex[sTexCount++];
        }
        if (t == NULL) { /* the table is full: the piece used least recently makes room */
            c = lru_chunk(0);
            if (c < 0) {
                return NULL;
            }
            chunk_evict(c);
        }
    }
    if (t == NULL) {
        return NULL;
    }
    p = arena_alloc(bytes, &c);
    memset(t, 0, sizeof(*t));
    if (p == NULL) {
        return NULL;
    }
    t->used = 1;
    t->chunk = c;
    t->px = p;
    sChunk[c].stamp = ++sTick;
    gPspGuTexBytes += (bytes + 63u) & ~63u;
    return t;
}

static unsigned pages_gen(uint32_t bp, unsigned bytes) {
    unsigned p0 = bp >> 5, n = (bytes + 8191u) / 8192u, i, g = 0;
    if (n == 0) {
        n = 1;
    }
    for (i = 0; i < n; i++) {
        g += gGsPageSig[(p0 + i) & 511]; /* (Sig, not Gen: Gen is bumped by every re-upload of the same texture, every frame) */
    }
    return g;
}

static uint32_t fix_alpha(uint32_t c) { /* the PS2's alpha 0x80 is 1.0: 0..255 for the GE */
    uint32_t a = c >> 24;
    a = a >= 0x80 ? 255 : a * 2;
    return (c & 0x00FFFFFFu) | (a << 24);
}

/* Rewrites a texture into the GE's swizzled order in place: 8 rows at a time, which fill the same bytes in both orders.
   pitch (bytes per row) must be a multiple of 16 and h of 8. */
static void swizzle(uint8_t *p, unsigned pitch, unsigned h) {
    static uint32_t band[512 * 8] __attribute__((aligned(16))); /* 8 rows of up to 2048 bytes */
    unsigned y, bx, r, blocks = pitch / 16u;
    for (y = 0; y < h; y += 8) {
        uint8_t *rows = p + y * pitch;
        memcpy(band, rows, pitch * 8u);
        for (bx = 0; bx < blocks; bx++) {
            for (r = 0; r < 8; r++) {
                memcpy(rows + (bx * 8u + r) * 16u, (const uint8_t *)band + r * pitch + bx * 16u, 16);
            }
        }
    }
}

/* The palette the GS holds for an indexed texture, as the GE takes it (256 entries, alpha 0..255). */
static void clut_read(uint32_t *out, uint32_t psm, uint32_t cbp, uint32_t cpsm, uint32_t csa) {
    uint32_t raw[256], n;
    memset(raw, 0, sizeof(raw));
    Gs_VramReadRect(cbp, 1, cpsm == 0 ? 0 : cpsm, 16, 16, raw);
    for (n = 0; n < 256; n++) {
        uint32_t k = psm == 0x14 ? (csa * 16 + (n & 15)) & 255 : n;
        uint32_t p = cpsm == 0 ? ((k & 0xE7u) | ((k & 8u) << 1) | ((k & 16u) >> 1)) : k;
        out[n] = fix_alpha(Gs_Expand(raw[p], cpsm));
    }
    sceKernelDcacheWritebackRange(out, 1024);
}

static Tex *pack_tex(int pi, uint32_t tbp, unsigned gen, unsigned cgen, uint32_t psm, uint32_t cbp, uint32_t cpsm, uint32_t csa, unsigned bytes) {
    const PackEnt *e = &gPspPack[pi];
    uint64_t key = 0xFACE000000000000ULL | (uint64_t)(unsigned)pi;
    Tex *t;
    int i, indexed = e->fmt == 1 || e->fmt == 2;
    uint32_t ckey = cbp | cpsm << 14 | csa << 18 | psm << 24;
    for (i = 0; i < sTexCount; i++) { /* (by the pack entry alone: its pixels are the same wherever the game uploads it and however
                                         often that place was written; with the place and its count in the key every use was a new
                                         texture, read from the Memory Stick again: 55-69% of the CPU in the menus) */
        Tex *f = &sTex[i];
        if (!f->used || f->key != key) {
            continue;
        }
        if (!f->valid) {
            return NULL;
        }
        sChunk[f->chunk].stamp = ++sTick;
        if (f->gsclut && (f->clutGen != cgen || f->ckey != ckey)) {
            list_flush(); /* (the GE may still have draws with the palette as it was) */
            clut_read(f->clut, psm, cbp, cpsm, csa);
            f->ckey = ckey;
            gPspGuClutRefresh++;
        }
        f->gen = gen; f->clutGen = cgen; f->tbp = tbp; f->bytes = bytes; f->cbp = f->gsclut ? cbp : 0xFFFFFFFFu;
        gPspGuTexHits++;
        return f;
    }
    t = tex_new(e->bytes + (indexed ? 1024u : 0u));
    if (t == NULL) {
        return NULL;
    }
    t->key = key; t->gen = gen; t->clutGen = cgen; t->tbp = tbp; t->bytes = bytes; t->cbp = indexed ? cbp : 0xFFFFFFFFu;
    if (gPspPackFd < 0) {
        return NULL;
    }
    {
        int s_ = gPspSect, got;
        gPspSect = 12;
        sceIoLseek(gPspPackFd, (SceOff)gPspPackData + e->off, PSP_SEEK_SET);
        got = sceIoRead(gPspPackFd, t->px, e->bytes);
        gPspSect = s_;
        if (got != (int)e->bytes) {
            return NULL;
        }
    }
    if (e->fmt == 3) { /* its own palette in front */
        t->clut = (uint32_t *)t->px;
        t->px += 1024;
    } else if (indexed) {
        t->clut = (uint32_t *)(t->px + e->bytes);
        clut_read(t->clut, psm, cbp, cpsm, csa);
        t->gsclut = 1;
        t->ckey = ckey;
    }
    sceKernelDcacheWritebackRange(t->px, e->bytes);
    t->w = e->w; t->h = e->h; t->swz = e->swz; t->t4 = e->fmt == 2;
    t->uw = e->tw >> e->shift ? e->tw >> e->shift : 1;
    t->uh = e->th >> e->shift ? e->th >> e->shift : 1;
    t->su = (float)t->uw / (float)e->tw;
    t->sv = (float)t->uh / (float)e->th;
    t->valid = 1;
    gPspGuTexDecodes++;
    return t;
}

static Tex *tex_get(uint64_t t0, uint64_t texa) {
    uint32_t tbp = (uint32_t)(t0 & 0x3FFF), tbw = (uint32_t)((t0 >> 14) & 0x3F), psm = (uint32_t)((t0 >> 20) & 0x3F);
    int w = 1 << ((t0 >> 26) & 15), h = 1 << ((t0 >> 30) & 15), idx = psm == 0x13 || psm == 0x14, i;
    uint64_t key = t0 & ~((7ULL << 34) | (7ULL << 61));
    unsigned bits = (unsigned)Gs_PsmBits(psm), gen, cgen = 0, kgen, bytes;
    uint32_t cbp = (uint32_t)((t0 >> 37) & 0x3FFF), cpsm = (uint32_t)((t0 >> 51) & 15), csa = (uint32_t)((t0 >> 56) & 31);
    uint32_t ckey = cbp | cpsm << 14 | csa << 18 | psm << 24;
    Tex *t, *stale = NULL;
    unsigned long long t_start;

    if (w > 512 || h > 512 || w < 16) {
        gPspGuBigTex++;
        return NULL;
    }
    if (tbp == (uint32_t)(gGs.frame[0] & 0x1FF) * 32u || tbp == (uint32_t)(gGs.frame[1] & 0x1FF) * 32u) {
        gPspGuRtt++; /* the game draws with a frame buffer as the texture (an effect pass): not handled, the texture is whatever the GS memory holds */
    }
    bytes = (unsigned)w * (unsigned)h * bits / 8u;
    gen = pages_gen(tbp, bytes);
    if (idx) {
        cgen = pages_gen(cbp, 1024);
    }
    /* a texture of bt3psp.tex: converted on the PC, read from the Memory Stick (once: then cached) */
    for (i = 0; i < 128; i++) {
        if (sBind[i].valid && sBind[i].dbp == tbp && sBind[i].bytes == bytes && sBind[i].sig == gen) {
            return pack_tex(sBind[i].idx, tbp, gen, cgen, psm, cbp, cpsm, csa, bytes);
        }
    }
    /* What the texture is. The game uploads its textures to a few places of GS memory in turn, every frame, so "this
       place, written so many times" (kgen = gen) named a new texture each time and nothing was ever found again: the
       cache filled within a picture, was emptied, and the pack's textures were read from the Memory Stick again. When
       gs_core.c noted the upload that filled this place (GsGpu_UploadNote) and the place was not written since, the
       texture is known by that upload's number instead: the same content, the same name, in any frame. */
    kgen = gen;
    for (i = 0; i < 128; i++) {
        if (sUpBind[i].valid && sUpBind[i].dbp == tbp && sUpBind[i].bytes == bytes) {
            if (sUpBind[i].fresh) {
                sUpBind[i].sig = gen;
                sUpBind[i].fresh = 0;
            }
            if (sUpBind[i].sig == gen) {
                key = (sUpBind[i].hash ^ ((uint64_t)(psm | (unsigned)w << 8 | (unsigned)h << 20) * 0x9E3779B97F4A7C15ULL)) | 1u;
                kgen = 0xFFFFFFFFu;
            } else {
                sUpBind[i].valid = 0;
            }
            break;
        }
    }
    for (i = 0; i < sTexCount; i++) {
        Tex *f = &sTex[i];
        if (!f->used || f->key != key || f->texa != texa) {
            continue;
        }
        if (f->kgen == kgen) {
            if (!f->valid) {
                return NULL;
            }
            sChunk[f->chunk].stamp = ++sTick;
            if (idx && (f->clutGen != cgen || f->ckey != ckey)) { /* the same pixels with another palette: only the palette is read */
                list_flush(); /* (the GE may still have draws with the palette as it was) */
                clut_read(f->clut, psm, cbp, cpsm, csa);
                f->ckey = ckey;
                gPspGuClutRefresh++;
            }
            f->gen = gen; f->clutGen = cgen; f->tbp = tbp; f->bytes = bytes; f->cbp = idx ? cbp : 0xFFFFFFFFu;
            gPspGuTexHits++;
            return f;
        }
        if (f->valid && f->w == w && f->h == h && (f->clut != NULL) == (idx != 0)) {
            stale = f; /* the same place and size with other content: its memory is used again */
        }
    }
    t_start = sceKernelGetSystemTimeWide();
    if (stale != NULL) {
        uint8_t *px = stale->px;
        uint32_t *cl = stale->clut;
        int ch = stale->chunk;
        list_flush(); /* (the GE through with what it held) */
        t = stale;
        memset(t, 0, sizeof(*t));
        t->px = px;
        t->clut = cl;
        t->used = 1;
        t->chunk = ch;
        sChunk[ch].stamp = ++sTick;
        gPspGuTexRemade++;
    } else {
        /* (an indexed texture's palette lies behind its pixels, in the same piece) */
        t = tex_new(idx ? (((unsigned)w * (unsigned)h + 63u) & ~63u) + 1024u : (unsigned)w * (unsigned)h * 4u);
        if (t == NULL) {
            return NULL;
        }
        if (idx) {
            t->clut = (uint32_t *)(t->px + (((unsigned)w * (unsigned)h + 63u) & ~63u));
        }
    }
    t->key = key; t->texa = texa; t->kgen = kgen; t->gen = gen; t->clutGen = cgen; t->ckey = ckey; t->w = w; t->h = h;
    if (idx) {
        if (t->clut == NULL || !Gs_VramReadIdx8(tbp, tbw, psm, (uint32_t)w, (uint32_t)h, t->px, (uint32_t)w)) {
            return NULL;
        }
        clut_read(t->clut, psm, cbp, cpsm, csa);
        if (sSwizzle && (h & 7) == 0) {
            swizzle(t->px, (unsigned)w, (unsigned)h);
            t->swz = 1;
        }
        sceKernelDcacheWritebackRange(t->px, (unsigned)w * (unsigned)h);
    } else {
        uint32_t *o = (uint32_t *)t->px, n, total = (uint32_t)w * (uint32_t)h;
        Gs_VramReadRect(tbp, tbw, psm, (uint32_t)w, (uint32_t)h, o);
        for (n = 0; n < total; n++) {
            o[n] = fix_alpha(Gs_Expand(o[n], psm));
        }
        if (sSwizzle && (h & 7) == 0) {
            swizzle(t->px, (unsigned)w * 4u, (unsigned)h);
            t->swz = 1;
        }
        sceKernelDcacheWritebackRange(o, total * 4u);
    }
    t->tbp = tbp; t->bytes = bytes; t->cbp = idx ? cbp : 0xFFFFFFFFu;
    t->uw = w; t->uh = h; t->su = t->sv = 1.0f;
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
        { int s_ = gPspSect; gPspSect = 11; tx = tex_get(t0, gGs.texa); gPspSect = s_; }
        if (tx == NULL) {
            gPspGuNoTex++; /* left out: the texture could not be read */
            return 0;
        }
        *tw = (float)tx->uw;
        *th = (float)tx->uh;
        sSU = tx->su; sSV = tx->sv;
        sBkey.tex = tx;
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
    /* FRAME.FBMSK: bits the game keeps (a mask drawn into alpha alone, later read by a DST_ALPHA blend) */
    {
        unsigned m = (unsigned)(gGs.frame[ctx] >> 32);
        if (CHG(fbmsk, m)) { sceGuPixelMask(m); }
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
                sceGuTexMode(tx->t4 ? GU_PSM_T4 : GU_PSM_T8, 0, 0, tx->swz);
                sceGuTexImage(0, tx->w, tx->h, tx->w, tx->px);
                cur.texptr = tx->px; cur.clutptr = tx->clut;
            }
        } else if (cur.texptr != tx->px || cur.clutptr != NULL || !cur.valid) {
            sceGuTexMode(GU_PSM_8888, 0, 0, tx->swz);
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

static void draw_impl(int type, int ctx, const GsVertex *v);
void GsGpu_Draw(int type, int ctx, const GsVertex *v) {
    int s_ = gPspSect;
    gPspSect = 10;
    draw_impl(type, ctx, v);
    gPspSect = s_;
}

static void draw_impl(int type, int ctx, const GsVertex *v) {
    int n = type == 6 ? 2 : type == 3 ? 3 : type == 1 ? 2 : 1, textured, i, fst = (int)((gGs.prim >> 8) & 1), gouraud = (int)((gGs.prim >> 3) & 1);
    int decal = (int)((gGs.tex0[ctx] >> 35) & 3) == 1;
    float tw = 1.0f, th = 1.0f, sx = 480.0f / 512.0f, sy = 272.0f / 448.0f;
    GuV *o;
    const GsVertex *flat = &v[n - 1];

    {
        /* the same registers as the batch so far (and its texture not rewritten since): no state to send, join it */
        int same = sBatching && sBatchN > 0 && sBkey.type == type && sBkey.ctx == ctx && sBkey.prim == gGs.prim && sBkey.tex0 == gGs.tex0[ctx] &&
                   sBkey.test == gGs.test[ctx] && sBkey.scis == gGs.scissor[ctx] && sBkey.zbuf == gGs.zbuf[ctx] &&
                   sBkey.alpha == gGs.alpha[ctx] && sBkey.clamp == gGs.clamp[ctx] && sBkey.tex1 == gGs.tex1[ctx] &&
                   sBkey.texa == gGs.texa && sBkey.frame == gGs.frame[ctx] && sBatchN + n <= BATCH_MAX;
        if (same && ((gGs.prim >> 4) & 1)) {
            const Tex *bt = (const Tex *)sBkey.tex;
            same = bt != NULL && bt->valid && pages_gen(bt->tbp, bt->bytes) == bt->gen &&
                   (bt->cbp == 0xFFFFFFFFu || pages_gen(bt->cbp, 1024) == bt->clutGen);
            if (same) { textured = 1; tw = (float)bt->uw; th = (float)bt->uh; sSU = bt->su; sSV = bt->sv; }
        } else if (same) {
            textured = 0;
        }
        if (!same) {
            batch_flush();
            sBkey.tex = NULL;
            if (!set_state(ctx, &textured, &tw, &th)) {
                return;
            }
            sBkey.type = type; sBkey.ctx = ctx; sBkey.prim = gGs.prim; sBkey.tex0 = gGs.tex0[ctx]; sBkey.test = gGs.test[ctx];
            sBkey.scis = gGs.scissor[ctx]; sBkey.zbuf = gGs.zbuf[ctx]; sBkey.alpha = gGs.alpha[ctx]; sBkey.clamp = gGs.clamp[ctx];
            sBkey.tex1 = gGs.tex1[ctx]; sBkey.texa = gGs.texa; sBkey.frame = gGs.frame[ctx];
        }
    }
    if (type == 0 || type == 1) {
        textured = 0;
    }
    o = &sBatch[sBatchN];
    for (i = 0; i < n; i++) {
        const GsVertex *c = (gouraud && type != 6) ? &v[i] : flat;
        o[i].x = v[i].x * sx;
        o[i].y = v[i].y * sy;
        o[i].z = zf(v[i].z, gGs.zbuf[ctx]);
        o[i].c = pack(c, textured, decal);
        if (textured) {
            /* GU_TRANSFORM_2D: the GE takes texture coordinates in texels, not 0..1 */
            if (fst) {
                o[i].u = (float)v[i].u / 16.0f * sSU;
                o[i].v = (float)v[i].v / 16.0f * sSV;
            } else {
                float q = v[i].q != 0.0f ? v[i].q : 1.0f;
                o[i].u = v[i].s / q * tw;
                o[i].v = v[i].t / q * th;
            }
        } else {
            o[i].u = o[i].v = 0.0f;
        }
    }
    if (type == 6) { /* two corners: the sprite is axis aligned, order them */
        if (o[0].x > o[1].x) { float t = o[0].x; o[0].x = o[1].x; o[1].x = t; t = o[0].u; o[0].u = o[1].u; o[1].u = t; }
        if (o[0].y > o[1].y) { float t = o[0].y; o[0].y = o[1].y; o[1].y = t; t = o[0].v; o[0].v = o[1].v; o[1].v = t; }
    }
    sBatchPrim = type == 6 ? GU_SPRITES : type == 3 ? GU_TRIANGLES : type == 1 ? GU_LINES : GU_POINTS;
    sBatchN += n;
    gPspGuPrims++;
    if (textured) { gPspGuTextured++; } else { gPspGuPlain++; }
    if (!sBatching) {
        batch_flush();
    }
    if (sPrimsInList >= FLUSH_EVERY || sListUsed > LIST_BYTES * 3 / 4) {
        list_flush();
    }
}

/* The vertex programs as shaders (the Vita's and the PC's way): not here; gs_vu1.c interprets them while gVglVuReady is 0. */
void GsGpu_DrawVu0(int layer, int ctx, const float *vertices, uint32_t count, const float *consts) { (void)layer; (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_DrawVu4(int ctx, const float *vertices, uint32_t count, const float *consts) { (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_DrawVu6(int ctx, const float *vertices, uint32_t count, const float *consts) { (void)ctx; (void)vertices; (void)count; (void)consts; gPspGuVuIgnored++; }
void GsGpu_FbUpload(int second) { (void)second; gPspGuFbUploads++; } /* not drawn yet: counted for the log */
void GsGpu_Native(int effect) {
    extern uint32_t gGsMarkerArg;
    const PackEnt *e;
    uint32_t dbp, bytes;
    int i;
    if (effect != PACK_MARK || gGsMarkerArg >= (uint32_t)gPspPackN) {
        return;
    }
    batch_flush(); /* (draws before it read the place as it was) */
    e = &gPspPack[gGsMarkerArg];
    dbp = (uint32_t)(gGs.bitbltbuf >> 32) & 0x3FFF; /* the upload's place: the BITBLTBUF just written */
    bytes = (uint32_t)e->tw * e->th * (e->fmt == 2 ? 4u : e->fmt == 1 ? 8u : 32u) / 8u;
    for (i = 0; i < 128 && !(sBind[i].valid && sBind[i].dbp == dbp); i++) {
    }
    if (i == 128) {
        i = sBindNext;
        sBindNext = (sBindNext + 1) & 127;
    }
    sBind[i].dbp = dbp; sBind[i].bytes = bytes; sBind[i].idx = (int)gGsMarkerArg; sBind[i].valid = 1;
    { int k; for (k = 0; k < 128; k++) { if (sUpBind[k].dbp == dbp) { sUpBind[k].valid = 0; } } }
    sBind[i].sig = pages_gen(dbp, bytes);
}

/* Psp_FreeMemory (psp_mem.c), before a screen that needs room: every texture the GE keeps is forgotten (they are made
   again as they are drawn) and the pieces of game heap its texture memory took go back. Returns the bytes let go. */
unsigned Psp_GuFreeAll(void) {
    unsigned n = 0;
    int c;
    if (!gPspGuActive) {
        return 0;
    }
    list_flush(); /* (the GE through with them) */
    for (c = 0; c < sChunkN; c++) {
        n += sChunk[c].used;
    }
    arena_reset();
    while (sChunkN > sChunkFixed) {
        Heap_FreeBlock(sChunk[sChunkN - 1].hblk);
        memset(&sChunk[sChunkN - 1], 0, sizeof(sChunk[0]));
        sChunkN--;
    }
    gPspGuHeapChunks = 0;
    return n;
}

/* Vita_Fatal: where the picture on the screen is in VRAM (-1: the GE does not draw). The GE is through first. */
int Psp_GuDispOffset(void) {
    if (!gPspGuActive) {
        return -1;
    }
    sceGuFinish();
    sceGuSync(0, 0);
    list_restart();
    return sDispOff;
}

/* gs_core.c: a whole texture upload of `bytes` bytes with the number `hash` goes to block dbp (written, or found
   unchanged there). */
void GsGpu_UploadNote(uint32_t dbp, uint32_t bytes, uint64_t hash) {
    int i, k;
    if (!gPspGuActive) {
        return;
    }
    batch_flush(); /* (draws before it read the place as it was) */
    for (k = 0; k < 128; k++) { /* a pack texture bound to the place is over */
        if (sBind[k].valid && sBind[k].dbp == dbp) { sBind[k].valid = 0; }
    }
    for (i = 0; i < 128 && !(sUpBind[i].valid && sUpBind[i].dbp == dbp); i++) {
    }
    if (i == 128) {
        i = sUpBindNext;
        sUpBindNext = (sUpBindNext + 1) & 127;
    }
    sUpBind[i].dbp = dbp; sUpBind[i].bytes = bytes; sUpBind[i].hash = hash; sUpBind[i].valid = 1; sUpBind[i].fresh = 0;
    sUpBind[i].sig = pages_gen(dbp, bytes);
}

/* The frame is finished: the GE through, the debug overlay on the buffer about to be shown, the buffers swapped. */
void GsGpu_FrameEnd(void) {
    if (!gPspGuActive) {
        return;
    }
    batch_flush();
    sceGuFinish();
    { unsigned long long w0 = sceKernelGetSystemTimeWide(); int s_ = gPspSect; gPspSect = 13; sceGuSync(0, 0); gPspSect = s_; gPspGeWaitUs += sceKernelGetSystemTimeWide() - w0; }
    {
        extern int gVitaGsSkipR; /* a skipped frame drew nothing: keep showing the last picture, no swap */
        if (gVitaGsSkipR) {
            list_restart();
            return;
        }
    }
    if (gPspDebug && !sFb16) {
        Psp_DebugOverlay(sDrawOff);
    } else if (gPspRamMeter && !sFb16) {
        Psp_RamMeter(sDrawOff);
    }
    sceGuSwapBuffers();
    { int t = sDrawOff; sDrawOff = sDispOff; sDispOff = t; }
    list_restart();
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    gPspGuFrames++;
}
