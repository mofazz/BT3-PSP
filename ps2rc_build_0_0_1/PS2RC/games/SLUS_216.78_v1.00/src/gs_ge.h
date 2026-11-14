/* gs_ge.h - BT3 (SLUS-216.78 v1.00), PSP ONLY: the GS primitives of gs_soft.c drawn by the PSP's GE instead of the
 * software rasteriser. Included by gs_soft.c (it uses its registers, vertex queue, GS memory and pixel readers).
 * Written for this pack from the public descriptions of the PS2 GS (PS2Tek) and the PSP GE (pspsdk's pspgu.h).
 *
 * What stays in gs_soft.c: GIF / VIF1 parsing, the VU1 interpreter, the GS register model and the 4 MB GS memory
 * (image uploads still land there: textures are read from it).
 * What happens here:
 *   - render targets: each 512-pixel-wide 32-bit GS frame buffer (FRAME fbw 8, psm CT32/CT24) gets a 480x272 8888
 *     target in VRAM (two slots, by FRAME fbp; the vertices are scaled by 480/512 and 272/448); one 16-bit Z buffer.
 *     Draws into anything else (work buffers 256x256 and below, the depth buffer drawn as colour, 16-bit views) are
 *     skipped and counted.
 *   - the picture shown = the target of DISPFB2/1's fbp, at each vblank after anything was drawn: the open display
 *     list is finished and synced, then sceDisplaySetFrameBuf (8888, stride 512).
 *   - textures: decoded from GS memory (pix_read) into a per-frame arena in the guest code hole: T8 / T4 (and the
 *     T8H / T4HL / T4HH views) keep their indices with a 256 / 16 entry 8888 CLUT; 32 / 24 / 16-bit become 8888.
 *     PS2 alpha 0x80 = 1.0 is doubled (GE 0xFF = 1.0); vertex colours too (MODULATE: 0x80 = 1.0). Cache by TEX0 within
 *     the frame; image uploads bump a generation per 8 KB page, so a re-upload to the same address (the stage
 *     streams its textures through one address) converts again. A texture whose pages a GE draw wrote (the game's
 *     post-processing reads its frame / depth / work buffers) is skipped and counted.
 *   - states: PRIM (Gouraud, texture, blend, FST, context), TEST (alpha test, depth test), ZBUF.ZMSK, FRAME.FBMSK
 *     (sceGuPixelMask), ALPHA (the common (A-B)*C+D forms), TEX1 (filter), CLAMP (repeat / clamp), TEX0.TFX / TCC.
 * Not done: perspective-correct textures (through mode: s/q, t/q per vertex), DATE, AFAIL modes, fog, FBA, PABE, the
 * post-processing passes (render-to-texture), lines / points. */

#include <pspgu.h>
#include <pspkernel.h>
#include <malloc.h>
#include <pspdebug.h>
#include <pspsysmem.h>

#define GE_FB_STRIDE 512
#define GE_T_BYTES (GE_FB_STRIDE * 272 * 4)           /* 0x88000 */
#define GE_T_OFF(slot) (0x90000u + (uint32_t)(slot) * (GE_T_BYTES + 0x8000u))   /* 0x090000 / 0x120000: gaps of 32 KB after
   the display buffers and between the buffers (PPSSPP joins touching buffers: one target came out dark) */
#define GE_Z_OFF 0x1B0000u                            /* 16-bit Z (to 0x1F4000) */
#define GE_LIST_BYTES (320u << 10)                 /* battle: ~1900 3D batches a frame */
static uint32_t ge_arena_bytes = 640u << 10;
/* a block of the kernel's user partition (the C heap has no room for it), 64-byte aligned; NULL = none */
static int ge_opt(const char *k, int def);
void *ge_kalloc(const char *name, uint32_t bytes)
{
    SceUID id = sceKernelAllocPartitionMemory(2, name, PSP_SMEM_Low, bytes + 64, NULL);
    if (id < 0) { LOG("gs_ge: no %u KB of kernel memory for %s (%08x)\n", bytes >> 10, name, (unsigned)id); return NULL; }
    uintptr_t a = (uintptr_t)sceKernelGetBlockHeadAddr(id);
    return (void *)((a + 63) & ~(uintptr_t)63);
}          /* the texture arena: the rest of the code hole (ge_init) */
#define GE_ARENA_BYTES ge_arena_bytes
#define GE_BATCH 768                                  /* vertices per batched draw */

typedef struct { float u, v; uint32_t c; float x, y, z; } GeV;

static int ge_on = -1;                                /* -1 = not decided yet; ps2rc.cfg gs_ge=0 = software GS */
static uint32_t *ge_list;                             /* uncached view */
static uint8_t *ge_arena;
static uint32_t ge_arena_used;
static uint32_t ge_tfbp[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};   /* GS fbp (blocks / 32) of the two target slots */
static int ge_tlru;
static uint32_t ge_list_seq = 1;                     /* the open display list's number (a texture used in it stays) */
static int ge_dirty;                                  /* something drawn since the last flip */
static uint32_t ge_shown = 0xFFFFFFFFu;
static uint16_t ge_pgen[512];                         /* upload generation of every 8 KB page of GS memory */
static uint8_t ge_prt[512];                           /* 1 = the GE drew into this page (its content is in VRAM) */
static uint64_t ge_st_prims, ge_st_skip_target, ge_st_skip_rtt, ge_st_skip_tex, ge_st_tex_conv, ge_st_flushes,
    ge_st_draws, ge_st_flips, ge_st_blend_other, ge_st_arena_full;
static unsigned ge_frame_us_sum, ge_sync_us_sum;
static uint32_t ge_conv_us, ge_dma_us;                 /* CPU time: texture hash + conversion, GS DMA (incl. VU1) */
static uint64_t ge_st_tex_hashhit, ge_st_tex_reuse, ge_st_cache_clears;

/* the current batch: one GE draw for consecutive primitives with the same state */
static GeV ge_bv[GE_BATCH];
static int ge_bn, ge_bprim;                           /* GU_TRIANGLES / GU_SPRITES */
static uint64_t ge_bkey[10];                          /* the state the batch was set up with */

static void ge_list_restart(void);

static void *ge_arena_take(uint32_t bytes)
{
    uint32_t a = (ge_arena_used + 63u) & ~63u;
    if (a + bytes > GE_ARENA_BYTES) return NULL;
    ge_arena_used = a + bytes;
    return ge_arena + a;
}

static int ge_init(void)
{
    const char *e = hle_cfg_get("gs_ge");
    if (e && atoi(e) == 0) { LOG("gs_ge: off (ps2rc.cfg gs_ge=0): software GS\n"); return 0; }
    /* the C heap, not the guest code region's "hole": the game writes its code region as data once it runs (the
       host's code-region check: 1.8 MB changed), which overwrote the list and the cached textures (garbage stages) */
    uint8_t *lm = ge_kalloc("ge_list", GE_LIST_BYTES);
    ge_arena_bytes = 1024u << 10;
    ge_arena = ge_kalloc("ge_arena", GE_ARENA_BYTES);
    if (!lm || !ge_arena) { LOG("gs_ge: no memory in the code hole for the list / texture arena: software GS\n"); return 0; }
    sceKernelDcacheWritebackInvalidateRange(lm, GE_LIST_BYTES);
    ge_list = (uint32_t *)((uintptr_t)lm | 0x40000000u);
    sceGuInit();
    sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;
    sceGuDrawBuffer(GU_PSM_8888, (void *)GE_T_OFF(0), GE_FB_STRIDE);
    sceGuDepthBuffer((void *)GE_Z_OFF, GE_FB_STRIDE);
    /* 3D draws (native VU1 programs, gs_ge_vu1.h): pixel = ndc (viewport scale 1 at 2048, offset 2048),
       z = 32767.5 + 32767.5 ndc_z; identity view / model; the projection is set per batch */
    sceGuOffset(2048, 2048);
    sceGuViewport(2048, 2048, 2, -2);
    sceGuDepthRange(0, 65535);
    if (ge_opt("gs_geclip", 1)) sceGuEnable(GU_CLIP_PLANES);   /* gs_geclip=0 (experiment, 0.0.7): GE clipping off */
    else sceGuDisable(GU_CLIP_PLANES);
    {
        ScePspFMatrix4 id;
        memset(&id, 0, sizeof id);
        id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_VIEW, &id);
        sceGuSetMatrix(GU_MODEL, &id);
        sceGuSetMatrix(GU_PROJECTION, &id);
    }
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_DITHER);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);
    sceGuClearColor(0xFF203040u);                     /* self-test: the GE clears target 0, the CPU reads it back */
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    LOG("gs_ge: self-test clear -> target 0 pixel (10,10) = %08x (expected ff203040)\n",
        ((volatile uint32_t *)(0x44000000u + GE_T_OFF(0)))[10 * GE_FB_STRIDE + 10]);
    for (int sl = 0; sl < 2; sl++) {                   /* both targets start equal: black, alpha 0 (the game's
                                                         blends read the destination alpha it never rewrites) */
        sceGuStart(GU_DIRECT, ge_list);
        sceGuDrawBufferList(GU_PSM_8888, (void *)GE_T_OFF(sl), GE_FB_STRIDE);
        sceGuClearColor(0);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    }
    sceGuDisplay(GU_TRUE);
    sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;
    LOG("gs_ge: on - targets 2 x 480x272 8888 (VRAM %06x / %06x), Z16 %06x, list %u KB at %p, texture arena %u KB at %p\n",
        GE_T_OFF(0), GE_T_OFF(1), GE_Z_OFF, GE_LIST_BYTES >> 10, (void *)lm, GE_ARENA_BYTES >> 10, (void *)ge_arena);
    return 1;
}

