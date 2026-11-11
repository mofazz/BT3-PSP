/* Lazy sprite sheets (PSP-1000): a BPE-packed sheet whose unpacked size does not fit in the game heap is unpacked
   WITHOUT the pixels of its textures. The sheet's sections are laid out again without those pixels (the pack table
   and nothing else changes), and each texture is decoded from the packed data, which stays in its archive, straight
   into the display list when the game uploads it (Tex_UploadPixels, src/sys/gfxm_b_b.c). BPE blocks are independent,
   so a block index gives random access. */
#include <stdint.h>
#include <string.h>

extern void *Heap_Alloc(int size, unsigned align, int fromTail, int heap);
extern void Heap_Free(void *ptr);
extern void Vita_Log(const char *fmt, ...);
extern uint32_t gDmaBufEnd[2];
extern int gDmaBufIdx;
extern uint32_t *gDmaCur;

typedef struct { const uint8_t *in; uint32_t out; } Blk;
typedef struct {
    uint8_t *ent, *pix;
    uint8_t *cache; void *cblk; /* the decoded copy kept in the game heap (its HeapBlock), or NULL */
    uint32_t src, size, gen;    /* gen: the display list that last referred to the copy */
    int pidx;                   /* the picture's entry in bt3psp.tex, -1 none, -2 not looked up yet */
    uint16_t sec, idx;
} LazyTex;
typedef struct {
    uint8_t *buf, *end;
    const uint8_t *packed;
    Blk *blk;
    int nblk;
    LazyTex *tex;
    int ntex;
    uint8_t *pixLo, *pixHi; /* where the game's pointers to the left-out pictures point (Res_RelocateOffsets) */
} Sheet;

#define MAX_SHEETS 4
static Sheet sSheet[MAX_SHEETS];

/* Decodes one block whose output starts at `at`, copying the bytes in [lo, hi) to dst + (pos - lo). Returns the
   input after the block and sets *outEnd. */
static const uint8_t *blk_run(const uint8_t *in, uint32_t at, uint32_t lo, uint32_t hi, uint8_t *dst, uint32_t *outEnd) {
    uint8_t left[256], right[256], stack[256];
    int c, count, i, blockSize;

    for (i = 0; i < 256; i++) {
        left[i] = (uint8_t)i;
    }
    c = 0;
    count = *in++;
    for (;;) {
        if (count > 127) {
            c += count - 127;
            count = 0;
        }
        if (c == 256) {
            break;
        }
        for (i = 0; i <= count; i++, c++) {
            left[c] = *in++;
            if (c != left[c]) {
                right[c] = *in++;
            }
        }
        if (c == 256) {
            break;
        }
        count = *in++;
    }
    blockSize = in[1] + (in[0] << 8);
    in += 2;
    i = 0;
    for (;;) {
        if (i != 0) {
            c = stack[--i];
        } else {
            if (blockSize-- == 0) {
                break;
            }
            c = *in++;
        }
        if (c == left[c]) {
            if (at >= lo && at < hi) {
                dst[at - lo] = (uint8_t)c;
            }
            at++;
        } else {
            stack[i++] = right[c];
            stack[i++] = left[c];
        }
    }
    *outEnd = at;
    return in;
}

/* Bytes [lo, hi) of the unpacked sheet into dst. */
static void sheet_read(const Sheet *s, uint32_t lo, uint32_t hi, uint8_t *dst) {
    int a = 0, b = s->nblk - 1;
    uint32_t end;
    while (a < b) { /* the last block that starts at or before lo */
        int m = (a + b + 1) / 2;
        if (s->blk[m].out <= lo) { a = m; } else { b = m - 1; }
    }
    for (; a < s->nblk && s->blk[a].out < hi; a++) {
        blk_run(s->blk[a].in, s->blk[a].out, lo, hi, dst, &end);
    }
}

static uint32_t rd32(const Sheet *s, uint32_t off) {
    uint32_t v = 0;
    sheet_read(s, off, off + 4, (uint8_t *)&v);
    return v;
}

