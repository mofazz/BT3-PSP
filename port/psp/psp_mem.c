/* PSP memory: generated from port/vita/vita_mem.c (the heap comes from the user partition). */
/*
 * Memory of the PS Vita build (port/src/plat_mem.c is the PC's).
 *
 * The PC build gives the game memory at the PS2's own addresses. A Vita program cannot choose where its memory
 * is, and it does not have to: the game takes its heap with one malloc and keeps ordinary 32-bit pointers, which
 * is what the Vita has. So here
 *   - the game heap is one block from the system (28.3 MB, the size of the PS2's region), wherever it lands;
 *   - the hardware registers are two arrays (the game's register macros go through PORT_HW, port_compat.h);
 *   - the data tables of the game's two programs are filled in from the user's own files at start
 *     (Vita_LoadGameData), through the table of addresses build.py generates (vita_dat.c).
 * Every address in a Vita program has its top bit set (0x81000000 and up). The PS2's never do, and nothing in the
 * PC builds has ever run that way (they keep everything below 2 GB): game code that treats a pointer as a signed
 * number or keeps a flag in its top bit would show here first. heap=cdram (bt3.txt) asks for the heap in the
 * graphics memory area instead, which lies lower; an experiment for that case, not a fix.
 */
#include "psp_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vita_dat.h"

#ifndef HOME
#define HOME "ms0:/PSP/GAME/BT3PSP"
#endif
#define HEAP_SKIP 0x730u /* the PS2's first address is 0x3BE730: keep its position within a page */

/* The game's own heap (src/sys/heap.c, Heap_Create) on the PS2: 0x1EFB000 - 0x3BE71C = 27.2 MB. */
#define GAME_HEAP_FULL ((uint32_t)(0x1EFB000 - 0x3BE71C))
/* The smallest heap worth starting with; below this the menus cannot load. A guess until the log's
   "heap peak" lines say how much the game really uses. */
#define GAME_HEAP_MIN (8u << 20)

unsigned char gPortHw[0x10000] __attribute__((aligned(16)));   /* stands for 0x10000000..0x1000FFFF */
unsigned char gPortGsReg[0x2000] __attribute__((aligned(16))); /* stands for 0x12000000..0x12001FFF */
volatile uint32_t gPortDataStripped = 1;                       /* the tables come from the disc, always */
uint32_t gVitaHeapBase, gVitaHeapUsed;

/* Memory profile (the NFSU2 idea): measured at start, "extended" when the PS2's whole heap fits (2000/3000, or a
   1000 with ARK's extra memory), "compact" otherwise; the game heap then takes what is left. */
int gPspMemCompact;
uint32_t gPspGameStack = 4u << 20;
static uint32_t sHeapSize, sGameHeap;
static uint32_t sHeapNext, sHeapEnd;
static uint8_t *sVolBase;
static uint32_t sVolSize, sVolUsed;
extern void Vita_Fatal(const char *fmt, ...);
extern void Vita_Log(const char *fmt, ...);
extern uint32_t gPortHeapUsedNow, gPortHeapUsedPeak, gPortHeapFails;
int sceKernelVolatileMemTryLock(int unk, void **ptr, int *size);
extern uint32_t *gPspGsVram;
static uint32_t sVolErr, sVolErr2;

/* The 4 MB at 0x08400000 that the system lends a game while no suspend image is kept: the emulated GS memory
   goes there (gs_core.c), which frees 4 MB of the main memory. NULL when the lock is refused or the space is taken. */
void *Psp_VolatileAlloc(uint32_t n) {
    uint8_t *p;
    n = (n + 63u) & ~63u;
    if (sVolBase == NULL || sVolUsed + n > sVolSize) {
        return NULL;
    }
    p = sVolBase + sVolUsed;
    sVolUsed += n;
    return p;
}

static uint32_t read_number(const char *path, uint32_t def) {
    char t[16] = {0};
    SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) {
        return def;
    }
    sceIoRead(f, t, sizeof(t) - 1);
    sceIoClose(f);
    return atoi(t) > 0 ? (uint32_t)atoi(t) : def;
}

/* What the main memory still has to hold after the heap: the game's stack, the GE's texture cache, and room for the C library's later allocations, the threads and the file buffers. */
#define AFTER_HEAP_SLACK (512u << 10) /* the log showed ~1.8 MB never used with 1.5 MB here */
#define TEX_CACHE (512u << 10)