/* pages [first, end) of a GS buffer of h lines */
static void ge_pages(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t h, uint32_t *p0, uint32_t *p1)
{
    uint32_t ph = (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S || psm == PSMT8) ? 64
                  : psm == PSMT4 ? 128 : 32;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = bw * 64 / pw;
    if (!prow) prow = 1;
    *p0 = bp / 32;
    *p1 = *p0 + ((h + ph - 1) / ph) * prow;
    if (*p1 > 512) *p1 = 512;
    if (*p0 > 512) *p0 = 512;
}

/* ------------------------------------------------------------------ texture cache, keyed by content
 * The game rotates its textures through a few GS addresses every frame (the title streams ~22 images through the
 * same pages), so "this address, written N times" made the same texture look new each time: it was never reused
 * and the cache filled with dead copies. Now:
 *   - every image upload gets a hash of its data (ge_upload_*: hashed while gs_soft.c writes it), and every 8 KB
 *     page the id of what filled it (a page wholly covered by one upload: that upload's hash and the page's place
 *     in it; a partial write: mixed into the page's previous id);
 *   - a texture's pixels are identified by its pages' ids, its offset in the first page and its format, NOT by its
 *     address: the same data uploaded elsewhere is reused;
 *   - the palette is a separate entry (1 KB), identified by its page's id and its CLUT fields: a palette change
 *     converts only the palette;
 *   - room: free arena space first, then the least recently used entry the open display list does not use (its GE
 *     work is done), converted again in place; only when neither exists is the list finished and the cache emptied.
 * A page the GE drew into (frame / depth / work buffers read back by the post-processing) is not decodable from GS
 * memory: such textures are skipped and counted. */
typedef struct { uint32_t id, fmt, fmt2, seq, cap, ver, chash; void *pix, *gepix; uint16_t w, h, bufw, ry0, ry1; uint8_t gpsm, ds; } GeTex;
typedef struct { uint32_t id, seq, ver; void *clut; } GeClut;
static uint32_t ge_ver;                              /* bumped by every conversion: part of the GE state key */
#define GE_NTEX 256
#define GE_NCLUT 128
static GeTex ge_tex[GE_NTEX];
static GeClut ge_clut[GE_NCLUT];
static int ge_ntex, ge_nclut;
static int ge_tex_full;                              /* the last ge_texture failed for room */
static const GeClut *ge_tclut;                       /* the palette of the texture ge_texture returned */
/* the texture and palette the GE state now points at (the last ge_setup): a list restart in the middle of a batch
   (list nearly full) keeps them "used by the open list", so no other lookup reuses their arena room in place before
   the GE has drawn with them (garbage textures in the battle) */
static GeTex *ge_bound_tex;
static GeClut *ge_bound_clut;
static uint32_t ge_pid[512];                         /* content id of every 8 KB page of GS memory */
static uint32_t ge_epoch = 1;                        /* bumped when page ids, dirtiness or GE marks change */
static uint8_t ge_pdirty[512];
static int ge_verify = -1;
static uint32_t ge_pchash[512];
static uint8_t ge_psnap[4][8192];                     /* gs_verify: copies of pages 0x150..0x153 at their last upload */                       /* gs_verify: page content right after its last upload */
static uint64_t ge_st_verify_bad;
static void ge_materialise(uint32_t p0, uint32_t p1);
static uint32_t ge_content_hash(uint32_t p0, uint32_t p1)   /* the pages' real content (deferred data written) */
{
    ge_materialise(p0, p1);
    uint32_t h = 0x811C9DC5u;
    for (uint32_t p = p0; p < p1 && p < 512; p++) {
        const uint32_t *w = (const uint32_t *)(gm + (size_t)p * 8192u);
        for (int k = 0; k < 2048; k++) h = (h ^ w[k]) * 0x01000193u;
    }
    return h;
}                       /* 1 = partly rewritten: the id is the content hash, made on use */
static uint64_t ge_st_page_hash;

/* a page's id: a partly rewritten page is hashed once from its content (palettes and small textures are uploaded in
   pieces smaller than a page; mixing each piece into the old id made identical data look new every frame) */
static void ge_materialise(uint32_t p0, uint32_t p1);
static uint32_t ge_page_id(uint32_t p)
{
    if (ge_pdirty[p]) {
        ge_materialise(p, p + 1);
        const uint32_t *w = (const uint32_t *)(gm + (size_t)p * 8192u);
        uint32_t h = 0x811C9DC5u;
        for (int k = 0; k < 2048; k++) h = (h ^ w[k]) * 0x01000193u;
        ge_pid[p] = h;
        ge_pdirty[p] = 0;
        ge_st_page_hash++;
    }
    return ge_pid[p];
}
static uint64_t ge_st_clut_conv, ge_st_tex_inplace;
/* ps2rc.cfg gs_tex_half (default 1): textures of 128 x 128 texels or more (gs_tex_half_min) are kept at half size
   per axis (ds = 1); the picture is shown at 480 x 272, so little is lost. Through-mode texel coordinates are
   divided by 2^ds (ge_vtx). */
static int ge_tex_half = -1, ge_tex_half_min = 128 * 128;
/* the rows the next draw samples (from its vertices' texture coordinates; ge_sprite / ge_tri): only that band of a
   texture is converted. Reset to "all rows" after each use. */
static int32_t ge_hint_lo = 0, ge_hint_hi = 0x7FFFFFFF;

static uint32_t ge_mix(uint32_t h, uint32_t v) { return (h ^ v) * 0x01000193u; }

/* ---- upload ids (called by gs_soft.c's image transfers, before and after GE init alike) */
static uint32_t gu_hash, gu_bp, gu_bw, gu_psm, gu_dx, gu_dy, gu_w, gu_h, gu_on, gu_hashed, gu_flux;
static uint64_t ge_st_up_skipped;                     /* bytes of uploads not written: GS memory already held them */
static void ge_upload_begin(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    gu_hash = 0x811C9DC5u;
    gu_bp = bp; gu_bw = bw; gu_psm = psm; gu_dx = dx; gu_dy = dy; gu_w = w; gu_h = h;
    gu_on = 1;
    gu_hashed = 0;
    gu_flux = 0;
}
/* the first data of an upload arrive: its pages are "unknown" until it ends (a texture read in between - the game
   sends an upload in pieces with draws between them - hashes their real content instead of trusting the old id) */
static void ge_upload_flux(void)
{
    gu_flux = 1;
    uint32_t psm = gu_psm, bw = gu_bw ? gu_bw : 1;
    uint32_t ph = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || psm == PSMT8H
                   || psm == PSMT4HL || psm == PSMT4HH) ? 32 : (psm == PSMT4) ? 128 : 64;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = bw * 64 / pw;
    if (!prow) prow = 1;
    uint32_t r0 = gu_dy / ph, r1 = (gu_dy + gu_h + ph - 1) / ph;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = 0; c < prow; c++) {
            uint32_t p = gu_bp / 32 + r * prow + c;
            if (p < 512) { ge_pdirty[p] = 1; ge_prt[p] = 0; }
        }
    ge_epoch++;
}
static void ge_upload_data(const uint8_t *q, uint32_t n)
{
    if (gu_on && !gu_flux) ge_upload_flux();
    if (!gu_on || gu_hashed) return;
    uint32_t h = gu_hash, k = 0;
    for (; k + 4 <= n; k += 4) { uint32_t w; memcpy(&w, q + k, 4); h = (h ^ w) * 0x01000193u; }
    for (; k < n; k++) h = (h ^ q[k]) * 0x01000193u;
    gu_hash = h;
}
/* the transfer is complete (or replaced by the next one): the pages it wrote get their ids */
static void ge_upload_end(void)
{
    if (!gu_on) return;
    gu_on = 0;
    uint32_t psm = gu_psm, bw = gu_bw ? gu_bw : 1;
    uint32_t ph = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || psm == PSMT8H
                   || psm == PSMT4HL || psm == PSMT4HH) ? 32 : (psm == PSMT4) ? 128 : 64;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = bw * 64 / pw;
    if (!prow) prow = 1;
    uint32_t r0 = gu_dy / ph, r1 = (gu_dy + gu_h + ph - 1) / ph;
    int fullw = gu_dx == 0 && gu_w >= bw * 64;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = 0; c < prow; c++) {
            uint32_t p = gu_bp / 32 + r * prow + c;
            if (p >= 512) continue;
            int fullh = r * ph >= gu_dy && (r + 1) * ph <= gu_dy + gu_h;
            uint32_t idx = (r - r0) * prow + c;
            if (fullw && fullh && (gu_bp & 31) == 0) { ge_pid[p] = ge_mix(ge_mix(gu_hash, idx), psm); ge_pdirty[p] = 0; ge_pwho[p] = 1; }
            else { ge_pdirty[p] = 1; ge_pwho[p] = 2; }
            ge_epoch++;
            if (ge_verify > 0) {
                ge_pchash[p] = ge_content_hash(p, p + 1);
                if (p >= 0x150 && p < 0x154) memcpy(ge_psnap[p - 0x150], gm + (size_t)p * 8192u, 8192);
            }
            ge_prt[p] = 0;
        }
}
/* the whole data of a transfer arrives in one piece (gif_block): hashed first; when every page it covers is wholly
   covered and already holds exactly this upload (same ids), the bytes are not written again (the game re-uploads
   the same textures every frame: the title sends ~900 KB a frame). 1 = skipped, the transfer is complete. */