/* The pictures of a texture file (offsets in the section), sorted and merged, in sRng; returns how many. */
static uint32_t sRng[2 * 512];
static uint32_t pix_ranges(const Sheet *t, uint32_t base, uint32_t eo, uint32_t cnt) {
    uint32_t n = 0, e, j;
    for (e = 0; e < cnt; e++) {
        uint32_t w[3], a, b;
        sheet_read(t, base + eo + e * 0x40u, base + eo + e * 0x40u + 12u, (uint8_t *)w);
        if (!w[0]) { continue; }
        a = w[0] * 4u; b = a + w[2];
        for (j = n; j > 0 && sRng[2 * (j - 1)] > a; j--) { sRng[2 * j] = sRng[2 * j - 2]; sRng[2 * j + 1] = sRng[2 * j - 1]; }
        sRng[2 * j] = a; sRng[2 * j + 1] = b;
        n++;
    }
    for (e = j = 0; e < n; e++) { /* merge overlaps (two entries may share one picture) */
        if (j > 0 && sRng[2 * e] <= sRng[2 * j - 1]) {
            if (sRng[2 * e + 1] > sRng[2 * j - 1]) { sRng[2 * j - 1] = sRng[2 * e + 1]; }
        } else {
            sRng[2 * j] = sRng[2 * e]; sRng[2 * j + 1] = sRng[2 * e + 1]; j++;
        }
    }
    return j;
}

/* An offset of the section once the pictures are out. */
static uint32_t rng_map(uint32_t off, uint32_t n) {
    uint32_t j, gone = 0;
    for (j = 0; j < n && sRng[2 * j + 1] <= off; j++) { gone += sRng[2 * j + 1] - sRng[2 * j]; }
    return off - gone;
}