void Vita_MapMemory(int cdram) {
    uint32_t largest, after, want;
    SceUID id;
    void *base = NULL;
    int vs = 0;

    (void)cdram;
    {
        /* two ways in: the power service's and the suspend service's (one of them may be refused on some firmwares) */
        int r1 = 0, r2 = 0; /* scePowerVolatileMemLock is not in this PSPSDK: the suspend service only */
        sVolBase = NULL;
        {
            sVolBase = NULL; vs = 0;
            r2 = sceKernelVolatileMemTryLock(0, (void **)&sVolBase, &vs);
        }
        if (sVolBase != NULL && vs > 0) {
            sVolSize = (uint32_t)vs;
        } else {
            sVolBase = NULL;
        }
        sVolErr = (uint32_t)(r1 < 0 ? r1 : 0); sVolErr2 = (uint32_t)(r2 < 0 ? r2 : 0);
        Vita_Log("volatile memory: power lock 0x%08X, suspend lock 0x%08X, %u KB at %p\n", (unsigned)r1, (unsigned)r2, sVolSize >> 10, (void *)sVolBase);
    }
    /* the GS memory the renderer emulates (4 MB): into the volatile memory when there is one, else the main memory */
    gPspGsVram = (uint32_t *)Psp_VolatileAlloc(4u << 20);
    if (gPspGsVram == NULL) {
        SceUID g = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "bt3_gsmem", PSP_SMEM_High, (4u << 20) + 64u, NULL);
        if (g < 0) {
            Vita_Fatal("no memory for the GS memory (4 MB): error 0x%08X", (unsigned)g);
        }
        gPspGsVram = (uint32_t *)(((uintptr_t)sceKernelGetBlockHeadAddr(g) + 63u) & ~(uintptr_t)63u);
    }
    memset(gPspGsVram, 0, 4u << 20);
    largest = (uint32_t)sceKernelMaxFreeMemSize();
    /* stack.txt: the game thread's stack in KB (default 4096, compact 1024) */
    after = (4u << 20) + AFTER_HEAP_SLACK + TEX_CACHE;
    want = HEAP_SKIP + GAME_HEAP_FULL + 0x10000u;
    gPspMemCompact = largest < want + after;
    if (gPspMemCompact) {
        gPspGameStack = 256u << 10; /* the game used ~10 KB of its stack in the menus */
    }
    gPspGameStack = read_number(HOME "/stack.txt", gPspGameStack >> 10) << 10;
    if (gPspGameStack < (128u << 10)) {
        gPspGameStack = 128u << 10;
    }
    after = gPspGameStack + AFTER_HEAP_SLACK + TEX_CACHE;
    if (gPspMemCompact) {
        want = largest > after ? largest - after : 0u;
    }
    sHeapSize = want & ~0xFFFFu;
    sGameHeap = sHeapSize > HEAP_SKIP + 0x10000u ? sHeapSize - HEAP_SKIP - 0x10000u : 0u;
    if (sGameHeap > GAME_HEAP_FULL) {
        sGameHeap = GAME_HEAP_FULL;
    }
    Vita_Log("memory profile: %s; largest free block %u KB, volatile memory %u KB%s, game heap %u of %u KB, stack %u KB\n",
             gPspMemCompact ? "COMPACT (less memory than the PS2's heap needs)" : "extended", largest >> 10, sVolSize >> 10,
             sVolBase ? "" : " (refused)", sGameHeap >> 10, GAME_HEAP_FULL >> 10, gPspGameStack >> 10);
    if (sGameHeap < GAME_HEAP_MIN) {
        Vita_Fatal("not enough memory for the game heap: %u KB free, at least %u KB needed\nvolatile memory %u KB (errors 0x%08X 0x%08X)\n(a PSP-1000 gives programs 24 MB; ARK's extra memory makes it 52 MB)",
                   largest >> 10, (GAME_HEAP_MIN + after) >> 10, sVolSize >> 10, (unsigned)sVolErr, (unsigned)sVolErr2);
    }
    id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "bt3_heap", PSP_SMEM_Low, sHeapSize, NULL);
    if (id >= 0) {
        base = sceKernelGetBlockHeadAddr(id);
    }
    if (id < 0 || base == NULL) {
        Vita_Fatal("no memory for the game heap (%u KB): error 0x%08X", sHeapSize >> 10, (unsigned)id);
    }
    memset(base, 0, sHeapSize);
    gVitaHeapBase = (uint32_t)(uintptr_t)base;
    sHeapNext = gVitaHeapBase + HEAP_SKIP;
    sHeapEnd = gVitaHeapBase + sHeapSize;
}