static int ge_opt(const char *k, int def);
static int ge_upload_try_skip(const uint8_t *q, uint32_t n, uint32_t left)
{
    static int on = -1;
    if (on < 0) on = ge_opt("gs_skipsame", 1);
    if (!on || !gu_on) return 0;
    uint32_t psm = gu_psm;
    uint32_t bits = (psm == PSMCT32 || psm == PSMZ32) ? 32 : (psm == PSMCT24 || psm == PSMZ24) ? 24
                    : (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) ? 16
                    : (psm == PSMT8 || psm == PSMT8H) ? 8 : 4;
    if ((uint64_t)n * 8 < (uint64_t)left * bits) return 0;   /* only part of the transfer here */
    uint32_t h = 0x811C9DC5u, k = 0, nb = (uint32_t)(((uint64_t)left * bits + 7) / 8);
    for (; k + 4 <= nb; k += 4) { uint32_t w; memcpy(&w, q + k, 4); h = (h ^ w) * 0x01000193u; }
    for (; k < nb; k++) h = (h ^ q[k]) * 0x01000193u;
    gu_hash = h;
    gu_hashed = 1;
    uint32_t bw = gu_bw ? gu_bw : 1;
    uint32_t ph = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || psm == PSMT8H
                   || psm == PSMT4HL || psm == PSMT4HH) ? 32 : (psm == PSMT4) ? 128 : 64;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = bw * 64 / pw;
    if (!prow) prow = 1;
    {
        static int nlog;
        extern int gs_bench_logging(void);
        if (gs_bench_logging() && nlog < 40) {
            nlog++;
            LOG("upload: bp %x bw %u psm %x at %u,%u size %ux%u bytes %u hash %08x pageid %08x dirty %d\n", gu_bp,
                gu_bw, psm, gu_dx, gu_dy, gu_w, gu_h, nb, h, ge_pid[(gu_bp / 32) & 511], ge_pdirty[(gu_bp / 32) & 511]);
        }
    }
    if (gu_dx != 0 || gu_w < bw * 64 || (gu_bp & 31) || gu_dy % ph || gu_h % ph) return 0;
    uint32_t r0 = gu_dy / ph, r1 = (gu_dy + gu_h) / ph;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = 0; c < prow; c++) {
            uint32_t p = gu_bp / 32 + r * prow + c;
            if (p >= 512 || ge_pdirty[p] || ge_prt[p]) return 0;
            if (ge_pid[p] != ge_mix(ge_mix(h, (r - r0) * prow + c), psm)) return 0;
        }
    gu_on = 0;
    ge_st_up_skipped += nb;
    return 1;
}

/* ---- deferred uploads (PSP, GE on): the title streams ~900 KB of 32-bit uploads a frame through one GS address,
   and the texture cache serves nearly all of them by content without reading GS memory. A whole-page 32-bit upload
   that comes straight from guest RAM (GIF channel) is therefore not written: each page keeps where its data is
   (pointer + row stride) and is written into GS memory only when something reads it (a texture or palette
   conversion that misses the cache, a local copy) or before a partial write lands on it. */
static const uint8_t *ge_pend_src[512];               /* page data in guest RAM (64 x 32 texels, rows at stride) */
static uint32_t ge_pend_stride[512];
static uint64_t ge_st_up_deferred, ge_st_up_materialised;

static void ge_materialise(uint32_t p0, uint32_t p1)
{
    if (p1 > 512) p1 = 512;
    for (uint32_t p = p0; p < p1; p++) {
        const uint8_t *src = ge_pend_src[p];
        if (!src) continue;
        ge_pend_src[p] = NULL;
        uint8_t *page = gm + (size_t)p * 8192u;
        uint32_t st = ge_pend_stride[p];
        for (uint32_t y = 0; y < 32; y++) {
            const uint8_t *row = src + (size_t)y * st;
            const uint16_t *o = tab32 + y * 64;
            for (uint32_t x = 0; x < 64; x++) memcpy(page + o[x], row + 4 * x, 4);
        }
        ge_st_up_materialised += 8192;
    }
}

/* the end of a frame: deferred data still pending are written now. The game reuses its RAM buffers, so a pointer
   kept longer read other data later (garbage textures once the battle's resident textures were re-read). Data
   replaced within the frame (the title's streamed textures) are never written. */
static uint32_t ge_npend;
static void ge_materialise_all(void)
{
    if (!ge_npend) return;
    ge_materialise(0, 512);
    ge_npend = 0;
}

/* the whole transfer is here in guest RAM (gif_block, GIF channel): a page-aligned whole-width 32-bit upload is
   recorded instead of written (its pages get their ids as if written). 1 = deferred, the transfer is complete. */
static int ge_opt(const char *k, int def)            /* ps2rc.cfg switches for A/B checks (read once each) */
{
    const char *e = hle_cfg_get(k);
    return e ? atoi(e) : def;
}
static int ge_upload_defer(const uint8_t *q, uint32_t n, uint32_t left)
{
    static int on = -1;
    if (on < 0) on = ge_opt("gs_defer", 1);
    if (!on || !gu_on || ge_on <= 0 || gu_psm != PSMCT32) return 0;
    uint32_t bw = gu_bw ? gu_bw : 1;
    if (gu_dx != 0 || gu_w != bw * 64 || (gu_bp & 31) || gu_dy % 32 || gu_h % 32 || (uint64_t)n < (uint64_t)left * 4)
        return 0;
    if (!gu_hashed) {
        uint32_t h = 0x811C9DC5u;
        for (uint32_t k = 0; k + 4 <= left * 4; k += 4) { uint32_t w; memcpy(&w, q + k, 4); h = (h ^ w) * 0x01000193u; }
        gu_hash = h;
        gu_hashed = 1;
    }
    uint32_t r0 = gu_dy / 32, r1 = (gu_dy + gu_h) / 32, stride = gu_w * 4;
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t c = 0; c < bw; c++) {
            uint32_t p = gu_bp / 32 + r * bw + c;
            if (p >= 512) continue;
            ge_pend_src[p] = q + (size_t)(r - r0) * 32 * stride + (size_t)c * 256;
            ge_npend++;
            ge_pend_stride[p] = stride;
        }
    ge_upload_end();                                  /* the pages' ids, as for a written upload */
    ge_st_up_deferred += (uint64_t)left * 4;
    return 1;
}

/* before anything else writes into pages of GS memory: deferred data there goes in first */
static void ge_materialise_upload(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y, uint32_t h, uint32_t x,
                                  uint32_t w)
{
    uint32_t ph = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || psm == PSMT8H
                   || psm == PSMT4HL || psm == PSMT4HH) ? 32 : (psm == PSMT4) ? 128 : 64;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = (bw ? bw : 1) * 64 / pw;
    if (!prow) prow = 1;
    uint32_t r0 = y / ph, r1 = (y + h + ph - 1) / ph;
    /* a page the write covers wholly (32-bit, whole width, page-aligned rows) loses its old content anyway */
    int whole = psm == PSMCT32 && x == 0 && w >= (bw ? bw : 1) * 64 && (bp & 31) == 0;
    for (uint32_t r = r0; r < r1; r++) {
        if (whole && r * ph >= y && (r + 1) * ph <= y + h) {   /* old pending data dropped, not written later */
            for (uint32_t p = bp / 32 + r * prow; p < bp / 32 + (r + 1) * prow && p < 512; p++) ge_pend_src[p] = NULL;
            continue;
        }
        ge_materialise(bp / 32 + r * prow, bp / 32 + (r + 1) * prow);
    }
}

/* a local copy or anything else that writes pages without data to hash: new, unique ids */
static void ge_mark_upload(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y, uint32_t h)
{
    static uint32_t serial = 0x9E3779B9u;
    uint32_t ph = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || psm == PSMT8H
                   || psm == PSMT4HL || psm == PSMT4HH) ? 32 : (psm == PSMT4) ? 128 : 64;
    uint32_t pw = (psm == PSMT8 || psm == PSMT4) ? 128 : 64;
    uint32_t prow = bw * 64 / pw;
    if (!prow) prow = 1;
    uint32_t p0 = bp / 32 + (y / ph) * prow, p1 = bp / 32 + ((y + h + ph - 1) / ph) * prow;
    if (p1 > 512) p1 = 512;
    (void)serial;
    for (uint32_t p = p0; p < p1; p++) { ge_pdirty[p] = 1; ge_prt[p] = 0; ge_pwho[p] = 3; }
    ge_epoch++;
}

static uint32_t ge_a2(uint32_t c)                     /* alpha 0x80 = 1.0 -> 0xFF */
{
    uint32_t a = (c >> 24) * 2;
    return (c & 0xFFFFFFu) | ((a > 255 ? 255 : a) << 24);
}

/* least recently used entry not used by the open list, with room for bytes (-1 = none) */
static int ge_tex_victim(uint32_t bytes)
{
    int best = -1;
    for (int i = 0; i < ge_ntex; i++)
        if (ge_tex[i].seq != ge_list_seq && ge_tex[i].cap >= bytes && (best < 0 || ge_tex[i].seq < ge_tex[best].seq))
            best = i;
    return best;
}

static void ge_room_log(uint32_t tw, uint32_t th, uint32_t psm, uint32_t bytes)
{
    ge_st_arena_full++;
    ge_tex_full = 1;
    if (ge_st_arena_full <= 12 || ge_st_arena_full % 500 == 0) {
        uint32_t live = 0;
        for (int i = 0; i < ge_ntex; i++) if (ge_tex[i].seq == ge_list_seq) live++;
        LOG("gs_ge: texture room full (%llu): wanted %ux%u psm %x (%u bytes); %d textures (%u used by this list), "
            "arena %u of %u KB\n", (unsigned long long)ge_st_arena_full, tw, th, psm, bytes, ge_ntex, live,
            ge_arena_used >> 10, GE_ARENA_BYTES >> 10);
    }
}