void *Psp_LazyBpe(const uint8_t *packed, uint32_t size, uint32_t packSize) {
    Sheet t, *s = NULL;
    const uint8_t *in = packed;
    uint32_t at = 0, nsec, k, *secOld = NULL, *secLen = NULL, *secKeep = NULL, newSize, o, dropped = 0;
    int nb = 0, cap = 1024, i;

    for (i = 0; i < MAX_SHEETS; i++) {
        if (sSheet[i].buf == NULL) { s = &sSheet[i]; break; }
    }
    if (s == NULL) {
        return NULL;
    }
    memset(&t, 0, sizeof(t));
    t.packed = packed;
    t.blk = Heap_Alloc(cap * (int)sizeof(Blk), 16, 1, 2);
    while (t.blk != NULL && (uint32_t)(in - packed) < packSize) { /* pass 1: the block index */
        if (nb == cap) {
            Blk *n = Heap_Alloc(cap * 2 * (int)sizeof(Blk), 16, 1, 2);
            if (n == NULL) { Heap_Free(t.blk); t.blk = NULL; break; }
            memcpy(n, t.blk, cap * sizeof(Blk));
            Heap_Free(t.blk);
            t.blk = n;
            cap *= 2;
        }
        t.blk[nb].in = in;
        t.blk[nb].out = at;
        nb++;
        in = blk_run(in, at, 0, 0, NULL, &at);
    }
    t.nblk = nb;
    if (t.blk == NULL || at != size) {
        Vita_Log("lazy sheet: index failed (%u of %u bytes)\n", at, size);
        if (t.blk) { Heap_Free(t.blk); }
        return NULL;
    }
    /* the pack table: word 0 = the number of sections, word n (1..count) = section n's byte offset (MPACK_AT) */
    nsec = rd32(&t, 0);
    if (nsec == 0 || nsec > 4096) {
        Heap_Free(t.blk);
        return NULL;
    }
    secOld = Heap_Alloc((int)(nsec * 12u), 16, 1, 2);
    if (secOld == NULL) { Heap_Free(t.blk); return NULL; }
    secLen = secOld + nsec;
    secKeep = secLen + nsec;
    for (k = 0; k < nsec; k++) {
        secOld[k] = rd32(&t, 4u + k * 4u);
    }
    for (k = 0; k < nsec; k++) {
        uint32_t next = size;
        for (o = 0; o < nsec; o++) { /* (not assumed in order) */
            if (secOld[o] > secOld[k] && secOld[o] < next) { next = secOld[o]; }
        }
        secLen[k] = secOld[k] <= size ? next - secOld[k] : 0;
        secKeep[k] = secLen[k];
    }
    /* pass 2: which sections are texture files, and how much of each holds anything but pixels */
    t.tex = Heap_Alloc(512 * (int)sizeof(LazyTex), 16, 1, 2);
    for (k = 0; k < nsec && t.tex != NULL; k++) {
        uint32_t base = secOld[k], len = secLen[k], cnt, eo, keep, e;
        int ok = 1;
        if (len < 0x20) { continue; }
        cnt = rd32(&t, base);
        eo = rd32(&t, base + 4) * 4u;
        if (cnt == 0 || cnt > 512 || eo != 0x20 || eo + cnt * 0x40u > len) { continue; } /* (every texture file has its entries at 0x20; a nested pack does not) */
        for (e = 0; e < cnt && ok; e++) { /* a texture file for sure: every picture a whole GS packet after the entries, a TEX0 */
            uint32_t w[16];
            sheet_read(&t, base + eo + e * 0x40u, base + eo + e * 0x40u + 0x40u, (uint8_t *)w);
            if (w[0] && (w[2] < 0x70 || (w[2] & 15) || (w[12] | w[13]) == 0 || w[0] * 4u < eo + cnt * 0x40u)) { ok = 0; }
            if (w[1] && w[1] * 4u < eo + cnt * 0x40u) { ok = 0; }
        }
        if (!ok) { continue; }
        keep = eo + cnt * 0x40u;
        for (e = 0; e < cnt && ok; e++) {
            uint32_t w[4];
            sheet_read(&t, base + eo + e * 0x40u, base + eo + e * 0x40u + 16u, (uint8_t *)w);
            if ((w[0] && (w[0] * 4u + (uint32_t)w[2] > len || (int32_t)w[2] < 0)) || (w[1] && (w[1] * 4u + (uint32_t)w[3] > len || (int32_t)w[3] < 0))) {
                ok = 0;
            }
            if (w[1] && w[1] * 4u + w[3] > keep) { keep = w[1] * 4u + w[3]; }
        }
        if (!ok) { continue; }
        {   /* every picture leaves; the rest (header, entries, palettes) closes up behind them */
            uint32_t n = pix_ranges(&t, base, eo, cnt), j, gone = 0;
            for (j = 0; j < n; j++) { gone += sRng[2 * j + 1] - sRng[2 * j]; }
            if (gone == 0) { continue; }
            keep = len - gone;
        }
        for (e = 0; e < cnt && t.ntex < 512; e++) {
            uint32_t w[3];
            sheet_read(&t, base + eo + e * 0x40u, base + eo + e * 0x40u + 12u, (uint8_t *)w);
            if (w[0]) {
                t.tex[t.ntex].ent = NULL; /* set below, once the section has its new place */
                t.tex[t.ntex].cache = NULL; t.tex[t.ntex].cblk = NULL; t.tex[t.ntex].gen = 0; t.tex[t.ntex].pidx = -2;
                t.tex[t.ntex].sec = (uint16_t)k;
                t.tex[t.ntex].idx = (uint16_t)e;
                t.tex[t.ntex].src = base + w[0] * 4u;
                t.tex[t.ntex].size = (uint32_t)w[2];
                t.ntex++;
            }
        }
        dropped += len - keep;
        secKeep[k] = keep;
    }
    if (t.tex == NULL || dropped == 0) {
        Vita_Log("lazy sheet: no texture pixels to leave out\n");
        goto fail;
    }
    /* the new layout: the table, then each section (64-byte aligned) with only its kept part */
    newSize = (nsec * 4u + 4u + 63u) & ~63u;
    for (k = 0; k < nsec; k++) {
        newSize += (secKeep[k] + 63u) & ~63u;
    }
    t.buf = Heap_Alloc((int)newSize, 0x20, 0, 2);
    if (t.buf == NULL) {
        Vita_Log("lazy sheet: %u KB still does not fit\n", newSize >> 10);
        goto fail;
    }
    t.end = t.buf + newSize;
    o = (nsec * 4u + 4u + 63u) & ~63u;
    memset(t.buf, 0, o);
    ((uint32_t *)t.buf)[0] = nsec;
    for (k = 0; k < nsec; k++) {
        uint32_t newOff = o;
        ((uint32_t *)t.buf)[k + 1] = newOff;
        if (secKeep[k] == secLen[k]) {
            sheet_read(&t, secOld[k], secOld[k] + secLen[k], t.buf + newOff);
        } else {
            uint32_t base = secOld[k], cnt = rd32(&t, base), eo = rd32(&t, base + 4) * 4u, n = pix_ranges(&t, base, eo, cnt), j, cur = 0, d = newOff, e;
            uint32_t *ent;
            for (j = 0; j <= n; j++) { /* the pieces between the pictures */
                uint32_t stop = j < n ? sRng[2 * j] : secLen[k];
                sheet_read(&t, base + cur, base + stop, t.buf + d);
                d += stop - cur;
                if (j < n) { cur = sRng[2 * j + 1]; }
            }
            eo = rng_map(eo, n);
            ((uint32_t *)(t.buf + newOff))[1] = eo / 4u;
            ent = (uint32_t *)(t.buf + newOff + eo);
            for (e = 0; e < cnt; e++) { /* palettes: where they are now (pictures: Psp_LazyUpload) */
                if (ent[e * 16u + 1u]) { ent[e * 16u + 1u] = rng_map(ent[e * 16u + 1u] * 4u, n) / 4u; }
            }
            for (i = 0; i < t.ntex; i++) {
                if (t.tex[i].sec == k) {
                    t.tex[i].ent = (uint8_t *)(ent + t.tex[i].idx * 16u);
                    t.tex[i].pix = t.buf + newOff + ent[t.tex[i].idx * 16u] * 4u; /* e->pix once relocated: what Dma_AddRef will be given */
                    if (t.pixLo == NULL || t.tex[i].pix < t.pixLo) { t.pixLo = t.tex[i].pix; }
                    if (t.tex[i].pix + 1 > t.pixHi) { t.pixHi = t.tex[i].pix + 1; }
                }
            }
        }
        o += (secKeep[k] + 63u) & ~63u;
    }
    Heap_Free(secOld);
    *s = t;
    Vita_Log("lazy sheet: %u KB unpacked as %u KB (%d textures decoded on upload, %d blocks)\n", size >> 10, newSize >> 10, t.ntex, t.nblk);
    return t.buf;
fail:
    if (t.tex) { Heap_Free(t.tex); }
    Heap_Free(secOld);
    Heap_Free(t.blk);
    return NULL;
}