void Port_HeapBig(int32_t size, void *data, void *caller) {
    extern void Vita_LogFlush(void);
    Vita_Log("game heap: %d KB at %p for %p; in use %u KB\n", (int)(size >> 10), data, caller, gPortHeapUsedNow >> 10);
    Vita_LogFlush(); /* (at once: a freeze right after it left nothing in the log) */
}

/* Before a screen that needs room (the character select, a battle's load): what the port itself keeps is let go. The
   game's own data of the screen before is the game's to free (it does, when the next screen starts). */
void Psp_FreeMemory(const char *why) {
    extern void Vita_LogFlush(void);
    extern unsigned Heap_GetLargestFree(void), Heap_GetFreeSize(void);
    extern unsigned Psp_GuFreeAll(void);
    extern int Psp_LazyTrim(void);
    extern unsigned gPspLazyCacheBytes;
    unsigned used0 = gPortHeapUsedNow, free0 = Heap_GetFreeSize(), big0 = Heap_GetLargestFree(), kept0 = gPspLazyCacheBytes, tex;
    int copies;
    tex = Psp_GuFreeAll();
    copies = Psp_LazyTrim();
    Vita_Log("memory let go before %s: %u KB of GE textures forgotten, %d decoded pictures (%u KB) given back; game heap in use %u KB, free %u -> %u KB, "
             "largest free block %u -> %u KB; system free %u KB\n", why, tex >> 10, copies, (kept0 - gPspLazyCacheBytes) >> 10, used0 >> 10, free0 >> 10,
             Heap_GetFreeSize() >> 10, big0 >> 10, Heap_GetLargestFree() >> 10, (unsigned)sceKernelTotalFreeMemSize() >> 10);
    Vita_LogFlush();
}

/* Heap_Free of a block of 1 MB or more: the game leaves a screen. */
void Port_HeapFreed(int32_t size) {
    extern void Vita_LogFlush(void);
    Vita_Log("game heap: %d KB freed; in use %u KB\n", (int)(size >> 10), gPortHeapUsedNow >> 10);
    Vita_LogFlush();
}

/* Heap_Create's size (src/sys/heap.c, PORT_PSP). */
int32_t Port_GameHeapSize(void) {
    return (int32_t)sGameHeap;
}

/* A request the game heap could not meet: the PS2's game never sees that, so on a compact heap this is the first
   thing to look for in the log after a crash. */
void Port_HeapAllocFailed(int32_t size, int32_t heap) {
    if (gPortHeapFails <= 20u) {
        extern void Vita_LogFlush(void);
        extern unsigned Heap_GetLargestFree(void), Heap_GetFreeSize(void);
        Vita_Log("game heap FULL: %d bytes (heap %d) refused for %p; in use %u KB, peak %u KB of %u KB; free %u KB, largest free block %u KB\n", (int)size, (int)heap,
                 __builtin_return_address(0), gPortHeapUsedNow >> 10, gPortHeapUsedPeak >> 10, sGameHeap >> 10, Heap_GetFreeSize() >> 10, Heap_GetLargestFree() >> 10);
        Vita_LogFlush(); /* (what the game does with the NULL may freeze the console before the heartbeat writes) */
    }
}

/* The game's malloc (port_compat.h renames it): a bump allocator over the heap block, 16-byte aligned. */
void *Port_Malloc(uint32_t size) {
    uint32_t p = (sHeapNext + 15) & ~15u;

    if (size > sHeapEnd - p) {
        Vita_Fatal("malloc(0x%X) does not fit in the game heap", size);
    }
    sHeapNext = p + size;
    gVitaHeapUsed = sHeapNext - gVitaHeapBase;
    return (void *)(uintptr_t)p;
}

/* Memory the port hands to the game by address (file handles, the "second processor's" memory): zeroed. */
void *Port_LowAlloc(size_t size) {
    void *p = calloc(1, size);

    if (p == NULL) {
        Vita_Fatal("out of memory (%u bytes)", (unsigned)size);
    }
    return p;
}

void Port_LowFree(void *addr) {
    free(addr);
}

/* The start-up reads about 4.5 MB of files that are thrown away at once. The game's heap is empty until the game runs,
   so they go there (Vita_MapMemory has run first): the C library's heap can stay small. */
static size_t sScrUsed;

static void *scr_alloc(size_t n) {
    uint8_t *p;
    if (gVitaHeapBase == 0) {
        return calloc(1, n);
    }
    p = (uint8_t *)(uintptr_t)gVitaHeapBase + sScrUsed;
    sScrUsed += (n + 63u) & ~(size_t)63u;
    if (sScrUsed > sHeapSize / 2) {
        Vita_Fatal("start-up buffers do not fit (%u bytes)", (unsigned)sScrUsed);
    }
    return p;
}