/* the palette of an indexed texture (256 or 16 entries, 8888, alpha doubled) */
static const GeClut *ge_palette(uint64_t tex0, int t4)
{
    uint32_t cbp = (uint32_t)(tex0 >> 37) & 0x3FFF;
    if (clut_mode <= 0 && ge_prt[cbp / 32]) { ge_st_skip_rtt++; return NULL; }
    uint32_t id;
    if (clut_mode > 0) {                              /* the palette's content (CLUT buffer, TEXA for 16-bit) */
        id = ge_mix(0x811C9DC5u, (uint32_t)t4);
        for (int i = 0, n = t4 ? 16 : 256; i < n; i++) id = ge_mix(id, clut_lookup(tex0, (uint32_t)i, t4));
    } else {
        id = ge_mix(ge_mix(ge_mix(ge_page_id(cbp / 32), cbp & 31), (uint32_t)(tex0 >> 51)), (uint32_t)t4);
        if (((tex0 >> 55) & 1)) id = ge_mix(id, (uint32_t)reg[0x1C]);   /* CSM2: TEXCLUT */
    }
    for (int i = 0; i < ge_nclut; i++)
        if (ge_clut[i].id == id) { ge_clut[i].seq = ge_list_seq; return &ge_clut[i]; }
    GeClut *c = NULL;
    if (ge_nclut < GE_NCLUT) {
        void *m = ge_arena_take(256 * 4);
        if (m) { c = &ge_clut[ge_nclut++]; c->clut = m; }
    }
    if (!c) {
        int best = -1;
        for (int i = 0; i < ge_nclut; i++)
            if (ge_clut[i].seq != ge_list_seq && (best < 0 || ge_clut[i].seq < ge_clut[best].seq)) best = i;
        if (best < 0) { ge_room_log(16, 16, 0, 1024); return NULL; }
        c = &ge_clut[best];
    }
    c->id = id;
    c->seq = ge_list_seq;
    c->ver = ++ge_ver;
    if (clut_mode <= 0) ge_materialise(cbp / 32, cbp / 32 + 1);
    uint32_t *o = c->clut;
    int n = t4 ? 16 : 256;
    for (int i = 0; i < n; i++) o[i] = ge_a2(clut_lookup(tex0, (uint32_t)i, t4));
    sceKernelDcacheWritebackRange(o, (size_t)n * 4);
    ge_st_clut_conv++;
    return c;
}

static const GeTex *ge_texture_lookup(uint64_t tex0);
/* the last lookups: same TEX0 / TEXA / TEXCLUT / rows wanted, nothing written to GS memory since (epoch), the entry
   not reused since (its id) -> the same texture and palette, without recomputing ids or scanning the cache */
typedef struct { uint64_t tex0, texa, texclut; uint32_t epoch, id, cid; int32_t hlo, hhi; const GeTex *t; const GeClut *c; } GeMemo;
static GeMemo ge_memo[4];
static int ge_memo_next;
static uint64_t ge_st_memo_hits;
static const GeTex *ge_texture(uint64_t tex0)
{
    static int memo_on = -1;
    if (memo_on < 0) memo_on = ge_opt("gs_memo", 1);
    for (int i = 0; i < 4 && memo_on; i++) {
        GeMemo *m = &ge_memo[i];
        if (m->t && m->tex0 == tex0 && m->epoch == ge_epoch && m->texa == texa && m->texclut == reg[0x1C]
            && m->hlo == ge_hint_lo && m->hhi == ge_hint_hi && m->t->id == m->id && (!m->c || m->c->id == m->cid)) {
            ge_hint_lo = 0;
            ge_hint_hi = 0x7FFFFFFF;
            ((GeTex *)m->t)->seq = ge_list_seq;
            if (m->c) ((GeClut *)m->c)->seq = ge_list_seq;
            ge_tclut = m->c;
            ge_st_memo_hits++;
            return m->t;
        }
    }
    int32_t hlo = ge_hint_lo, hhi = ge_hint_hi;
    const GeTex *t = ge_texture_lookup(tex0);
    if (t) {
        GeMemo *m = &ge_memo[ge_memo_next];
        ge_memo_next = (ge_memo_next + 1) & 3;
        m->tex0 = tex0; m->texa = texa; m->texclut = reg[0x1C]; m->epoch = ge_epoch; m->hlo = hlo; m->hhi = hhi;
        m->t = t; m->id = t->id; m->c = ge_tclut; m->cid = ge_tclut ? ge_tclut->id : 0;
    }
    return t;
}

static const GeTex *ge_texture_lookup(uint64_t tex0)
{
    uint32_t tbp = (uint32_t)tex0 & 0x3FFF, tbw = (uint32_t)(tex0 >> 14) & 63, psm = (uint32_t)(tex0 >> 20) & 63;
    uint32_t tw = 1u << ((tex0 >> 26) & 15), th = 1u << ((tex0 >> 30) & 15);
    ge_tclut = NULL;
    if (tw > 512 || th > 512) { ge_st_skip_tex++; return NULL; }
    int t4 = psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH, t8 = psm == PSMT8 || psm == PSMT8H;
    uint32_t p0, p1;
    ge_pages(tbp, tbw ? tbw : 1, psm, th, &p0, &p1);
    uint32_t id = 0x811C9DC5u;
    for (uint32_t p = p0; p < p1; p++) {
        if (ge_prt[p]) { ge_st_skip_rtt++; return NULL; }
        id = ge_mix(id, ge_page_id(p));
    }
    if (ge_tex_half < 0) {
        const char *e = hle_cfg_get("gs_tex_half"), *m = hle_cfg_get("gs_tex_half_min");
        ge_tex_half = e ? atoi(e) : 1;
        if (m && atoi(m) > 0) ge_tex_half_min = atoi(m);
    }
    uint32_t ds = ge_tex_half && tw * th >= (uint32_t)ge_tex_half_min ? 1 : 0;
    uint32_t fmt = psm | tbw << 6 | ((uint32_t)(tex0 >> 26) & 0xFF) << 12 | (tbp & 31) << 20 | ds << 25;
    uint32_t fmt2 = (t4 || t8) ? 0 : (uint32_t)texa ^ (uint32_t)(texa >> 32);   /* 16 / 24-bit alpha from TEXA */
    if (t4 || t8) {
        ge_tclut = ge_palette(tex0, t4);
        if (!ge_tclut) return NULL;
    }
    uint32_t dw = tw >> ds, dh = th >> ds;
    uint32_t lo = 0, hi = dh;
    if (!ds && ge_hint_lo >= 0 && ge_hint_hi <= (int32_t)th && ge_hint_lo < ge_hint_hi) {
        lo = ge_hint_lo > 2 ? (uint32_t)ge_hint_lo - 2 : 0;
        hi = (uint32_t)ge_hint_hi + 3 < th ? (uint32_t)ge_hint_hi + 3 : th;
    }
    ge_hint_lo = 0;
    ge_hint_hi = 0x7FFFFFFF;
    static int nocache = -1;
    if (nocache < 0) nocache = ge_opt("gs_nocache", 0);
    for (int i = 0; i < ge_ntex && !nocache; i++) {
        GeTex *e = &ge_tex[i];
        if (e->id == id && e->fmt == fmt && e->fmt2 == fmt2 && e->ry0 <= lo && e->ry1 >= hi) {
            if (ge_verify < 0) { const char *v = hle_cfg_get("gs_verify"); ge_verify = v ? atoi(v) : 0; }
            if (ge_verify && ge_content_hash(p0, p1) != e->chash) {
                ge_st_verify_bad++;
                if (ge_st_verify_bad <= 30) {
                    char w[64];
                    int k = 0;
                    for (uint32_t q = p0; q < p1 && k < 60; q++) k += snprintf(w + k, sizeof w - k, "%d%s", ge_pwho[q], ge_pdirty[q] ? "d" : "");
                    for (uint32_t q = p0; q < p1 && q < 512; q++)
                        if (ge_content_hash(q, q + 1) != ge_pchash[q])
                        {
                            LOG("gs_ge: VERIFY   page %x changed since its last upload (writer %d)\n", q, ge_pwho[q]);
                            if (q >= 0x150 && q < 0x154) {
                                const uint8_t *o = ge_psnap[q - 0x150], *nw = gm + (size_t)q * 8192u;
                                int nd = 0, first = -1;
                                for (int b = 0; b < 8192; b += 4) if (memcmp(o + b, nw + b, 4)) { nd++; if (first < 0) first = b; }
                                uint32_t ov = 0, nv = 0;
                                if (first >= 0) { memcpy(&ov, o + first, 4); memcpy(&nv, nw + first, 4); }
                                LOG("gs_ge: VERIFY     %d words differ, first at +%x: was %08x now %08x (page host address %p)\n",
                                    nd, first, ov, nv, (void *)(gm + (size_t)q * 8192u));
                            }
                        }
                    LOG("gs_ge: VERIFY stale texture TEX0 %llx pages %x..%x writers %s id %08x\n",
                        (unsigned long long)tex0, p0, p1, w, id);
                }
            }
            e->seq = ge_list_seq;
            ge_st_tex_hashhit++;
            return e;
        }
    }
    static int idxd = -1;                             /* gs_idx_direct=1: indexed textures as direct colour (A/B) */
    if (idxd < 0) idxd = ge_opt("gs_idx_direct", 0);
    if (idxd) { t4 = 0; t8 = 0; }
    uint32_t bufw = dw < 32 ? 32 : dw;
    uint32_t rowb = t4 ? bufw / 2 : t8 ? bufw : bufw * 2;   /* direct colour: 16-bit */
    uint32_t bytes = rowb * (hi - lo);
    GeTex *t = NULL;
    void *pix = NULL;
    if (ge_ntex < GE_NTEX && (pix = ge_arena_take(bytes))) { t = &ge_tex[ge_ntex++]; t->cap = bytes; }
    else {
        static int vict = -1;                          /* gs_victim=0: no in-place reuse (A/B check) */
        if (vict < 0) { const char *e = hle_cfg_get("gs_victim"); vict = e ? atoi(e) : 1; }
        int v = vict ? ge_tex_victim(bytes) : -1;
        if (v < 0) { ge_room_log(tw, th, psm, bytes); return NULL; }
        t = &ge_tex[v];
        pix = t->pix;
        ge_st_tex_inplace++;
    }
    ge_materialise(p0, p1);                           /* deferred uploads this conversion reads */
    SceUInt32 c0 = sceKernelGetSystemTimeLow();
    t->id = id; t->fmt = fmt; t->fmt2 = fmt2; t->seq = ge_list_seq; t->pix = pix; t->ver = ++ge_ver;
    if (ge_verify > 0) t->chash = ge_content_hash(p0, p1);
    t->w = (uint16_t)dw; t->h = (uint16_t)dh; t->bufw = (uint16_t)bufw; t->ds = (uint8_t)ds;
    t->ry0 = (uint16_t)lo; t->ry1 = (uint16_t)hi;
    t->gepix = (uint8_t *)pix - (size_t)lo * rowb;          /* the GE addresses row 0; only rows lo..hi are sampled */
    ge_st_tex_conv++;
    if (t8) {
        t->gpsm = GU_PSM_T8;
        uint8_t *o = pix;
        for (uint32_t y = lo; y < hi; y++)
            for (uint32_t x = 0; x < dw; x++) o[(y - lo) * bufw + x] = (uint8_t)pix_read(tbp, tbw, psm, x << ds, y << ds);
    } else if (t4) {
        t->gpsm = GU_PSM_T4;
        uint8_t *o = pix;
        memset(o, 0, bytes);
        for (uint32_t y = lo; y < hi; y++)
            for (uint32_t x = 0; x < dw; x++)
                o[((y - lo) * bufw + x) >> 1] |= (uint8_t)((pix_read(tbp, tbw, psm, x << ds, y << ds) & 15) << ((x & 1) * 4));
    } else {
        /* direct colour as 16-bit: 5551 when every alpha is 0 or 1.0 (PS2 0 / >= 0x80), else 4444 */
        uint16_t *o = pix;
        cur_clamp = 0;
        int partial = 0;
        for (uint32_t y = lo; y < hi && !partial; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t a = tex_fetch(tex0, (int32_t)(x << ds), (int32_t)(y << ds)) >> 24;
                if (a && a < 0x7F) { partial = 1; break; }
            }
        t->gpsm = partial ? GU_PSM_4444 : GU_PSM_5551;
        for (uint32_t y = lo; y < hi; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t c = ge_a2(tex_fetch(tex0, (int32_t)(x << ds), (int32_t)(y << ds)));
                uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
                o[(y - lo) * bufw + x] = partial ? (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12))
                                                 : (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >= 0x80) << 15));
            }
    }
    ge_conv_us += sceKernelGetSystemTimeLow() - c0;
    sceKernelDcacheWritebackRange(pix, bytes);
    return t;
}