/* ---- decoded pictures kept. The game sends every texture every frame, so a left-out picture that bt3psp.tex does
   not have was decoded from its packed sheet again every frame. Now the decoded copy stays in free memory of the
   game heap (tail end) and the display list refers to it, as the PS2's own list refers to the sheet. The copies
   leave a reserve for the game, and Heap_Alloc takes back the ones no pending list uses when the game runs out
   (Psp_LazyTrim). What the pack says about a picture is remembered with the picture (no hash after the first). */
extern void *Heap_AllocBlock(int size, unsigned align, int fromTail, int heap);
extern void Heap_FreeBlock(void *block);
extern unsigned Heap_GetLargestFree(void);
extern volatile int gPspSect;
#define CACHE_CAP_DEFAULT (8u << 20)
#define CACHE_RESERVE (2560u << 10) /* the largest free block the game keeps after a copy is made */
static uint32_t sFlushGen = 4, sCacheCap = CACHE_CAP_DEFAULT;
unsigned gPspLazyCacheBytes, gPspLazyDecodes, gPspLazyHits, gPspLazyTrims;

static void cache_drop(LazyTex *x) {
    if (x->cblk != NULL) {
        Heap_FreeBlock(x->cblk);
        gPspLazyCacheBytes -= x->size & ~15u;
        x->cblk = NULL;
        x->cache = NULL;
    }
}

static uint8_t *cache_new(LazyTex *x, uint32_t n) {
    void *b;
    if (gPspLazyCacheBytes + n > sCacheCap || Heap_GetLargestFree() < n + CACHE_RESERVE) {
        return NULL;
    }
    b = Heap_AllocBlock((int)n, 0x40, 1, 2);
    if (b == NULL) {
        return NULL;
    }
    x->cblk = b;
    x->cache = (uint8_t *)(uintptr_t)((uint32_t *)b)[5]; /* HeapBlock.data */
    gPspLazyCacheBytes += n;
    return x->cache;
}

/* Heap_Alloc (src/sys/heap.c) could not serve the game: the copies that no list of this frame or the last refers to
   are given back. Returns how many. */
int Psp_LazyTrim(void) {
    int i, j, freed = 0;
    uint32_t before = gPspLazyCacheBytes;
    for (i = 0; i < MAX_SHEETS; i++) {
        for (j = 0; sSheet[i].buf != NULL && j < sSheet[i].ntex; j++) {
            LazyTex *x = &sSheet[i].tex[j];
            if (x->cblk != NULL && x->gen + 2u <= sFlushGen) {
                cache_drop(x);
                freed++;
            }
        }
    }
    if (freed) {
        sCacheCap = before / 2u;
        if (gPspLazyTrims++ < 10u) {
            Vita_Log("lazy pictures: %d copies (%u KB) given back to the game; at most %u KB kept from now on\n", freed,
                     (before - gPspLazyCacheBytes) >> 10, sCacheCap >> 10);
        }
    }
    return freed;
}

