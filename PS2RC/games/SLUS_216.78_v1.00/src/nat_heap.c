/* nat_heap.c - BT3 (SLUS-216.78 v1.00): (dev) a log of the game heap's big blocks. ps2rc.cfg heap_log=1: every
 * Heap_Alloc (0x2554D8; a0 size, a3 heap) of heap_log_min bytes or more (default 64 KB) logs size, caller (ra) and
 * address, every Heap_Free (0x255508) of such a block its address - what the battle keeps in memory. Off: the
 * translated bodies only. */
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"

void fn_002554d8__ynguest(R5900Ctx *c);
void fn_00255508__ynguest(R5900Ctx *c);
static int hl_on = -1, hl_min;
#define HL_N 512
static uint32_t hl_ptr[HL_N], hl_size[HL_N], hl_ra[HL_N];
static void hl_init(void)
{
    const char *e = hle_cfg_get("heap_log"); hl_on = e ? atoi(e) : 0;
    e = hle_cfg_get("heap_log_min"); hl_min = e ? atoi(e) : 65536;
}
/* memory savings (sizes from the game's code; the PS2 values are kept with the keys at 0 / 1024):
   - slim_file=1 (default): inside File_Init (0x264BD0) every request of 64 KB or more becomes 64 bytes - CRI's AFS
     partition tables (127 + 406 KB) and the ADX players' work areas (166 + 4 x 192 KB): the ADXF / ADXT entries are
     served by adx_hle.c from its own copies, the game never reads those blocks;
   - dma_kb=N (default 512; the battle uses up to 217 KB): the two display-list buffers of Dma_InitBuffers (0x100670), N KB each instead of 1 MB;
     Dma_Flush (0x100798) logs the largest list (heap_log=1) and an overflow. */
static int hm_mode;                                  /* 1 = in File_Init, 2 = in Dma_InitBuffers */
static int hm_opt(const char *k, int d) { const char *e = hle_cfg_get(k); return e ? atoi(e) : d; }
void y_nat_002554d8(R5900Ctx *c)
{
    if (hl_on < 0) hl_init();
    if (hm_mode == 1 && (int32_t)c->r[4].w[0] >= 0x10000) c->r[4].d[0] = 0x40;
    else if (hm_mode == 2 && c->r[4].w[0] == 0x100000u) c->r[4].d[0] = SX32((uint32_t)hm_opt("dma_kb", 512) << 10);
    if (!hl_on) { fn_002554d8__ynguest(c); return; }
    const uint32_t size = c->r[4].w[0], ra = c->r[31].w[0];
    fn_002554d8__ynguest(c);
    const uint32_t p = c->r[2].w[0];
    if ((int32_t)size >= hl_min) {
        LOG("heap: alloc %u KB at %08x from %08x\n", size >> 10, p, ra);
        for (int i = 0; i < HL_N; i++) if (!hl_ptr[i]) { hl_ptr[i] = p; hl_size[i] = size; hl_ra[i] = ra; break; }
    }
}
void y_nat_00255508(R5900Ctx *c)
{
    if (hl_on < 0) hl_init();
    if (hl_on) {
        const uint32_t p = c->r[4].w[0];
        for (int i = 0; i < HL_N; i++)
            if (hl_ptr[i] == p) { LOG("heap: free %u KB at %08x (from %08x)\n", hl_size[i] >> 10, p, hl_ra[i]); hl_ptr[i] = 0; break; }
    }
    fn_00255508__ynguest(c);
}

void fn_00264bd0__ynguest(R5900Ctx *c);
void y_nat_00264bd0(R5900Ctx *c)                    /* File_Init */
{
    static int on = -1;
    if (on < 0) on = hm_opt("slim_file", 1);
    hm_mode = on ? 1 : 0;
    fn_00264bd0__ynguest(c);
    hm_mode = 0;
    if (on) LOG("memory: File_Init with 64-byte AFS tables and ADX work areas (slim_file=1: 1.4 MB of game heap free)\n");
}
void fn_00100670__ynguest(R5900Ctx *c);
void y_nat_00100670(R5900Ctx *c)                    /* Dma_InitBuffers */
{
    const uint32_t kb = (uint32_t)hm_opt("dma_kb", 512), gp = c->r[28].w[0];
    hm_mode = kb != 1024 ? 2 : 0;
    fn_00100670__ynguest(c);
    hm_mode = 0;
    if (kb != 1024) {                                /* the ends and size the original wrote for 1 MB */
        const R5900Mem m_ = {c->ram, c->ram_mask};
        const uint32_t sz = kb << 10;
        rmb_sw(m_, gp, -23016, rmb_lw(m_, gp, -23024) + sz);
        rmb_sw(m_, gp, -23012, rmb_lw(m_, gp, -23020) + sz);
        rmb_sw(m_, gp, -23008, sz);
        LOG("memory: display lists %u KB each (dma_kb)\n", kb);
    }
}
void fn_00100798__ynguest(R5900Ctx *c);
void y_nat_00100798(R5900Ctx *c)                    /* Dma_Flush: the list's size */
{
    static uint32_t peak;
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t gp = c->r[28].w[0], idx = rmb_lw(m_, gp, -23004) & 1;
    const uint32_t used = rmb_lw(m_, gp, -23000) - rmb_lw(m_, gp + 4 * idx, -23024), size = rmb_lw(m_, gp, -23008);
    if (used > size) LOG("memory: DISPLAY LIST OVERFLOW: %u KB written in a %u KB buffer\n", used >> 10, size >> 10);
    if (used > peak + (16u << 10) || (used > peak && used > size - (64u << 10))) {
        peak = used;
        if (hl_on > 0 || used > size - (64u << 10)) LOG("memory: display list peak %u KB of %u KB\n", used >> 10, size >> 10);
    }
    if (hl_on > 0) {                                 /* the battle pool's arenas (gBtlPool 0x2FEAE0: 9 x {base, cur, size, used}) */
        static uint32_t ppk[9];
        const uint32_t bp = rmb_lw(m_, 0x2FEAE0u, 0);
        if (bp)
            for (int i = 0; i < 9; i++) {
                const uint32_t b = rmb_lw(m_, bp, 16 * i), cu = rmb_lw(m_, bp, 16 * i + 4), sz = rmb_lw(m_, bp, 16 * i + 8);
                if (b && cu >= b && cu - b > ppk[i] + (8u << 10)) {
                    ppk[i] = cu - b;
                    LOG("memory: battle pool slot %d peak %u KB of %u KB\n", i, ppk[i] >> 10, sz >> 10);
                }
            }
    }
    fn_00100798__ynguest(c);
}