/* ------------------------------------------------------------------ batching and flushing */
static void ge_flush_batch(void)
{
    if (!ge_bn) return;
    if (sceGuCheckList() + (int)(ge_bn * sizeof(GeV)) + (int)(16u << 10) > (int)GE_LIST_BYTES) ge_list_restart();
    GeV *v = sceGuGetMemory((int)(ge_bn * sizeof(GeV)));
    memcpy(v, ge_bv, (size_t)ge_bn * sizeof(GeV));
    if (ge_st_draws >= 2000 && ge_st_draws < 2040)          /* diagnostics: a sample of draws in the log */
        LOG("gs_ge: draw %llu prim %d n %d v0 (%.1f %.1f %.0f) c %08x uv (%.1f %.1f) v1 (%.1f %.1f) key prim %llx "
            "frame %llx test %llx alpha %llx tex0 %llx scissor %llx\n", (unsigned long long)ge_st_draws, ge_bprim,
            ge_bn, ge_bv[0].x, ge_bv[0].y, ge_bv[0].z, ge_bv[0].c, ge_bv[0].u, ge_bv[0].v, ge_bv[1].x, ge_bv[1].y,
            (unsigned long long)ge_bkey[0], (unsigned long long)ge_bkey[1], (unsigned long long)ge_bkey[3],
            (unsigned long long)ge_bkey[6], (unsigned long long)ge_bkey[4], (unsigned long long)ge_bkey[8]);
    static int nodraw = -1;                           /* ps2rc.cfg gs_nodraw=1: everything but the draws (measuring) */
    if (nodraw < 0) { const char *e = hle_cfg_get("gs_nodraw"); nodraw = e ? atoi(e) : 0; }
    if (!nodraw) sceGuDrawArray(ge_bprim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, ge_bn, NULL, v);
    ge_bn = 0;
    ge_st_draws++;
    if (sceGuCheckList() > (int)(GE_LIST_BYTES - (32u << 10))) ge_list_restart();
}

/* the list is nearly full or the arena is: let the GE finish it, start a new one (states are re-sent) */
static void ge_list_restart(void)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;                                    /* the GE is idle: every cached texture stays valid */
    memset(ge_bkey, 0xFF, sizeof ge_bkey);
    ge_st_flushes++;
    static int clear = -1;                            /* gs_restart_clear=1: empty the caches too (A/B check) */
    if (clear < 0) { const char *e = hle_cfg_get("gs_restart_clear"); clear = e ? atoi(e) : 0; }
    if (clear) { ge_arena_used = 0; ge_ntex = 0; ge_nclut = 0; ge_bound_tex = NULL; ge_bound_clut = NULL; }
    if (ge_bound_tex) ge_bound_tex->seq = ge_list_seq;
    if (ge_bound_clut) ge_bound_clut->seq = ge_list_seq;
}

/* no room left in the texture arena: let the GE finish, then empty the caches */
static void ge_cache_clear(void)
{
    ge_list_restart();
    ge_arena_used = 0;
    ge_ntex = 0;
    ge_nclut = 0;
    ge_bound_tex = NULL;
    ge_bound_clut = NULL;
    ge_st_cache_clears++;
}

static int ge_blend(uint64_t al)
{
    uint32_t a = (uint32_t)al & 3, b = (uint32_t)(al >> 2) & 3, c = (uint32_t)(al >> 4) & 3, d = (uint32_t)(al >> 6) & 3;
    uint32_t fix = (uint32_t)(al >> 32) & 0xFF;
    uint32_t f2 = fix * 2 > 255 ? 255 : fix * 2, fixc = f2 | (f2 << 8) | (f2 << 16), inv = (255 - f2) * 0x010101u;
    int fs = c == 0 ? GU_SRC_ALPHA : c == 1 ? GU_DST_ALPHA : GU_FIX;
    int fsi = c == 0 ? GU_ONE_MINUS_SRC_ALPHA : c == 1 ? GU_ONE_MINUS_DST_ALPHA : GU_FIX;
    if (a == 0 && b == 1 && d == 1) { sceGuBlendFunc(GU_ADD, fs, fsi, fixc, inv); return 1; }      /* Cs*C + Cd*(1-C) */
    if (a == 0 && b == 2 && d == 1) { sceGuBlendFunc(GU_ADD, fs, GU_FIX, fixc, 0xFFFFFF); return 1; } /* Cs*C + Cd */
    if (a == 2 && b == 0 && d == 1) { sceGuBlendFunc(GU_REVERSE_SUBTRACT, fs, GU_FIX, fixc, 0xFFFFFF); return 1; } /* Cd - Cs*C */
    if (a == 1 && b == 0 && d == 0) { sceGuBlendFunc(GU_ADD, fsi, fs, inv, fixc); return 1; }      /* Cd*C + Cs*(1-C) */
    if (a == 0 && b == 2 && d == 2) { sceGuBlendFunc(GU_ADD, fs, GU_FIX, fixc, 0); return 1; }      /* Cs*C */
    if (a == 1 && b == 2 && d == 0) { sceGuBlendFunc(GU_ADD, GU_FIX, fs, 0xFFFFFF, fixc); return 1; } /* Cd*C + Cs */
    {                                                 /* not translated: logged once per ALPHA value */
        static uint64_t seen[16];
        static int ns;
        int k = 0;
        while (k < ns && seen[k] != al) k++;
        if (k == ns && ns < 16) { seen[ns++] = al; LOG("gs_ge: blend form not translated: ALPHA %llx\n", (unsigned long long)al); }
    }
    ge_st_blend_other++;
    return 0;
}

static float ge_sx = 480.0f / 512.0f, ge_sy = 272.0f / 448.0f;

