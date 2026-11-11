/* The game thread's half of bt3psp.tex: the index, and a marker in the display list after every texture upload the
   pack has (Dma_AddRef), so that the GPU back end (psp_gu.c) draws that texture from the pack, PC-converted and
   small, instead of decoding it from GS memory. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "psp_pack.h"

extern void Vita_Log(const char *fmt, ...);
extern void Port_GsMarkerArg(int effect, unsigned arg);

PackEnt *gPspPack;
int gPspPackN;
uint32_t gPspPackData;
int gPspPackFd = -1;

void Psp_PackInit(const char *path) {
    uint32_t h[8];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        Vita_Log("texture pack: none (%s)\n", path);
        return;
    }
    if (sceIoRead(fd, h, 32) != 32 || h[0] != 0x54335442u || h[1] != 2u || h[2] == 0 || h[2] > 65536u) {
        Vita_Log("texture pack: %s is not version 2 (make it again with crear_carpeta.html)\n", path);
        sceIoClose(fd);
        return;
    }
    gPspPack = malloc(h[2] * sizeof(PackEnt));
    sceIoLseek(fd, h[3], PSP_SEEK_SET);
    if (gPspPack == NULL || sceIoRead(fd, gPspPack, (SceSize)(h[2] * sizeof(PackEnt))) != (int)(h[2] * sizeof(PackEnt))) {
        Vita_Log("texture pack: index not read\n");
        free(gPspPack);
        gPspPack = NULL;
        sceIoClose(fd);
        return;
    }
    gPspPackN = (int)h[2];
    gPspPackData = h[4];
    gPspPackFd = fd;
    Vita_Log("texture pack: %d textures\n", gPspPackN);
}

static uint64_t fnv64(const uint8_t *p, uint32_t n) { /* (two 32-bit halves: the prime is 2^40 + 0x1B3) */
    uint32_t lo = 0x84222325u, hi = 0xCBF29CE4u, i;
    for (i = 0; i < n; i++) {
        uint64_t m;
        lo ^= p[i];
        m = (uint64_t)lo * 0x1B3u;
        hi = hi * 0x1B3u + (uint32_t)(m >> 32) + (lo << 8);
        lo = (uint32_t)m;
    }
    return (uint64_t)hi << 32 | lo;
}

static int find(uint64_t key) {
    int a = 0, b = gPspPackN - 1;
    while (a <= b) {
        int m = (a + b) >> 1;
        if (gPspPack[m].key == key) { return m; }
        if (gPspPack[m].key < key) { a = m + 1; } else { b = m - 1; }
    }
    return -1;
}

/* What is remembered of a texture packet by its place: 1024 sets of 4 (one direct-mapped table of 512 made textures
   that share a set hash their whole packet again every frame: 15-28% of the CPU in the character select). `chg`
   counts how often the content at one place changed: a place whose content keeps changing (a buffer the game
   redraws) is not in the pack, and is hashed only now and then. */
typedef struct { const void *a; uint32_t n, s0, s1, s2; int idx; uint16_t chg, wait; } Memo;
#define MEMO_SETS 1024
static Memo sMemo[MEMO_SETS][4];
static uint8_t sMemoNext[MEMO_SETS];
unsigned gPspPackHashes, gPspPackHashKB;

/* The pack's index of a texture packet (`size` bytes at `data`, kept at `ident`), or -1; hashed once per place and
   content (a few words compared: the game sends every texture every frame). data NULL: from what is remembered of
   `ident` alone (a lazy sheet's picture, not decoded yet), -2 when nothing is. */
int Psp_PackLookup(const void *data, int size, const void *ident) {
    const uint32_t *w = data;
    uint32_t s0 = 0, s1 = 0, s2 = 0, h = ((uint32_t)(uintptr_t)ident >> 4) * 0x9E3779B1u ^ (uint32_t)size * 0x85EBCA6Bu;
    Memo *set = sMemo[(h >> 16) & (MEMO_SETS - 1)], *m = NULL;
    int idx, k;
    if (gPspPackN == 0 || size < 0x70) {
        return -1;
    }
    for (k = 0; k < 4; k++) {
        if (set[k].a == ident && set[k].n == (uint32_t)size) { m = &set[k]; break; }
    }
    if (data == NULL) {
        return m != NULL ? m->idx : -2;
    }
    if ((w[3] >> 24) != 0x50 || w[4] != 0x8003u) {
        return -1; /* not a texture's packet: VIF DIRECT, then a GIF tag of three A+D writes */
    }
    s0 = w[(size >> 3) & ~3u]; s1 = w[(size >> 2) - 1]; s2 = w[24];
    if (m != NULL) {
        if (m->s0 == s0 && m->s1 == s1 && m->s2 == s2) {
            return m->idx;
        }
        if (m->chg >= 3) { /* a place that keeps changing: looked at again every 32nd time */
            if (++m->wait < 32) {
                return -1;
            }
            m->wait = 0;
        } else {
            m->chg++;
        }
    } else {
        k = sMemoNext[(h >> 16) & (MEMO_SETS - 1)]++ & 3;
        m = &set[k];
        m->chg = 0; m->wait = 0;
    }
    { extern volatile int gPspSect; int prev = gPspSect; gPspSect = 2; idx = find(fnv64(data, (uint32_t)size)); gPspSect = prev; }
    gPspPackHashes++; gPspPackHashKB += (uint32_t)size >> 10;
    if (idx >= 0) { m->chg = 0; }
    m->a = ident; m->n = (uint32_t)size; m->s0 = s0; m->s1 = s1; m->s2 = s2; m->idx = idx;
    return idx;
}

/* Instead of the upload: "the texture at the upload's place is pack entry idx" (GsGpu_Native, psp_gu.c). */
void Psp_PackEmit(int idx) {
    Port_GsMarkerArg(PACK_MARK, (unsigned)idx);
}

/* A lazy sheet freed: what was remembered by place goes (another sheet may land there). */
void Psp_PackForget(void) {
    memset(sMemo, 0, sizeof(sMemo));
}