static void scr_free(void *p) {
    if (gVitaHeapBase == 0 || (uintptr_t)p < gVitaHeapBase || (uintptr_t)p >= gVitaHeapBase + sHeapSize) {
        free(p);
    }
}

static uint8_t *read_file(const char *path, size_t *size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    uint8_t *buf;
    SceOff n;

    if (fd < 0) {
        return NULL;
    }
    n = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    buf = n > 0 && n < 0x4000000 ? scr_alloc((size_t)n) : NULL;
    if (buf == NULL || sceIoRead(fd, buf, (SceSize)n) != (int)n) {
        sceIoClose(fd);
        scr_free(buf);
        return NULL;
    }
    sceIoClose(fd);
    *size = (size_t)n;
    return buf;
}

/* Copies the values of the game's data tables in from <disc>/SLUS_216.78 (its loaded sections as one image from
   0x100000) and <disc>/BIN/DBZP.BIN, and checks them against the checksum of port/data: the same step as
   Port_LoadGameData of the PC build, with the destinations as addresses the Vita's loader has already placed. */
#define ROM_BASE 0x100000u
#define ROM_END 0x2FF180u

void Vita_LoadGameData(const char *disc) {
    char path[300];
    uint8_t *elf, *src[2] = {NULL, NULL};
    size_t elfSize = 0, srcSize[2] = {0, 0};
    uint64_t hash = 0xCBF29CE484222325ull;
    uint32_t i, k;

    snprintf(path, sizeof(path), "%s/SLUS_216.78", disc);
    elf = read_file(path, &elfSize);
    if (elf == NULL || elfSize < 0x34 || memcmp(elf, "\177ELF", 4) != 0) {
        Vita_Fatal("the game's program was not found:\n  %s\nCopy the files of your own disc there (see the README).", path);
    }
    {
        uint32_t shoff, sh[6];
        uint16_t shentsize, shnum;
        memcpy(&shoff, elf + 0x20, 4);
        memcpy(&shentsize, elf + 0x2E, 2);
        memcpy(&shnum, elf + 0x30, 2);
        srcSize[0] = ROM_END - ROM_BASE;
        src[0] = scr_alloc(srcSize[0]);
        if (src[0] == NULL) {
            Vita_Fatal("out of memory");
        }
        for (i = 0; i < shnum && (size_t)shoff + (size_t)(i + 1) * shentsize <= elfSize; i++) {
            memcpy(sh, elf + shoff + (size_t)i * shentsize, sizeof(sh)); /* name, type, flags, addr, offset, size */
            if ((sh[2] & 2) && sh[1] != 8 && sh[5] != 0 && sh[3] >= ROM_BASE && sh[3] + sh[5] <= ROM_END &&
                (size_t)sh[4] + sh[5] <= elfSize) {
                memcpy(src[0] + (sh[3] - ROM_BASE), elf + sh[4], sh[5]);
            }
        }
        scr_free(elf);
    }
    snprintf(path, sizeof(path), "%s/BIN/DBZP.BIN", disc);
    src[1] = read_file(path, &srcSize[1]);
    if (src[1] == NULL) {
        Vita_Fatal("the game's menu program was not found:\n  %s", path);
    }
    for (i = 0; i < gPortDatCount; i++) {
        const PortDatRun *r = &gPortDatRuns[i];
        const uint8_t *from;

        if (r->source > 1 || (size_t)r->offset + r->length > srcSize[r->source]) {
            Vita_Fatal("the list of the game's data does not fit the files in\n  %s", disc);
        }
        from = src[r->source] + r->offset;
        memcpy(r->dst, from, r->length);
        for (k = 0; k < r->length; k++) {
            hash = (hash ^ from[k]) * 0x100000001B3ull;
        }
    }
    scr_free(src[0]);
    scr_free(src[1]);
    if (gVitaHeapBase != 0 && sScrUsed != 0) {
        memset((void *)(uintptr_t)gVitaHeapBase, 0, sScrUsed); /* the game's heap starts zeroed */
        sScrUsed = 0;
    }
    if (hash != gPortDatChecksum) {
        Vita_Fatal("SLUS_216.78 / BIN/DBZP.BIN in\n  %s\nare not the unmodified USA release (SLUS-21678).", disc);
    }
}

/* The program's end address on the PS2: a table of the PS2's C library points at it; nothing reads it here
   (src/sys/heap.c takes the number itself in this build). */
unsigned char D_3BE71C[16];