/* the state of a primitive -> GE; returns the target slot, -1 = not drawn */
static int ge_setup(uint64_t p, int ctx, const GeTex **ptex, int prim_kind)
{
    uint64_t frame = reg[0x4C + ctx], zbuf = reg[0x4E + ctx], test = reg[0x47 + ctx], tex0 = reg[0x06 + ctx];
    uint32_t fbp = (uint32_t)frame & 0x1FF, fbw = (uint32_t)(frame >> 16) & 63, fpsm = (uint32_t)(frame >> 24) & 63;
    /* full-width 32-bit frame buffers only; not the depth buffer's memory drawn as colour with a write mask (the
       battle's alpha copies into the depth page's spare byte). A frame equal to ZBUF without a mask is an ordinary
       picture whose ZBUF register is simply left at 0 (title, menus) */
    int zcopy = fbp == ((uint32_t)zbuf & 0x1FF) && (uint32_t)(frame >> 32) != 0;
    if ((fbw != 8 && fbw != 10) || (fpsm != PSMCT32 && fpsm != PSMCT24) || zcopy) {
        ge_hint_lo = 0;                                  /* the row hint of a primitive not drawn must not reach the */
        ge_hint_hi = 0x7FFFFFFF;                         /* next texture lookup (a 3D batch got only a band of rows) */
        ge_st_skip_target++;
        static uint64_t seen[8];
        static int nseen;
        uint64_t f = frame & 0xFFFFFFFF3F3F01FFull;
        int k = 0;
        while (k < nseen && seen[k] != f) k++;
        if (k == nseen && nseen < 8) {
            seen[nseen++] = f;
            LOG("gs_ge: frame buffer not drawn: FRAME %llx (fbp %x fbw %u psm %x, ZBUF %llx)\n",
                (unsigned long long)frame, fbp, fbw, fpsm, (unsigned long long)zbuf);
        }
        static int mark = -1;                            /* gs_mark_skipped=0: only the depth-page copies */
        if (mark < 0) mark = ge_opt("gs_mark_skipped", 1);
        if (zcopy || mark) {                             /* those pages now hold a picture the GE did not draw: a
                                                            texture read from them is skipped, not stale memory */
            uint32_t q0, q1, sh = zcopy ? 448 : (uint32_t)((reg[0x40 + ctx] >> 48) & 0x7FF) + 1;   /* (scissor rows) */
            ge_pages(fbp * 32, fbw ? fbw : 1, fpsm, sh, &q0, &q1);
            for (uint32_t q = q0; q < q1; q++) if (!ge_prt[q]) { ge_prt[q] = 1; ge_epoch++; }
        }
        return -1;
    }
    {                                                 /* gs_skippsm=P: textured draws of texture format P not drawn (A/B) */
        static int sp = -2;
        if (sp == -2) { const char *e = hle_cfg_get("gs_skippsm"); sp = e ? (int)strtol(e, NULL, 16) : -1; }
        if (sp >= 0 && ((p >> 4) & 1) && (int)((tex0 >> 20) & 63) == sp) return -1;
    }
    ge_sx = 480.0f / (float)(fbw * 64);             /* 512 or 640 GS pixels -> 480 */
    int slot = ge_tfbp[0] == fbp ? 0 : ge_tfbp[1] == fbp ? 1 : -1;
    if (slot < 0) { slot = ge_tlru; ge_tlru ^= 1; ge_tfbp[slot] = fbp; }
    *ptex = NULL;
    if ((p >> 4) & 1) {
        *ptex = ge_texture(tex0);
        if (!*ptex && ge_tex_full) {                     /* arena / cache full: let the GE finish, start again */
            ge_tex_full = 0;
            ge_flush_batch();
            ge_cache_clear();
            *ptex = ge_texture(tex0);
        }
        if (!*ptex) return -1;
    }
    ge_bound_tex = (GeTex *)*ptex;
    ge_bound_clut = *ptex ? (GeClut *)ge_tclut : NULL;
    if (((test >> 16) & 1) && !((zbuf >> 32) & 1)) {      /* the depth buffer's pages: written by the GE */
        uint32_t q0, q1;
        ge_pages(((uint32_t)zbuf & 0x1FF) * 32, 8, PSMCT32, 448, &q0, &q1);
        for (uint32_t q = q0; q < q1; q++) if (!ge_prt[q]) { ge_prt[q] = 1; ge_epoch++; }
    }
    uint64_t key[10] = {p & 0x7F8, frame, zbuf, test, (p >> 4) & 1 ? tex0 : 0, (p >> 4) & 1 ? reg[0x14 + ctx] : 0,
                        (p >> 6) & 1 ? reg[0x42 + ctx] : 0, (p >> 4) & 1 ? reg[0x08 + ctx] : 0, reg[0x40 + ctx],
                        ((uint64_t)(*ptex ? (*ptex)->ver : 0)) ^ ((uint64_t)prim_kind << 56)
                        ^ ((uint64_t)(ge_tclut ? ge_tclut->ver : 0) << 28)};   /* versions: a slot redone in place is new */
    if (ge_bn && !memcmp(key, ge_bkey, sizeof key)) return slot;
    ge_flush_batch();
    if (!memcmp(key, ge_bkey, sizeof key)) return slot;
    memcpy(ge_bkey, key, sizeof key);
    sceGuDrawBuffer(GU_PSM_8888, (void *)GE_T_OFF(slot), GE_FB_STRIDE);   /* (kept by libgu: a list restart re-sends it) */
    uint32_t p0, p1;
    ge_pages(fbp * 32, fbw, fpsm, 448, &p0, &p1);
    for (uint32_t q = p0; q < p1; q++) if (!ge_prt[q]) { ge_prt[q] = 1; ge_epoch++; }
    uint64_t sc = reg[0x40 + ctx];
    int x0 = (int)((float)(sc & 0x7FF) * ge_sx), x1 = (int)((float)(((sc >> 16) & 0x7FF) + 1) * ge_sx + 0.5f);
    int y0 = (int)((float)((sc >> 32) & 0x7FF) * ge_sy), y1 = (int)((float)(((sc >> 48) & 0x7FF) + 1) * ge_sy + 0.5f);
    if (x1 > 480) x1 = 480;
    if (y1 > 272) y1 = 272;
    sceGuScissor(x0, y0, x1, y1);
    sceGuPixelMask((uint32_t)(frame >> 32) | (fpsm == PSMCT24 ? 0xFF000000u : 0));
    /* depth */
    static int nodepth = -1;
    if (nodepth < 0) nodepth = ge_opt("gs_nodepth", 0);
    if (((test >> 16) & 1) && !nodepth) {
        static const int zf[4] = {GU_NEVER, GU_ALWAYS, GU_GEQUAL, GU_GREATER};
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(zf[(test >> 17) & 3]);
        sceGuDepthMask((zbuf >> 32) & 1 ? GU_TRUE : GU_FALSE);
    } else sceGuDisable(GU_DEPTH_TEST);
    /* alpha test (AFAIL other than KEEP: test off) */
    if ((test & 1) && ((test >> 12) & 3) == 0) {
        static const int af[8] = {GU_NEVER, GU_ALWAYS, GU_LESS, GU_LEQUAL, GU_EQUAL, GU_GEQUAL, GU_GREATER, GU_NOTEQUAL};
        uint32_t aref = ((uint32_t)(test >> 4) & 0xFF) * 2;
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(af[(test >> 1) & 7], (int)(aref > 255 ? 255 : aref), 0xFF);
    } else sceGuDisable(GU_ALPHA_TEST);
    /* blending */
    if (((p >> 6) & 1) && ge_blend(reg[0x42 + ctx])) sceGuEnable(GU_BLEND);
    else sceGuDisable(GU_BLEND);
    /* texture */
    if (*ptex) {
        const GeTex *t = *ptex;
        sceGuEnable(GU_TEXTURE_2D);
        if (ge_tclut && (t->gpsm == GU_PSM_T4 || t->gpsm == GU_PSM_T8)) {
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(t->gpsm == GU_PSM_T4 ? 2 : 32, ge_tclut->clut);
        }
        sceGuTexMode(t->gpsm, 0, 0, 0);
        sceGuTexImage(0, t->w, t->h, t->bufw, t->gepix);
        uint32_t tfx = (uint32_t)(tex0 >> 35) & 3, tcc = (uint32_t)(tex0 >> 34) & 1;
        sceGuTexFunc(tfx == 1 ? GU_TFX_REPLACE : GU_TFX_MODULATE, tcc ? GU_TCC_RGBA : GU_TCC_RGB);
        uint64_t tex1 = reg[0x14 + ctx];
        int mag = (tex1 >> 5) & 1 ? GU_LINEAR : GU_NEAREST, mmin = ((tex1 >> 6) & 7) == 1 ? GU_LINEAR : GU_NEAREST;
        sceGuTexFilter(mmin, mag);
        uint64_t cl = reg[0x08 + ctx];
        sceGuTexWrap((cl & 3) == 0 ? GU_REPEAT : GU_CLAMP, ((cl >> 2) & 3) == 0 ? GU_REPEAT : GU_CLAMP);
        sceGuTexFlush();
    } else sceGuDisable(GU_TEXTURE_2D);
    return slot;
}

static uint32_t ge_col(uint32_t c)                    /* PS2 colour (0x80 = 1.0) -> GE (0xFF = 1.0) */
{
    uint32_t o = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t v = ((c >> (8 * k)) & 0xFF) * 2;
        o |= (v > 255 ? 255 : v) << (8 * k);
    }
    return o;
}

static void ge_vtx(GeV *o, const Vtx *v, int ctx, uint64_t p, const GeTex *t, uint32_t col, uint32_t zsh)
{
    uint64_t xyo = reg[0x18 + ctx];
    o->x = (float)((int32_t)v->x - (int32_t)(xyo & 0xFFFF)) * (ge_sx / 16.0f);
    o->y = (float)((int32_t)v->y - (int32_t)((xyo >> 32) & 0xFFFF)) * (ge_sy / 16.0f);
    uint32_t z = v->z >> zsh;
    o->z = (float)(z > 0xFFFF ? 0xFFFF : z);
    o->c = col;
    if (t) {
        if ((p >> 8) & 1) {
            float k = 1.0f / (float)(16u << t->ds);
            o->u = (float)v->u * k;
            o->v = (float)v->v * k;
        }
        else {
            float iq = v->q != 0.0f ? 1.0f / v->q : 1.0f;
            o->u = v->s * iq * (float)t->w;
            o->v = v->t * iq * (float)t->h;
        }
    } else o->u = o->v = 0;
}