/* Dma_Flush: the list was handed over. */
void Psp_LazyFlushed(void) {
    sFlushGen++;
}

/* Dma_AddRef (src/sys/dma.c): 1 = `addr` is a left-out picture and everything is done (a marker for the pack's
   picture, or the picture decoded into the display list). 0 with *sub set = send the decoded copy at *sub instead.
   Every path that sends a texture's pixels comes through here. */
int Psp_LazyRef(void *addr, int bytes, void **sub) {
    extern int Psp_PackLookup(const void *data, int size, const void *ident);
    extern void Psp_PackEmit(int idx);
    int i, j;
    for (i = 0; i < MAX_SHEETS; i++) {
        Sheet *s = &sSheet[i];
        if (s->buf == NULL || (uint8_t *)addr < s->pixLo || (uint8_t *)addr >= s->pixHi) {
            continue;
        }
        for (j = 0; j < s->ntex; j++) {
            LazyTex *x = &s->tex[j];
            uint32_t qwc, n, *p;
            uint8_t *c;
            int prev;
            if (x->pix != (uint8_t *)addr || x->size != (uint32_t)bytes) {
                continue;
            }
            if (x->pidx >= 0) { /* in bt3psp.tex: not decoded at all */
                Psp_PackEmit(x->pidx);
                return 1;
            }
            if (x->cache != NULL) { /* decoded before */
                x->gen = sFlushGen;
                *sub = x->cache;
                gPspLazyHits++;
                return 0;
            }
            qwc = x->size / 16u;
            n = qwc * 16u;
            prev = gPspSect;
            c = cache_new(x, n);
            if (c != NULL) {
                gPspSect = 1;
                sheet_read(s, x->src, x->src + n, c);
                gPspLazyDecodes++;
                if (x->pidx == -2) {
                    gPspSect = 2;
                    x->pidx = Psp_PackLookup(c, bytes, addr);
                    if (x->pidx < -1) { x->pidx = -1; }
                }
                gPspSect = prev;
                if (x->pidx >= 0) { /* the pack has it after all: the copy is not needed */
                    cache_drop(x);
                    Psp_PackEmit(x->pidx);
                    return 1;
                }
                x->gen = sFlushGen;
                *sub = c;
                return 0;
            }
            /* no memory to keep it: decoded into the display list, this frame and the next */
            p = gDmaCur;
            if ((uint8_t *)p + 16u + n + 0x10000u > (uint8_t *)(uintptr_t)gDmaBufEnd[gDmaBufIdx]) {
                Vita_Log("lazy sheet: no room in the display list for %u KB\n", x->size >> 10);
                return 1; /* left out rather than overrun */
            }
            gPspSect = 1;
            p[0] = 0x10000000u | qwc; /* CNT */
            p[1] = 0; p[2] = 0; p[3] = 0;
            sheet_read(s, x->src, x->src + n, (uint8_t *)(p + 4));
            gDmaCur = p + 4 + qwc * 4u;
            gPspLazyDecodes++;
            if (x->pidx == -2) {
                gPspSect = 2;
                x->pidx = Psp_PackLookup(p + 4, bytes, addr);
                if (x->pidx < -1) { x->pidx = -1; }
            }
            gPspSect = prev;
            if (x->pidx >= 0) {
                gDmaCur = p; /* (the copy is not needed after all) */
                Psp_PackEmit(x->pidx);
            }
            return 1;
        }
    }
    return 0;
}

/* Heap_Free of a lazy sheet: its index and table go with it. */
void Psp_LazyFreed(void *ptr) {
    int i;
    for (i = 0; i < MAX_SHEETS; i++) {
        if (sSheet[i].buf != NULL && sSheet[i].buf == (uint8_t *)ptr) {
            { int j; for (j = 0; j < sSheet[i].ntex; j++) { cache_drop(&sSheet[i].tex[j]); } }
            sCacheCap = CACHE_CAP_DEFAULT;
            Heap_Free(sSheet[i].tex);
            Heap_Free(sSheet[i].blk);
            memset(&sSheet[i], 0, sizeof(Sheet));
            { extern void Psp_PackForget(void); Psp_PackForget(); }
        }
    }
}