static uint32_t ge_zshift(int ctx)
{
    uint32_t zpsm = ((uint32_t)(reg[0x4E + ctx] >> 24) & 15) | 0x30;
    return zpsm == PSMZ32 ? 16 : zpsm == PSMZ24 ? 8 : 0;
}

static uint32_t ge_prim_us, ge_texlook_us;        /* (bench) through-mode primitives; texture lookups in them */
static void ge_tri(const Vtx *a, const Vtx *b, const Vtx *c)
{
    if (bench_ff) return;                             /* benchmark fast-forward: nothing drawn */
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    const GeTex *t;
    ge_st_prims++;
    if (ge_bn && ge_bprim != GU_TRIANGLES) ge_flush_batch();
    {                                                 /* the texture rows this primitive samples */
        uint64_t t0h = reg[0x06 + ctx];
        float thf = (float)(1u << ((t0h >> 30) & 15)), mn = 1e30f, mx = -1e30f;
        const Vtx *hv[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            float r = ((p >> 8) & 1) ? (float)hv[k]->v / 16.0f : hv[k]->t / (hv[k]->q != 0.0f ? hv[k]->q : 1.0f) * thf;
            if (r < mn) mn = r;
            if (r > mx) mx = r;
        }
        ge_hint_lo = (int32_t)floorf(mn);
        ge_hint_hi = (int32_t)ceilf(mx) + 1;
    }
    if (ge_setup(p, ctx, &t, GU_TRIANGLES) < 0) return;
    ge_bprim = GU_TRIANGLES;
    if (ge_bn + 3 > GE_BATCH) ge_flush_batch();
    int iip = (int)((p >> 3) & 1);
    uint32_t zsh = ge_zshift(ctx), cf = ge_col(c->rgba);
    ge_vtx(&ge_bv[ge_bn++], a, ctx, p, t, iip ? ge_col(a->rgba) : cf, zsh);
    ge_vtx(&ge_bv[ge_bn++], b, ctx, p, t, iip ? ge_col(b->rgba) : cf, zsh);
    ge_vtx(&ge_bv[ge_bn++], c, ctx, p, t, cf, zsh);
    ge_dirty = 1;
}

static void ge_sprite(const Vtx *a, const Vtx *b)
{
    if (bench_ff) return;                             /* benchmark fast-forward: nothing drawn */
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    const GeTex *t;
    ge_st_prims++;
    /* an untextured blended sprite of alpha 0 with the blend (Cs - Cd) As + Cd changes nothing (the battle draws 32
       full-screen strips like that every frame; drawn, they darkened every other frame) */
    if (!((p >> 4) & 1) && ((p >> 6) & 1) && (reg[0x42 + ctx] & 0x33) == 0x00 && ((reg[0x42 + ctx] >> 2) & 3) == 1
        && ((reg[0x42 + ctx] >> 6) & 3) == 1 && !(a->rgba >> 24) && !(b->rgba >> 24))
        return;
    /* the battle's depth fog: a 16-bit pass (FRAME CT16, FBMSK 0x3FFF, not drawn here) copies depth bits into the
       frame's alpha, then untextured sprites subtract colour x destination alpha (ALPHA 0x52: Cd - Cs Ad). Without
       the first pass the GE's destination alpha is the 3D's own: the picture went dark. Not drawn (gs_dfog=1 draws). */
    {
        static int dfog = -1;
        if (dfog < 0) dfog = ge_opt("gs_dfog", 0);
        if (!dfog && !((p >> 4) & 1) && ((p >> 6) & 1) && (reg[0x42 + ctx] & 0xFF) == 0x52) return;
    }
    if (ge_bn && ge_bprim != GU_SPRITES) ge_flush_batch();
    {                                                 /* the texture rows this primitive samples */
        uint64_t t0h = reg[0x06 + ctx];
        float thf = (float)(1u << ((t0h >> 30) & 15)), mn = 1e30f, mx = -1e30f;
        const Vtx *hv[3] = {a, b, b};
        for (int k = 0; k < 3; k++) {
            float r = ((p >> 8) & 1) ? (float)hv[k]->v / 16.0f : hv[k]->t / (hv[k]->q != 0.0f ? hv[k]->q : 1.0f) * thf;
            if (r < mn) mn = r;
            if (r > mx) mx = r;
        }
        ge_hint_lo = (int32_t)floorf(mn);
        ge_hint_hi = (int32_t)ceilf(mx) + 1;
    }
    if (ge_setup(p, ctx, &t, GU_SPRITES) < 0) return;
    ge_bprim = GU_SPRITES;
    if (ge_bn + 2 > GE_BATCH) ge_flush_batch();
    uint32_t zsh = ge_zshift(ctx), cf = ge_col(b->rgba);
    ge_vtx(&ge_bv[ge_bn++], a, ctx, p, t, cf, zsh);
    Vtx bb = *b;
    if (!((p >> 8) & 1)) { bb.q = a->q; }             /* sprites: Q of the first vertex (STQ sprites are rare) */
    ge_vtx(&ge_bv[ge_bn++], &bb, ctx, p, t, cf, zsh);
    ge_dirty = 1;
}

static uint64_t ge_st_nat_batches, ge_st_nat_verts;

/* ---- on-screen monitors (ps2rc.cfg osd=0 turns them off): three lines at the top of the shown picture. The text is
   written (libpspdebug's font) into a 512 x 32 8888 buffer in RAM when the values change (once a second) and the GE
   draws it over the present copy, half-transparent (CPU writes into VRAM do not show in PPSSPP). */
static uint32_t ge_osd_buf[512 * 32] __attribute__((aligned(64)));
#define BT3_OSD_VERSION "v0.0.1"
static int ge_osd(void)                              /* 1 = draw the buffer */
{
    static int on = -1;
    static char l1[72], l2[72], l3[72];
    static SceInt64 t_last;
    static uint64_t fl0, pr0, nv0, nb0, dr0, tc0;
    static double vt0;
    static unsigned frame_us_max, last_flip_us;
    static SceInt64 t_prev_flip;
    if (on < 0) {
        const char *e = hle_cfg_get("osd");
        on = e ? atoi(e) : 1;
        if (on) {
            /* (InitEx clears a whole 512 x 272 screen at its base: target 0, redrawn anyway; then the base moves) */
            pspDebugScreenInitEx((void *)(0x44000000u + GE_T_OFF(0)), PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
            pspDebugScreenEnableBackColor(1);
            pspDebugScreenSetBackColor(0xFF000000u);
            pspDebugScreenSetTextColor(0xFFFFFFFFu);
        }
    }
    if (!on) return 0;
    SceInt64 now = sceKernelGetSystemTimeWide();
    if (t_prev_flip) {
        unsigned d = (unsigned)(now - t_prev_flip);
        if (d > frame_us_max) frame_us_max = d;
        last_flip_us = d;
    }
    t_prev_flip = now;
    if (!t_last) { t_last = now; vt0 = (double)hle_vtime / EE_HZ; }
    if (now - t_last >= 1000000) {
        double secs = (double)(now - t_last) / 1e6, vt = (double)hle_vtime / EE_HZ;
        double fps = (double)(ge_st_flips - fl0) / secs;
        snprintf(l1, sizeof l1, "%s  %5.1f FPS %6.2f ms (max %5.1f)  speed %.2fx", BT3_OSD_VERSION, fps,
                 fps > 0 ? 1000.0 / fps : 0.0, frame_us_max / 1000.0, (vt - vt0) / secs);
        snprintf(l2, sizeof l2, "%6llu tris %5llu draws %4llu batches %3llu tex  free %u K",
                 (unsigned long long)((ge_st_nat_verts - nv0) / 3 / (ge_st_flips - fl0 ? ge_st_flips - fl0 : 1)),
                 (unsigned long long)((ge_st_draws - dr0) / (ge_st_flips - fl0 ? ge_st_flips - fl0 : 1)),
                 (unsigned long long)((ge_st_nat_batches - nb0) / (ge_st_flips - fl0 ? ge_st_flips - fl0 : 1)),
                 (unsigned long long)(ge_st_tex_conv - tc0), (unsigned)(sceKernelTotalFreeMemSize() >> 10));
        snprintf(l3, sizeof l3, "t %.1f s  prims %llu/s  arena %u/%u K  flips %llu", vt,
                 (unsigned long long)((ge_st_prims - pr0) / secs), ge_arena_used >> 10, GE_ARENA_BYTES >> 10,
                 (unsigned long long)ge_st_flips);
        t_last = now; vt0 = vt; fl0 = ge_st_flips; pr0 = ge_st_prims; nv0 = ge_st_nat_verts; nb0 = ge_st_nat_batches;
        dr0 = ge_st_draws; tc0 = ge_st_tex_conv; frame_us_max = 0;
        if (on != 3) pspDebugScreenSetBase((u32 *)ge_osd_buf);
        pspDebugScreenSetXY(0, 0);
        if (on != 3) pspDebugScreenPrintf("%-60s\n%-60s\n%-60s", l1, l2, l3);
        if (on != 3) sceKernelDcacheWritebackRange(ge_osd_buf, sizeof ge_osd_buf);
        pspDebugScreenSetBase((u32 *)(0x44000000u + GE_T_OFF(0)));   /* the log console prints there, as before */
        pspDebugScreenSetXY(0, 0);
    }
    (void)last_flip_us;
    return on == 1 && l1[0] != 0;
}

/* the vblank: finish what was drawn, show the target the GS displays */
static void ge_vblank(void)
{
    if (ge_on <= 0) return;
    uint64_t pm = hle_gs_priv(0x0000);
    uint64_t fb = hle_gs_priv((pm & 2) ? 0x90 : 0x70);
    uint32_t fbp = (uint32_t)fb & 0x1FF;
    if (!ge_dirty && fbp == ge_shown) return;
    ge_flush_batch();
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    int slot = ge_tfbp[0] == fbp ? 0 : ge_tfbp[1] == fbp ? 1 : -1;
    if (slot >= 0) {
        /* present a COPY: the GE copies the finished target into one of two 16-bit display buffers (VRAM 0 /
           0x44000, the boot console's area; ps2rc.cfg screen=0 keeps the console from writing there) and that is
           shown. Showing the target itself let the next frame's drawing into it reach the screen (split picture)
           and the shown target alternate with the half-drawn other one (flicker). */
        static int disp;
        sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;
        sceGuDrawBufferList(GU_PSM_5650, (void *)(disp * 0x44000u), GE_FB_STRIDE);
        sceGuScissor(0, 0, 480, 272);
        sceGuPixelMask(0);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_DITHER);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 512, 512, GE_FB_STRIDE, (void *)(0x04000000u + GE_T_OFF(slot)));
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        GeV *cv = sceGuGetMemory(2 * sizeof(GeV));
        cv[0].u = 0; cv[0].v = 0; cv[0].c = 0xFFFFFFFFu; cv[0].x = 0; cv[0].y = 0; cv[0].z = 0;
        cv[1].u = 480; cv[1].v = 272; cv[1].c = 0xFFFFFFFFu; cv[1].x = 480; cv[1].y = 272; cv[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, cv);
        if (ge_osd()) {                               /* the monitors, over the picture */
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 512, 32, 512, ge_osd_buf);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
            sceGuTexFlush();
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            GeV *ov = sceGuGetMemory(2 * sizeof(GeV));
            ov[0].u = 0; ov[0].v = 0; ov[0].c = 0xFFFFFFFFu; ov[0].x = 0; ov[0].y = 0; ov[0].z = 0;
            ov[1].u = 480; ov[1].v = 24; ov[1].c = 0xFFFFFFFFu; ov[1].x = 480; ov[1].y = 24; ov[1].z = 0;
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL,
                           ov);
            sceGuDisable(GU_BLEND);
        }
        sceGuDisable(GU_DITHER);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        sceDisplaySetFrameBuf((void *)(0x04000000u + (uint32_t)disp * 0x44000u), GE_FB_STRIDE,
                              PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
        disp ^= 1;
        ge_shown = fbp;
    }
    ge_sync_us_sum += (unsigned)(sceKernelGetSystemTimeWide() - t0);   /* frame sync + the present copy */
    ge_st_flips++;
    ge_dirty = 0;                                     /* the texture cache lives on (pages' generations decide) */
    sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;
    memset(ge_bkey, 0xFF, sizeof ge_bkey);
    if (ge_st_flips % 300 == 1)
        LOG("gs_ge: flip %llu (shows fbp %x slot %d): prims %llu, draws %llu, textures converted %llu, skipped: "
            "target %llu, render-to-texture %llu, texture %llu, arena/cache full %llu; blend forms not mapped %llu; "
            "list restarts %llu; GE sync %u us per flip; per flip: textures %u us, GS DMA + VU1 %u us; cache hits %llu, palettes converted %llu, textures redone in place %llu, pages hashed %llu\n", (unsigned long long)ge_st_flips, fbp, slot,
            (unsigned long long)ge_st_prims, (unsigned long long)ge_st_draws, (unsigned long long)ge_st_tex_conv,
            (unsigned long long)ge_st_skip_target, (unsigned long long)ge_st_skip_rtt,
            (unsigned long long)ge_st_skip_tex, (unsigned long long)ge_st_arena_full,
            (unsigned long long)ge_st_blend_other, (unsigned long long)ge_st_flushes,
            ge_sync_us_sum / (unsigned)(ge_st_flips < 300 ? ge_st_flips : 300),
            ge_conv_us / (unsigned)(ge_st_flips < 300 ? ge_st_flips : 300),
            ge_dma_us / (unsigned)(ge_st_flips < 300 ? ge_st_flips : 300), (unsigned long long)ge_st_tex_hashhit,
            (unsigned long long)ge_st_clut_conv, (unsigned long long)ge_st_tex_inplace,
            (unsigned long long)ge_st_page_hash);
    if (ge_st_flips % 300 == 0) ge_sync_us_sum = ge_conv_us = ge_dma_us = 0;
}

/* the target shown now, copied to RAM by the GE (a block transfer: PPSSPP's hardware renderer downloads its
   framebuffer for it, a CPU read of VRAM would see stale memory there), for the gs_shot screenshots; the copy goes
   into the texture arena, which is emptied (the GE is idle afterwards); NULL = none */
static uint32_t *ge_shown_pixels(void)
{
    int slot = ge_tfbp[0] == ge_shown ? 0 : ge_tfbp[1] == ge_shown ? 1 : -1;
    if (slot < 0 || GE_ARENA_BYTES < GE_T_BYTES) return NULL;
    ge_flush_batch();
    sceKernelDcacheWritebackInvalidateRange(ge_arena, GE_T_BYTES);
    sceGuCopyImage(GU_PSM_8888, 0, 0, 480, 272, GE_FB_STRIDE, (void *)(0x04000000u + GE_T_OFF(slot)), 0, 0,
                   GE_FB_STRIDE, ge_arena);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    sceKernelDcacheInvalidateRange(ge_arena, GE_T_BYTES);
    sceGuStart(GU_DIRECT, ge_list);
    ge_list_seq++;
    ge_arena_used = 0;
    ge_ntex = 0;
    ge_nclut = 0;
    memset(ge_bkey, 0xFF, sizeof ge_bkey);
    return (uint32_t *)ge_arena;
}

/* burst captures: what the GE back end shows */
static void ge_burst_log(unsigned k)
{
    LOG("burst ge %u: shown fbp %x, slots fbp %x / %x, flips %llu\n", k, ge_shown, ge_tfbp[0], ge_tfbp[1],
        (unsigned long long)ge_st_flips);
}

#include "gs_ge_vu1.h"

/* benchmark (gs_soft.c bench_at): the flip count, and the cost split of the measured stretch */
static uint64_t ge_st_flips_get(void) { return ge_st_flips; }
static void ge_bench_report(void)
{
    extern uint64_t gs_stat_uploads(void);
    LOG("BENCH: through-mode primitives %u ms, texture lookups (all draws) %u ms\n", ge_prim_us / 1000,
        ge_texlook_us / 1000);
    LOG("BENCH: texture memo hits %llu\n", (unsigned long long)ge_st_memo_hits);
    LOG("BENCH: texture cache emptied %llu times\n", (unsigned long long)ge_st_cache_clears);
    LOG("BENCH: native VU1 programs %u ms in all\n", ge_nat_us / 1000);
    LOG("BENCH: uploads %llu KB in all (%llu KB already there, %llu KB deferred, %llu KB of those written later); upload "
        "hashing %u ms, writing (32/8/4-bit) %u ms in all\n", (unsigned long long)(gs_stat_uploads() >> 10),
        (unsigned long long)(ge_st_up_skipped >> 10), (unsigned long long)(ge_st_up_deferred >> 10),
        (unsigned long long)(ge_st_up_materialised >> 10), up_hash_us / 1000, up_write_us / 1000);
    LOG("BENCH: GE: prims %llu, draws %llu, textures converted %llu (cache hits %llu, palettes %llu), list restarts "
        "%llu, native VU1 batches %llu; last 300 flips per flip: textures %u us, GS DMA + VU1 %u us, GE wait %u us\n",
        (unsigned long long)ge_st_prims, (unsigned long long)ge_st_draws, (unsigned long long)ge_st_tex_conv,
        (unsigned long long)ge_st_tex_hashhit, (unsigned long long)ge_st_clut_conv,
        (unsigned long long)ge_st_flushes, (unsigned long long)ge_st_nat_batches,
        ge_conv_us / (unsigned)(ge_st_flips % 300 ? ge_st_flips % 300 : 300),
        ge_dma_us / (unsigned)(ge_st_flips % 300 ? ge_st_flips % 300 : 300),
        ge_sync_us_sum / (unsigned)(ge_st_flips % 300 ? ge_st_flips % 300 : 300));
}

/* gs_soft.c pix_write outside an upload (local copies, software draws): the page's id is recomputed on use */
void ge_page_touched(uint32_t page)
{
    page &= 511;
    if (!ge_pdirty[page]) { ge_pdirty[page] = 1; ge_epoch++; }
    ge_pwho[page] = 9;
}

/* gs_verify: where GS memory pages 0x170..0x190 change without a tracked write (logged once per page) */
static const char *gs_watch_last = "start";
void gs_watch(const char *where)
{
    if (ge_verify <= 0) return;
    static uint8_t told[512];
    for (uint32_t p = 0x170; p < 0x190; p++) {
        if (ge_pwho[p] != 1 || ge_pdirty[p] || ge_pend_src[p] || told[p] || !ge_pchash[p]) continue;
        uint32_t h = 0x811C9DC5u;
        const uint32_t *w = (const uint32_t *)(gm + (size_t)p * 8192u);
        for (int k = 0; k < 2048; k++) h = (h ^ w[k]) * 0x01000193u;
        if (h != ge_pchash[p]) {
            told[p] = 1;
            LOG("gs_ge: WATCH page %x changed between '%s' and '%s' (word0 %08x)\n", p, gs_watch_last, where, w[0]);
        }
    }
    gs_watch_last = where;
}

/* gs_soft.c: a palette went into the CLUT buffer (the memo of texture lookups is no longer valid) */
static void ge_clut_loaded(void) { ge_epoch++; }
