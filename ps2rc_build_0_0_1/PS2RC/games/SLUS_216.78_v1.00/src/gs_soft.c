/* gs_soft.c - BT3 (SLUS-216.78 v1.00), HOST ONLY: a software model of the PS2 Graphics Synthesizer, to see what the
 * game draws. Written for this pack from the public description of the hardware (PS2Tek: GIF tags, GS registers,
 * primitives, vertex attributes, frame / Z buffers, transfers, textures, alpha blending, tests).
 *
 * The GS local memory layout (page / block / column order of every pixel format, the "swizzle") follows the tables
 * of the Play! emulator (Source/gs/GsPixelFormats.cpp, GsPixelFormats.h), used under its BSD 2-clause licence:
 *
 *   Copyright (c) 2006-2026, Jean-Philip Desjardins. All rights reserved.
 *   Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 *   the following conditions are met: * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer. * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer in the documentation and/or other
 *   materials provided with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *   ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Path: the core hands the VIF1 / GIF DMA it would drop to hle_pack_gfx_dma; the DMA chain is walked, VIF1 codes
 * give the DIRECT / DIRECTHL GIF data (PATH2), the GIF channel carries GIF data (PATH3); GIF packets set GS registers,
 * upload images and draw. VU1 programs (MSCAL: the 3D models) run in vu1_soft.c and their XGKICKs (PATH1) come
 * back here. BT3_GS_TRACE=<n> logs the drawing state changes of the frame of dump n. Every BT3_FRAME_EVERY vblanks (default 60) the displayed buffer (DISPFB2/1) is
 * written as a BMP to BT3_FRAMES (default /mnt/c/bt3ps2rc/work/runs/frames).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "hle.h"
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include "hle_cfg.h"
#include "hle_prof.h"                         /* 0.0.5: PZ_SCOPE zones gs.native / gs.vu1interp / gs.unpack */
#define HLE_IS_PSP 1
#else
#define HLE_IS_PSP 0
#endif

/* ---------------------------------------------------------------- GS local memory and its layout */
#define GSMEM (4u << 20)
static uint8_t *gm;
static int gs_alloc(void);

static const int bt32[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const int ct32[2][8] = {{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15}};
static const int bz32[4][8] = {{24,25,28,29,8,9,12,13},{26,27,30,31,10,11,14,15},{16,17,20,21,0,1,4,5},{18,19,22,23,2,3,6,7}};
static const int bt16[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const int bt16s[8][4] = {{0,2,16,18},{1,3,17,19},{8,10,24,26},{9,11,25,27},{4,6,20,22},{5,7,21,23},{12,14,28,30},{13,15,29,31}};
/* depth formats: the colour formats' block order with the block index XOR 0x18 */
static const int bz16[8][4] = {{24,26,16,18},{25,27,17,19},{28,30,20,22},{29,31,21,23},{8,10,0,2},{9,11,1,3},{12,14,4,6},{13,15,5,7}};
static const int bz16s[8][4] = {{24,26,8,10},{25,27,9,11},{16,18,0,2},{17,19,1,3},{28,30,12,14},{29,31,13,15},{20,22,4,6},{21,23,5,7}};
static const int ct16[2][16] = {{0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27},{4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31}};
static const int cw8[2][2][8] = {{{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15}},{{8,9,12,13,0,1,4,5},{10,11,14,15,2,3,6,7}}};

/* byte address of pixel (x, y) of a buffer at base (in 256-byte blocks) and width (in 64-pixel units) */
static uint32_t addr32_calc(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const int (*bt)[8])
{
    uint32_t page = (x / 64) + (y / 32) * bw;
    uint32_t px = x % 64, py = y % 32;
    uint32_t blk = bt[py / 8][px / 8];
    uint32_t col = (py % 8) / 2;
    return (bp * 256 + page * 8192 + blk * 256 + col * 64 + 4 * ct32[py % 2][px % 8]) & (GSMEM - 1);
}

static uint32_t addr16_calc(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const int (*bt)[4])
{
    uint32_t page = (x / 64) + (y / 64) * bw;
    uint32_t px = x % 64, py = y % 64;
    uint32_t blk = bt[py / 8][px / 16];
    uint32_t col = (py % 8) / 2;
    return (bp * 256 + page * 8192 + blk * 256 + col * 64 + 2 * ct16[py % 2][px % 16]) & (GSMEM - 1);
}

static uint32_t addr8_calc(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (x / 128) + (y / 64) * (bw / 2 ? bw / 2 : 1);
    uint32_t px = x % 128, py = y % 64;
    uint32_t blk = bt32[py / 16][px / 16];
    uint32_t bx = px % 16, by = py % 16;
    uint32_t col = by / 4, cy = by % 4;
    uint32_t table = ((cy & 2) >> 1) ^ (col & 1);
    uint32_t byte = ((bx & 8) >> 2) + ((cy & 2) >> 1);
    return (bp * 256 + page * 8192 + blk * 256 + col * 64 + 4 * cw8[table][cy & 1][bx & 7] + byte) & (GSMEM - 1);
}

/* PSMT4: returns the byte address, *shift = 0 or 4 */
static uint32_t addr4_calc(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int *shift)
{
    uint32_t page = (x / 128) + (y / 128) * (bw / 2 ? bw / 2 : 1);
    uint32_t px = x % 128, py = y % 128;
    uint32_t blk = bt16[py / 16][px / 32];
    uint32_t bx = px % 32, by = py % 16;
    uint32_t col = by / 4, cy = by % 4;
    uint32_t sh = (bx & 0x18) + ((cy & 2) << 1);
    uint32_t sub = ((cy & 2) >> 1) ^ (col & 1);
    uint32_t word = cw8[sub][cy & 1][bx & 7];
    uint32_t a = bp * 256 + page * 8192 + blk * 256 + col * 64 + 4 * word + sh / 8;
    *shift = sh & 4;
    return a & (GSMEM - 1);
}

/* the same addresses from tables: the offset of every pixel inside its 8 KB page, built once from the formulas
   above (68 KB). Uploads write every texel through these (the title uploads ~2.8 MB of textures per frame); the
   depth formats are the colour ones with the block number XOR 0x18, i.e. the offset XOR 0x1800. */
static uint16_t tab32[32 * 64], tab16[64 * 64], tab16s[64 * 64], tab8[64 * 128], tab4[128 * 128];
static int tabs_ready;
static void swz_tables(void)
{
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 64; x++) tab32[y * 64 + x] = (uint16_t)addr32_calc(0, 1, x, y, bt32);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            tab16[y * 64 + x] = (uint16_t)addr16_calc(0, 1, x, y, bt16);
            tab16s[y * 64 + x] = (uint16_t)addr16_calc(0, 1, x, y, bt16s);
        }
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 128; x++) tab8[y * 128 + x] = (uint16_t)addr8_calc(0, 2, x, y);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) {
            int sh;
            uint32_t a = addr4_calc(0, 2, x, y, &sh);
            tab4[y * 128 + x] = (uint16_t)(a << 1 | (sh ? 1 : 0));
        }
    tabs_ready = 1;
}
static inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const int (*bt)[8])
{
    if (!tabs_ready) swz_tables();
    uint32_t page = (x >> 6) + (y >> 5) * bw;
    uint32_t o = tab32[(y & 31) * 64 + (x & 63)];
    if (bt != bt32) o ^= 0x1800;
    return (bp * 256 + page * 8192 + o) & (GSMEM - 1);
}
static inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const int (*bt)[4])
{
    if (!tabs_ready) swz_tables();
    uint32_t page = (x >> 6) + (y >> 6) * bw;
    uint32_t o = (bt == bt16 || bt == bz16 ? tab16 : tab16s)[(y & 63) * 64 + (x & 63)];
    if (bt == bz16 || bt == bz16s) o ^= 0x1800;
    return (bp * 256 + page * 8192 + o) & (GSMEM - 1);
}
static inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!tabs_ready) swz_tables();
    uint32_t page = (x >> 7) + (y >> 6) * (bw / 2 ? bw / 2 : 1);
    return (bp * 256 + page * 8192 + tab8[(y & 63) * 128 + (x & 127)]) & (GSMEM - 1);
}
static inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int *shift)
{
    if (!tabs_ready) swz_tables();
    uint32_t page = (x >> 7) + (y >> 7) * (bw / 2 ? bw / 2 : 1);
    uint32_t t = tab4[(y & 127) * 128 + (x & 127)];
    *shift = (t & 1) ? 4 : 0;
    return (bp * 256 + page * 8192 + (t >> 1)) & (GSMEM - 1);
}

enum { PSMCT32 = 0, PSMCT24 = 1, PSMCT16 = 2, PSMCT16S = 0x0A, PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B,
       PSMT4HL = 0x24, PSMT4HH = 0x2C, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A };

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, gm + a, 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(gm + a, &v, 4); }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, gm + a, 2); return v; }
static void wr16(uint32_t a, uint16_t v) { memcpy(gm + a, &v, 2); }

/* raw value of pixel (x, y) in format psm (indices for T8 / T4, colour words otherwise) */
static uint32_t pix_read(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y)
{
    int sh;
    switch (psm) {
    case PSMCT32: case PSMCT24: return rd32(addr32(bp, bw, x, y, bt32));
    case PSMZ32: case PSMZ24: return rd32(addr32(bp, bw, x, y, bz32));
    case PSMCT16: return rd16(addr16(bp, bw, x, y, bt16));
    case PSMCT16S: return rd16(addr16(bp, bw, x, y, bt16s));
    case PSMZ16: return rd16(addr16(bp, bw, x, y, bz16));
    case PSMZ16S: return rd16(addr16(bp, bw, x, y, bz16s));
    case PSMT8: return gm[addr8(bp, bw, x, y)];
    case PSMT4: { uint32_t a = addr4(bp, bw, x, y, &sh); return (gm[a] >> sh) & 15; }
    case PSMT8H: return rd32(addr32(bp, bw, x, y, bt32)) >> 24;
    case PSMT4HL: return (rd32(addr32(bp, bw, x, y, bt32)) >> 24) & 15;
    case PSMT4HH: return rd32(addr32(bp, bw, x, y, bt32)) >> 28;
    default: return 0;
    }
}

static int dbg_opt(const char *k, int def)
{
#ifdef HLE_PSP
    const char *e = hle_cfg_get(k);
#else
    const char *e = getenv(k);
#endif
    return e ? atoi(e) : def;
}
#ifdef HLE_PSP
static int gs_in_upload;                     /* gs_verify: writes outside an upload are marked on their page */
static uint8_t ge_pwho[512];
static uint32_t gs_odd_writes;
#endif
static void pix_write(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t v)
{
#ifdef HLE_PSP
    if (!gs_in_upload) {                      /* a write outside an upload: the page's content id is stale */
        uint32_t a8 = addr32(bp, bw, x, y, bt32) >> 13;   /* (the page, close enough for every format) */
        extern void ge_page_touched(uint32_t page);
        ge_page_touched(a8);
        gs_odd_writes++;
    }
#endif
    int sh;
    uint32_t a;
    switch (psm) {
    case PSMCT32: wr32(addr32(bp, bw, x, y, bt32), v); break;
    case PSMCT24: a = addr32(bp, bw, x, y, bt32); wr32(a, (rd32(a) & 0xFF000000u) | (v & 0xFFFFFF)); break;
    case PSMZ32: wr32(addr32(bp, bw, x, y, bz32), v); break;
    case PSMZ24: a = addr32(bp, bw, x, y, bz32); wr32(a, (rd32(a) & 0xFF000000u) | (v & 0xFFFFFF)); break;
    case PSMCT16: wr16(addr16(bp, bw, x, y, bt16), (uint16_t)v); break;
    case PSMCT16S: wr16(addr16(bp, bw, x, y, bt16s), (uint16_t)v); break;
    case PSMZ16: wr16(addr16(bp, bw, x, y, bz16), (uint16_t)v); break;
    case PSMZ16S: wr16(addr16(bp, bw, x, y, bz16s), (uint16_t)v); break;
    case PSMT8: gm[addr8(bp, bw, x, y)] = (uint8_t)v; break;
    case PSMT4: a = addr4(bp, bw, x, y, &sh); gm[a] = (uint8_t)((gm[a] & ~(15 << sh)) | ((v & 15) << sh)); break;
    case PSMT8H: a = addr32(bp, bw, x, y, bt32); wr32(a, (rd32(a) & 0x00FFFFFFu) | (v << 24)); break;
    case PSMT4HL: a = addr32(bp, bw, x, y, bt32); wr32(a, (rd32(a) & 0xF0FFFFFFu) | ((v & 15) << 24)); break;
    case PSMT4HH: a = addr32(bp, bw, x, y, bt32); wr32(a, (rd32(a) & 0x0FFFFFFFu) | ((v & 15) << 28)); break;
    default: break;
    }
}

/* 16-bit colour <-> RGBA8888 (TEXA: alpha of 16 / 24-bit colours) */
static uint64_t texa;
static uint32_t c16to32(uint32_t c, int texa_on)
{
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    uint32_t a = texa_on ? ((c & 0x8000) ? (uint32_t)(texa >> 32) & 0xFF
                                         : ((texa >> 15) & 1) && !(c & 0x7FFF) ? 0 : (uint32_t)texa & 0xFF)
                         : ((c & 0x8000) ? 0x80 : 0);
    return r | (g << 8) | (b << 16) | (a << 24);
}
static uint32_t c32to16(uint32_t c)
{
    return ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10) | ((c >> 31) << 15);
}

/* ---------------------------------------------------------------- GS registers */
static uint64_t st_vif[128], st_gif[4], st_regs[0x64], st_ch[3];
static uint64_t reg[0x64];
static int ctx_of_prim(void);
#define PRIM reg[0x00]
#define RGBAQ reg[0x01]

typedef struct { int32_t x, y; uint32_t z; uint32_t rgba; float s, t, q; uint32_t u, v; } Vtx;
static Vtx vq[3];
static int nvq;
static uint64_t cur_st;           /* last ST (S, T) */
static float cur_q = 1.0f;

/* CLUT: the GS copies a palette from GS memory into its CLUT buffer when TEX0 / TEX2 is written (CLD: 0 keep,
   1 load, 2 load + CBP0, 3 load + CBP1, 4 load if CBP != CBP0, 5 load if CBP != CBP1) and draws from the buffer.
   The game rewrites palette memory after loading it (and draws 4-bit textures from sub-tables of a loaded 256-colour
   palette with CSA): read at draw time, those came out white or as noise. ps2rc.cfg gs_cld=0: the old reading. */
static uint32_t clut_buf[512];                           /* raw entries: CT32 colours or CT16 values */
static uint32_t clut_cbp0 = 0xFFFFFFFFu, clut_cbp1 = 0xFFFFFFFFu;
static int clut_mode = -1;
static int psm_indexed(uint32_t psm)
{
    return psm == PSMT8 || psm == PSMT8H || psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
}
#ifdef HLE_PSP
static void ge_materialise(uint32_t p0, uint32_t p1);
static void ge_clut_loaded(void);
#endif
static uint32_t clut_base(uint64_t tex0, int t4)         /* where the palette sits in the buffer */
{
    uint32_t cpsm = (uint32_t)(tex0 >> 51) & 15, csa = (uint32_t)(tex0 >> 56) & 31;
    int c32 = cpsm == PSMCT32 || cpsm == PSMCT24;
    if (t4) return c32 ? (csa & 15) * 16 : csa * 16;
    return c32 ? 0 : (csa & 16) * 16;
}
static void clut_load(uint64_t tex0)
{
    if (clut_mode < 0) {
#ifdef HLE_PSP
        const char *e = hle_cfg_get("gs_cld");
#else
        const char *e = getenv("BT3_GS_CLD");
#endif
        clut_mode = e ? atoi(e) : 1;
    }
    uint32_t psm = (uint32_t)(tex0 >> 20) & 63;
    if (!clut_mode || !psm_indexed(psm)) return;
    uint32_t cbp = (uint32_t)(tex0 >> 37) & 0x3FFF, cld = (uint32_t)(tex0 >> 61) & 7;
    int load = 0;
    switch (cld) {
    case 1: load = 1; break;
    case 2: load = 1; clut_cbp0 = cbp; break;
    case 3: load = 1; clut_cbp1 = cbp; break;
    case 4: if (cbp != clut_cbp0) { load = 1; clut_cbp0 = cbp; } break;
    case 5: if (cbp != clut_cbp1) { load = 1; clut_cbp1 = cbp; } break;
    default: break;
    }
    if (!load) return;
    int t4 = !(psm == PSMT8 || psm == PSMT8H);
    uint32_t cpsm = (uint32_t)(tex0 >> 51) & 15, csm = (uint32_t)(tex0 >> 55) & 1;
    int c32 = cpsm == PSMCT32 || cpsm == PSMCT24;
#ifdef HLE_PSP
    ge_materialise(cbp / 32, cbp / 32 + 2);
#endif
    uint32_t base = clut_base(tex0, t4), n = t4 ? 16 : 256;
    uint64_t tc = reg[0x1C];
    uint32_t cbw = (uint32_t)tc & 63, cou = (uint32_t)(tc >> 6) & 63, cov = (uint32_t)(tc >> 12) & 0x3FF;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c;
        if (csm == 0) {
            uint32_t x, y;
            if (t4) { x = i & 7; y = i >> 3; }                               /* 8 x 2 */
            else { uint32_t e = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); x = e & 15; y = e >> 4; }
            c = pix_read(cbp, 1, c32 ? PSMCT32 : cpsm, x, y);
        } else c = pix_read(cbp, cbw, PSMCT16, cou * 16 + i, cov);
        clut_buf[(base + i) & 511] = c;
    }
#ifdef HLE_PSP
    ge_clut_loaded();
#endif
}
static uint32_t clut_lookup(uint64_t tex0, uint32_t idx, int t4)
{
    uint32_t cbp = (uint32_t)(tex0 >> 37) & 0x3FFF, cpsm = (uint32_t)(tex0 >> 51) & 15, csm = (uint32_t)(tex0 >> 55) & 1;
    uint32_t csa = (uint32_t)(tex0 >> 56) & 31;
    uint32_t x, y;
    if (clut_mode > 0) {
        uint32_t c = clut_buf[(clut_base(tex0, t4) + idx) & 511];
        return (cpsm == PSMCT32 || cpsm == PSMCT24) ? c : c16to32(c, 1);
    }
    if (t4) idx += (csa & 15) * 16 + (cpsm == PSMCT32 || cpsm == PSMCT24 ? 0 : (csa >> 4) * 256);
    if (csm == 0) {                                     /* CSM1: 16 x 16 (8-bit) or 8 x 2 (4-bit), bits 3 and 4 swapped */
        uint32_t e = (idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1);
        x = e & 15; y = e >> 4;
        uint32_t c = pix_read(cbp, 1, cpsm == PSMCT32 || cpsm == PSMCT24 ? PSMCT32 : cpsm, x, y);
        return (cpsm == PSMCT32 || cpsm == PSMCT24) ? c : c16to32(c, 1);
    }
    /* CSM2: a line of 16-bit colours at TEXCLUT (cbw, cou, cov) */
    uint64_t tc = reg[0x1C];
    uint32_t cbw = (uint32_t)tc & 63, cou = (uint32_t)(tc >> 6) & 63, cov = (uint32_t)(tc >> 12) & 0x3FF;
    return c16to32(pix_read(cbp, cbw, PSMCT16, cou * 16 + idx, cov), 1);
}

static uint64_t cur_clamp;                               /* CLAMP_1/2 of the primitive being drawn */

/* CLAMP: WMS / WMT 0 REPEAT, 1 CLAMP, 2 REGION_CLAMP (MIN..MAX), 3 REGION_REPEAT ((c & MIN) | MAX) */
static int32_t wrap(int32_t c, uint32_t size, uint32_t mode, uint32_t mn, uint32_t mx)
{
    switch (mode) {
    case 0: return c & (int32_t)(size - 1);
    case 1: return c < 0 ? 0 : c >= (int32_t)size ? (int32_t)size - 1 : c;
    case 2: return c < (int32_t)mn ? (int32_t)mn : c > (int32_t)mx ? (int32_t)mx : c;
    default: return (int32_t)(((uint32_t)c & mn) | mx);
    }
}

static uint32_t tex_fetch(uint64_t tex0, int32_t u, int32_t v)
{
    uint32_t tbp = (uint32_t)tex0 & 0x3FFF, tbw = (uint32_t)(tex0 >> 14) & 63, psm = (uint32_t)(tex0 >> 20) & 63;
    uint32_t tw = 1u << ((tex0 >> 26) & 15), th = 1u << ((tex0 >> 30) & 15);
    uint64_t cl = cur_clamp;
    u = wrap(u, tw, (uint32_t)cl & 3, (uint32_t)(cl >> 4) & 0x3FF, (uint32_t)(cl >> 14) & 0x3FF);
    v = wrap(v, th, (uint32_t)(cl >> 2) & 3, (uint32_t)(cl >> 24) & 0x3FF, (uint32_t)(cl >> 34) & 0x3FF);
    if (u < 0 || v < 0) return 0;
    int texa_on = 1;
    switch (psm) {
    case PSMCT32: case PSMZ32: return pix_read(tbp, tbw, psm, (uint32_t)u, (uint32_t)v);
    case PSMCT24: case PSMZ24: {                         /* alpha TEXA.TA0; AEM: 0 for black */
        uint32_t c = pix_read(tbp, tbw, psm, (uint32_t)u, (uint32_t)v) & 0xFFFFFF;
        return c | (((texa >> 15) & 1) && !c ? 0 : ((uint32_t)texa & 0xFF) << 24); }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return c16to32(pix_read(tbp, tbw, psm, (uint32_t)u, (uint32_t)v), texa_on);
    case PSMT8: case PSMT8H: return clut_lookup(tex0, pix_read(tbp, tbw, psm, (uint32_t)u, (uint32_t)v), 0);
    case PSMT4: case PSMT4HL: case PSMT4HH: return clut_lookup(tex0, pix_read(tbp, tbw, psm, (uint32_t)u, (uint32_t)v), 1);
    default: return 0xFFFF00FFu;
    }
}

/* ---------------------------------------------------------------- pixel pipeline */
static int ctx_of_prim(void)
{
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];     /* PRMODECONT.AC: attributes from PRIM, else PRMODE */
    return (int)((p >> 9) & 1);
}

static int alpha_test(uint64_t test, uint32_t a)
{
    if (!(test & 1)) return 1;
    uint32_t aref = (uint32_t)(test >> 4) & 0xFF;
    switch ((test >> 1) & 7) {
    case 0: return 0; case 1: return 1; case 2: return a < aref; case 3: return a <= aref;
    case 4: return a == aref; case 5: return a >= aref; case 6: return a > aref; default: return a != aref;
    }
}

static uint64_t stat_pixels, stat_prims, stat_uploads;

static void put_pixel(int ctx, int x, int y, uint32_t z, uint32_t c, int abe)
{
    uint64_t frame = reg[0x4C + ctx], zbuf = reg[0x4E + ctx], sc = reg[0x40 + ctx], test = reg[0x47 + ctx];
    if (x < (int)(sc & 0x7FF) || x > (int)((sc >> 16) & 0x7FF) || y < (int)((sc >> 32) & 0x7FF) || y > (int)((sc >> 48) & 0x7FF))
        return;
    uint32_t fbp = (uint32_t)(frame & 0x1FF) * 32, fbw = (uint32_t)(frame >> 16) & 63, fpsm = (uint32_t)(frame >> 24) & 63;
    uint32_t fbmsk = (uint32_t)(frame >> 32);
    int f16 = fpsm == PSMCT16 || fpsm == PSMCT16S;
    uint32_t a = c >> 24;
    int write_fb = 1, write_z = !((zbuf >> 32) & 1), rgb_only = 0;
    if (!alpha_test(test, a)) {                          /* AFAIL: 0 KEEP, 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY */
        switch ((test >> 12) & 3) {
        case 0: return;
        case 1: write_z = 0; break;
        case 2: write_fb = 0; break;
        default: write_z = 0; rgb_only = 1; break;
        }
    }
    uint32_t dst = 0;
    int have_dst = 0;
    if ((test >> 14) & 1) {                              /* DATE: destination alpha test against DATM */
        dst = pix_read(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y);
        have_dst = 1;
        uint32_t da = f16 ? (dst >> 15) & 1 : (dst >> 31) & 1;
        if (da != ((test >> 15) & 1)) return;
    }
    uint32_t zbp = (uint32_t)(zbuf & 0x1FF) * 32, zpsm = ((uint32_t)(zbuf >> 24) & 15) | 0x30;
    if (test & 0x10000) {                                /* ZTE */
        uint32_t zo = pix_read(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y);
        if (zpsm == PSMZ24) zo &= 0xFFFFFF;
        if (zpsm == PSMZ24 && z > 0xFFFFFF) z = 0xFFFFFF;
        if ((zpsm == PSMZ16 || zpsm == PSMZ16S) && z > 0xFFFF) z = 0xFFFF;
        switch ((test >> 17) & 3) {
        case 0: return;
        case 1: break;
        case 2: if (z < zo) return; break;
        case 3: if (z <= zo) return; break;
        }
    } else write_z = 0;
    if (write_z) pix_write(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y, z);
    if (!write_fb) return;
    if (!have_dst && (abe || fbmsk || rgb_only)) {
        dst = pix_read(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y);
        have_dst = 1;
    }
    uint32_t dst32 = have_dst ? (f16 ? c16to32(dst, 0) : dst) : 0;
    if (abe && (!(reg[0x49] & 1) || (a & 0x80))) {      /* Cv = (A - B) * C >> 7 + D; PABE: only when As MSB */
        uint64_t al = reg[0x42 + ctx];
        uint32_t ca = (uint32_t)al & 3, cb = (uint32_t)(al >> 2) & 3, cc = (uint32_t)(al >> 4) & 3, cd = (uint32_t)(al >> 6) & 3;
        uint32_t fix = (uint32_t)(al >> 32) & 0xFF;
        uint32_t aa = cc == 0 ? a : cc == 1 ? dst32 >> 24 : fix;
        int clampc = (int)(reg[0x46] & 1);
        uint32_t out = 0;
        for (int k = 0; k < 3; k++) {
            int cs = (int)((c >> (8 * k)) & 0xFF), cdst = (int)((dst32 >> (8 * k)) & 0xFF);
            int A = ca == 0 ? cs : ca == 1 ? cdst : 0, B = cb == 0 ? cs : cb == 1 ? cdst : 0;
            int D = cd == 0 ? cs : cd == 1 ? cdst : 0;
            int v = (((A - B) * (int)aa) >> 7) + D;
            if (clampc) { if (v < 0) v = 0; if (v > 255) v = 255; } else v &= 0xFF;
            out |= (uint32_t)v << (8 * k);
        }
        c = out | (c & 0xFF000000u);
    }
    if (reg[0x4A + ctx] & 1) c |= 0x80000000u;          /* FBA: alpha MSB forced */
    if (f16) {
        uint32_t v = c32to16(c), m16 = ((fbmsk >> 3) & 31) | (((fbmsk >> 11) & 31) << 5) | (((fbmsk >> 19) & 31) << 10)
                                      | ((fbmsk >> 31) << 15);
        if (rgb_only) m16 |= 0x8000;
        v = (v & ~m16) | (dst & m16);
        pix_write(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, v);
    } else {
        uint32_t m = fbmsk | (rgb_only ? 0xFF000000u : 0);
        if (fpsm == PSMCT24) m |= 0xFF000000u;
        c = (c & ~m) | (dst & m);
        pix_write(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, c);
    }
    stat_pixels++;
}

/* colour of a textured / untextured pixel (TFX: 0 modulate, 1 decal, 2 highlight, 3 highlight2) */
static uint32_t shade(uint64_t p, int ctx, uint32_t vc, float s, float t, float q, int32_t u, int32_t v)
{
    if (!((p >> 4) & 1)) return vc;
    uint64_t tex0 = reg[0x06 + ctx];
    uint32_t tw = 1u << ((tex0 >> 26) & 15), th = 1u << ((tex0 >> 30) & 15);
    int32_t tu, tv;                                      /* texel coordinates in 1/16 texel */
    if ((p >> 8) & 1) { tu = u; tv = v; }                /* FST: UV in 12.4 texels */
    else {
        float iq = q != 0.0f ? 1.0f / q : 1.0f;
        tu = (int32_t)floorf(s * iq * (float)tw * 16.0f);
        tv = (int32_t)floorf(t * iq * (float)th * 16.0f);
    }
    cur_clamp = reg[0x08 + ctx];
    uint64_t tex1 = reg[0x14 + ctx];
    uint32_t tc;
    if ((tex1 >> 5) & 1) {                               /* MMAG LINEAR: bilinear between the 4 nearest texels */
        int32_t bu = tu - 8, bv = tv - 8, x0 = bu >> 4, y0 = bv >> 4, fx = bu & 15, fy = bv & 15;
        uint32_t t00 = tex_fetch(tex0, x0, y0), t10 = tex_fetch(tex0, x0 + 1, y0);
        uint32_t t01 = tex_fetch(tex0, x0, y0 + 1), t11 = tex_fetch(tex0, x0 + 1, y0 + 1);
        tc = 0;
        for (int k = 0; k < 4; k++) {
            uint32_t c00 = (t00 >> (8 * k)) & 0xFF, c10 = (t10 >> (8 * k)) & 0xFF;
            uint32_t c01 = (t01 >> (8 * k)) & 0xFF, c11 = (t11 >> (8 * k)) & 0xFF;
            uint32_t top = c00 * (16 - fx) + c10 * fx, bot = c01 * (16 - fx) + c11 * fx;
            tc |= ((top * (16 - fy) + bot * fy) >> 8) << (8 * k);
        }
    } else tc = tex_fetch(tex0, tu >> 4, tv >> 4);
    uint32_t tfx = (uint32_t)(tex0 >> 35) & 3, tcc = (uint32_t)(tex0 >> 34) & 1;
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t ct = (tc >> (8 * k)) & 0xFF, cf = (vc >> (8 * k)) & 0xFF, r;
        if (tfx == 1) r = ct;
        else if (tfx == 0) r = (ct * cf) >> 7;
        else r = ((ct * cf) >> 7) + (vc >> 24);
        out |= (r > 255 ? 255 : r) << (8 * k);
    }
    uint32_t at = tc >> 24, af = vc >> 24, a;
    if (!tcc) a = af;
    else if (tfx == 0) { a = (at * af) >> 7; if (a > 255) a = 255; }
    else if (tfx == 2) { a = at + af; if (a > 255) a = 255; }
    else a = at;
    return out | (a << 24);
}

/* rasterise only in the vblanks before a dump (BT3_DRAW_ALL=1: always); state and uploads are kept every frame */
static int draw_on = 1;
static int bench_ff;                         /* benchmark fast-forward (bench_at): data processed, nothing drawn */
static int vu_need_mscal;
static uint32_t vu_interp_us, vu_prog_h[12], vu_prog_us[12], vu_prog_runs[12];   /* interpreter cost (bench) */
static uint64_t vu_interp_runs, vu_interp_pairs;
static uint32_t t_unpack_us, t_gif_us, t_vifstream_us, t_dmagather_us;   /* (bench) */
uint32_t vu1_micro_hash(void);                    /* after a benchmark fast-forward: MSCNTs wait for an MSCAL */

/* the PSP's GE back end (gs_ge.h, included at the end of this file): GE_ACTIVE = primitives go to the GE */
#ifdef HLE_PSP
static int ge_on;
static void ge_tri(const Vtx *a, const Vtx *b, const Vtx *c);
static void ge_sprite(const Vtx *a, const Vtx *b);
static void ge_mark_upload(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y, uint32_t h);
static void ge_upload_begin(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
static void ge_upload_data(const uint8_t *q, uint32_t n);
static void ge_upload_end(void);
static int ge_upload_try_skip(const uint8_t *q, uint32_t n, uint32_t left);
static int ge_upload_defer(const uint8_t *q, uint32_t n, uint32_t left);
static void ge_materialise_upload(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t y, uint32_t h, uint32_t x,
                                  uint32_t w);
static void ge_vblank(void);
static int ge_init(void);
static uint32_t *ge_shown_pixels(void);
static void ge_burst_log(unsigned k);
static void ge_materialise_all(void);
static uint32_t ge_nat_us, ge_prim_us, ge_texlook_us;
static uint64_t ge_st_flips_get(void);
static void ge_bench_report(void);
static int ge_vu1_native(int cont, uint32_t pair, uint32_t top);
static uint32_t ge_dma_us;
#define GE_ACTIVE (ge_on > 0)
#else
#define GE_ACTIVE 0
#define ge_tri(a, b, c) ((void)0)
#define ge_sprite(a, b) ((void)0)
#define ge_mark_upload(bp, bw, psm, y, h) ((void)0)
#endif

/* BT3_GS_TRACE=<dump index>: in the vblank before that dump, log every change of drawing state (one line per batch
 * of primitives drawn with the same registers, with the batch's count and bounding box) */
static int trace_on;
static uint64_t tr_key[9];
static unsigned tr_n;
static int tr_x0, tr_y0, tr_x1, tr_y1;
static void snap_buffer(unsigned k, uint32_t fbp);
static unsigned tr_batches, tr_post;
static void trace_flush(void)
{
    if (!tr_n) return;
    tr_batches++;
    /* snapshot after every batch except plain 3D batches into a frame buffer (prims 3-5 without mask) */
    uint32_t fbp = (uint32_t)tr_key[1] & 0x1FF;
    int plain3d = (tr_key[0] & 7) >= 3 && (tr_key[0] & 7) <= 5 && !(tr_key[1] >> 32);
    if (!plain3d && tr_post < 250) snap_buffer(tr_batches, fbp == 0 ? 0 : 0x70), tr_post++;
    LOG("gstrace: %4u x prim %03llx fr %llx zb %llx tex0 %llx tex1 %llx al %llx test %llx clamp %llx fba %llx bbox %d,%d-%d,%d\n",
        tr_n, (unsigned long long)tr_key[0], (unsigned long long)tr_key[1], (unsigned long long)tr_key[2],
        (unsigned long long)tr_key[3], (unsigned long long)tr_key[4], (unsigned long long)tr_key[5],
        (unsigned long long)tr_key[6], (unsigned long long)tr_key[7], (unsigned long long)tr_key[8],
        tr_x0, tr_y0, tr_x1, tr_y1);
    tr_n = 0;
}
static void trace_prim(uint64_t p, int ctx, int x0, int y0, int x1, int y1)
{
    uint64_t k[9] = {p & 0x7FF, reg[0x4C + ctx], reg[0x4E + ctx], (p >> 4) & 1 ? reg[0x06 + ctx] : 0,
                     (p >> 4) & 1 ? reg[0x14 + ctx] : 0, (p >> 6) & 1 ? reg[0x42 + ctx] : 0, reg[0x47 + ctx],
                     (p >> 4) & 1 ? reg[0x08 + ctx] : 0, reg[0x4A + ctx]};
    if (tr_n && memcmp(k, tr_key, sizeof k)) trace_flush();
    if (!tr_n) { memcpy(tr_key, k, sizeof k); tr_x0 = x0; tr_y0 = y0; tr_x1 = x1; tr_y1 = y1; }
    if (x0 < tr_x0) tr_x0 = x0;
    if (y0 < tr_y0) tr_y0 = y0;
    if (x1 > tr_x1) tr_x1 = x1;
    if (y1 > tr_y1) tr_y1 = y1;
    tr_n++;
}

static void draw_sprite(const Vtx *a, const Vtx *b)
{
    if (!draw_on) return;
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    uint64_t xyo = reg[0x18 + ctx];
    int ox = (int)(xyo & 0xFFFF), oy = (int)((xyo >> 32) & 0xFFFF);
    int x0 = (a->x - ox) >> 4, y0 = (a->y - oy) >> 4, x1 = (b->x - ox) >> 4, y1 = (b->y - oy) >> 4;
    const Vtx *l = a, *r = b;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; l = b; r = a; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int w = x1 - x0 ? x1 - x0 : 1, h = y1 - y0 ? y1 - y0 : 1;
    int abe = (int)((p >> 6) & 1);
    if (trace_on) trace_prim(p, ctx, x0, y0, x1, y1);
    for (int y = y0; y < y1; y++) {
        float fy = (float)(y - y0) / (float)h;
        for (int x = x0; x < x1; x++) {
            float fx = (float)(x - x0) / (float)w;
            int32_t u = (int32_t)(a->u + ((int32_t)b->u - (int32_t)a->u) * (a == l ? fx : 1 - fx));
            int32_t v = (int32_t)(a->v + ((int32_t)b->v - (int32_t)a->v) * fy);
            float s = a->s + (b->s - a->s) * fx, t = a->t + (b->t - a->t) * fy;
            uint32_t c = shade(p, ctx, b->rgba, s, t, b->q, u, v);
            put_pixel(ctx, x, y, b->z, c, abe);
        }
    }
    (void)r;
    stat_prims++;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* triangles: float only (the PSP has no double-precision hardware); the bounding box is clipped to the scissor
 * first (games send triangles far larger than the screen); attributes are interpolated from the first vertex
 * (a0 + w1 * (a1 - a0) + w2 * (a2 - a0)): exact for a flat triangle, so the outline hull drawn at the body's depth
 * no longer shows through as dark speckles (weights summing to just under 1 put depth one unit low) */
static void draw_tri(const Vtx *v0, const Vtx *v1, const Vtx *v2)
{
    if (!draw_on) return;
    uint64_t p = (reg[0x1A] & 1) ? PRIM : reg[0x1B];
    int ctx = (int)((p >> 9) & 1);
    uint64_t xyo = reg[0x18 + ctx], sc = reg[0x40 + ctx];
    float ox = (float)(xyo & 0xFFFF), oy = (float)((xyo >> 32) & 0xFFFF);
    float X[3] = {(v0->x - ox) / 16.f, (v1->x - ox) / 16.f, (v2->x - ox) / 16.f};
    float Y[3] = {(v0->y - oy) / 16.f, (v1->y - oy) / 16.f, (v2->y - oy) / 16.f};
    float area = edge(X[0], Y[0], X[1], Y[1], X[2], Y[2]);
    if (fabsf(area) < 1e-6f) return;
    int minx = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), maxx = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
    int miny = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), maxy = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
    int scx0 = (int)(sc & 0x7FF), scx1 = (int)((sc >> 16) & 0x7FF) + 1;
    int scy0 = (int)((sc >> 32) & 0x7FF), scy1 = (int)((sc >> 48) & 0x7FF) + 1;
    if (minx < scx0) minx = scx0;
    if (miny < scy0) miny = scy0;
    if (maxx > scx1) maxx = scx1;
    if (maxy > scy1) maxy = scy1;
    if (minx >= maxx || miny >= maxy) return;
    if (trace_on) trace_prim(p, ctx, minx, miny, maxx, maxy);
    int iip = (int)((p >> 3) & 1), abe = (int)((p >> 6) & 1);
    float ia = 1.0f / area;
    float z0 = (float)v0->z, dz1 = (float)v1->z - z0, dz2 = (float)v2->z - z0;
    float s0 = v0->s, ds1 = v1->s - s0, ds2 = v2->s - s0;
    float t0 = v0->t, dt1 = v1->t - t0, dt2 = v2->t - t0;
    float q0 = v0->q, dq1 = v1->q - q0, dq2 = v2->q - q0;
    float u0 = (float)v0->u, du1 = (float)v1->u - u0, du2 = (float)v2->u - u0;
    float vv0 = (float)v0->v, dv1 = (float)v1->v - vv0, dv2 = (float)v2->v - vv0;
    float c0[4], dc1[4], dc2[4];
    for (int k = 0; k < 4; k++) {
        c0[k] = (float)((v0->rgba >> (8 * k)) & 0xFF);
        dc1[k] = (float)((v1->rgba >> (8 * k)) & 0xFF) - c0[k];
        dc2[k] = (float)((v2->rgba >> (8 * k)) & 0xFF) - c0[k];
    }
    for (int y = miny; y < maxy; y++) {
        float py = y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f;
            float w1 = edge(X[2], Y[2], X[0], Y[0], px, py) * ia;
            float w2 = edge(X[0], Y[0], X[1], Y[1], px, py) * ia;
            if (w1 < 0 || w2 < 0 || w1 + w2 > 1.0f) continue;
            uint32_t col = 0;
            if (iip) {
                for (int k = 0; k < 4; k++) {
                    float cv = c0[k] + w1 * dc1[k] + w2 * dc2[k];
                    col |= (uint32_t)(cv > 255.f ? 255.f : cv < 0 ? 0 : cv + 0.5f) << (8 * k);
                }
            } else col = v2->rgba;
            float z = z0 + (w1 * dz1 + w2 * dz2);
            put_pixel(ctx, x, y, (uint32_t)(z < 0 ? 0 : z + 0.5f),
                      shade(p, ctx, col, s0 + w1 * ds1 + w2 * ds2, t0 + w1 * dt1 + w2 * dt2, q0 + w1 * dq1 + w2 * dq2,
                            (int32_t)(u0 + w1 * du1 + w2 * du2), (int32_t)(vv0 + w1 * dv1 + w2 * dv2)), abe);
        }
    }
    stat_prims++;
}

/* a vertex arrives (XYZ2 / XYZF2 kick, or 3 = no kick) */
static void vertex(int32_t x, int32_t y, uint32_t z, int kick)
{
    Vtx v;
    v.x = x; v.y = y; v.z = z;
    v.rgba = (uint32_t)RGBAQ;
    v.s = 0; v.t = 0;
    { uint32_t sw = (uint32_t)cur_st, tw = (uint32_t)(cur_st >> 32); memcpy(&v.s, &sw, 4); memcpy(&v.t, &tw, 4); }
    { uint32_t qw = (uint32_t)(RGBAQ >> 32); memcpy(&v.q, &qw, 4); if (v.q == 0.0f) v.q = cur_q; }
    v.u = (uint32_t)reg[0x03] & 0x3FFF;
    v.v = (uint32_t)(reg[0x03] >> 16) & 0x3FFF;
    uint32_t prim = (uint32_t)PRIM & 7;
    int need = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : 3;
    if (nvq < 3) vq[nvq++] = v;
    else { vq[0] = vq[1]; vq[1] = vq[2]; vq[2] = v; }
    if (nvq < need) return;
    if (!kick) {
        if (prim == 3 || prim == 1 || prim == 6 || prim == 0) nvq = 0;
        return;
    }
    switch (prim) {
    case 0: nvq = 0; break;                                            /* points: not drawn */
    case 1: nvq = 0; break;                                            /* lines: not drawn */
    case 2: vq[0] = vq[1]; nvq = 1; break;
    case 3: if (GE_ACTIVE) ge_tri(&vq[0], &vq[1], &vq[2]); else draw_tri(&vq[0], &vq[1], &vq[2]); nvq = 0; break;
    case 4: if (GE_ACTIVE) ge_tri(&vq[nvq - 3], &vq[nvq - 2], &vq[nvq - 1]); else draw_tri(&vq[nvq - 3], &vq[nvq - 2], &vq[nvq - 1]); break; /* strip: keep the last two */
    case 5: if (GE_ACTIVE) ge_tri(&vq[0], &vq[1], &vq[2]); else draw_tri(&vq[0], &vq[1], &vq[2]); vq[1] = vq[2]; nvq = 2; break;   /* fan: keep the first */
    case 6: if (GE_ACTIVE) ge_sprite(&vq[0], &vq[1]); else draw_sprite(&vq[0], &vq[1]); nvq = 0; break;
    default: nvq = 0; break;
    }
}

/* ---------------------------------------------------------------- image transfers (host -> local) */
static uint32_t trx_x, trx_y, trx_left;
static int trx_active;

static void trx_start(void)
{
    uint64_t bb = reg[0x50], pos = reg[0x51], rg = reg[0x52];
    (void)bb;
    trx_x = 0;
    trx_y = 0;
    trx_left = (uint32_t)(rg & 0xFFF) * (uint32_t)((rg >> 32) & 0xFFF);
    trx_active = (reg[0x53] & 3) == 0 && trx_left > 0;
#ifdef HLE_PSP
    ge_upload_end();                                 /* an unfinished previous transfer: its pages get their ids */
    if ((reg[0x53] & 3) == 0 && trx_left > 0)        /* deferred data under this upload goes in first */
        ge_materialise_upload((uint32_t)(bb >> 32) & 0x3FFF, (uint32_t)(bb >> 48) & 63, (uint32_t)(bb >> 56) & 63,
                              (uint32_t)(pos >> 48) & 0x7FF, (uint32_t)(rg >> 32) & 0xFFF, (uint32_t)(pos >> 32) & 0x7FF,
                              (uint32_t)rg & 0xFFF);
    if (trx_active) ge_upload_begin((uint32_t)(bb >> 32) & 0x3FFF, (uint32_t)(bb >> 48) & 63, (uint32_t)(bb >> 56) & 63,
                                    (uint32_t)(pos >> 32) & 0x7FF, (uint32_t)(pos >> 48) & 0x7FF, (uint32_t)rg & 0xFFF,
                                    (uint32_t)(rg >> 32) & 0xFFF);
#endif
    (void)pos;
}

static void trx_pixel(uint32_t v)
{
#ifdef HLE_PSP
    gs_in_upload = 1;
#endif
    uint64_t bb = reg[0x50], pos = reg[0x51], rg = reg[0x52];
    uint32_t dbp = (uint32_t)(bb >> 32) & 0x3FFF, dbw = (uint32_t)(bb >> 48) & 63, dpsm = (uint32_t)(bb >> 56) & 63;
    uint32_t dx = (uint32_t)(pos >> 32) & 0x7FF, dy = (uint32_t)(pos >> 48) & 0x7FF, w = (uint32_t)rg & 0xFFF;
    pix_write(dbp, dbw, dpsm, dx + trx_x, dy + trx_y, v);
#ifdef HLE_PSP
    gs_in_upload = 0;
#endif
    if (++trx_x >= w) { trx_x = 0; trx_y++; }
    if (trx_left) trx_left--;
    if (!trx_left) {
        trx_active = 0;
#ifdef HLE_PSP
        ge_upload_end();
#endif
    }
}

static uint32_t up_hash_us, up_write_us;   /* upload cost split (bench): hashing, writing */
uint64_t gs_stat_uploads(void);
static void trx_data(const uint8_t *q, uint32_t nbytes)
{
    if (!trx_active) return;
    uint32_t dpsm = (uint32_t)(reg[0x50] >> 56) & 63;
    stat_uploads += nbytes;
#ifdef HLE_PSP
    SceUInt32 th0 = sceKernelGetSystemTimeLow();
    ge_upload_data(q, nbytes);
    SceUInt32 th1 = sceKernelGetSystemTimeLow();
    up_hash_us += th1 - th0;
    struct up_timer { SceUInt32 t; } ut = {th1};
#define UP_DONE() (up_write_us += sceKernelGetSystemTimeLow() - ut.t)
#else
#define UP_DONE() ((void)0)
#endif
    if (dpsm == PSMCT32 || dpsm == PSMT8 || dpsm == PSMT4) {          /* fast paths */
        uint64_t bb = reg[0x50], pos = reg[0x51], rg = reg[0x52];
        uint32_t dbp = (uint32_t)(bb >> 32) & 0x3FFF, dbw = (uint32_t)(bb >> 48) & 63;
        uint32_t dx = (uint32_t)(pos >> 32) & 0x7FF, dy = (uint32_t)(pos >> 48) & 0x7FF, w = (uint32_t)rg & 0xFFF;
        uint32_t n = dpsm == PSMCT32 ? nbytes / 4 : dpsm == PSMT8 ? nbytes : nbytes * 2;
        uint32_t i = 0;
        if (!tabs_ready) swz_tables();
        /* 32-bit and 8-bit: a stretch of one row inside one page at a time (page base once, then table offsets) */
        static int rowswz = -1;
        if (rowswz < 0) rowswz = dbg_opt("gs_rowswz", 1);
        if (rowswz && (dpsm == PSMCT32 || dpsm == PSMT8)) {
            int t8 = dpsm == PSMT8;
            uint32_t pwm = t8 ? 127 : 63, phm = t8 ? 63 : 31, pws = t8 ? 7 : 6, phs = t8 ? 6 : 5;
            uint32_t prow = t8 ? (dbw / 2 ? dbw / 2 : 1) : dbw;
            while (i < n && trx_left) {
                uint32_t x = dx + trx_x, y = dy + trx_y;
                uint32_t cnt = w - trx_x, pagel = pwm + 1 - (x & pwm);
                if (cnt > pagel) cnt = pagel;
                if (cnt > n - i) cnt = n - i;
                if (cnt > trx_left) cnt = trx_left;
                uint8_t *base = gm + ((dbp * 256 + ((x >> pws) + (y >> phs) * prow) * 8192) & (GSMEM - 1));
                if (t8) {
                    const uint16_t *o = tab8 + (y & phm) * 128 + (x & pwm);
                    for (uint32_t j = 0; j < cnt; j++) base[o[j]] = q[i + j];
                } else {
                    const uint16_t *o = tab32 + (y & phm) * 64 + (x & pwm);
                    for (uint32_t j = 0; j < cnt; j++) memcpy(base + o[j], q + 4 * (i + j), 4);
                }
                i += cnt;
                trx_left -= cnt;
                trx_x += cnt;
                if (trx_x >= w) { trx_x = 0; trx_y++; }
            }
        }
        for (; i < n && trx_left; i++) {
            uint32_t x = dx + trx_x, y = dy + trx_y;
            if (dpsm == PSMCT32) { uint32_t v; memcpy(&v, q + 4 * i, 4); wr32(addr32(dbp, dbw, x, y, bt32), v); }
            else if (dpsm == PSMT8) gm[addr8(dbp, dbw, x, y)] = q[i];
            else {
                int sh;
                uint32_t a = addr4(dbp, dbw, x, y, &sh), v = (q[i >> 1] >> ((i & 1) * 4)) & 15;
                gm[a] = (uint8_t)((gm[a] & ~(15 << sh)) | (v << sh));
            }
            if (++trx_x >= w) { trx_x = 0; trx_y++; }
            trx_left--;
        }
        if (!trx_left) {
            trx_active = 0;
#ifdef HLE_PSP
            ge_upload_end();
#endif
        }
        UP_DONE();
        return;
    }
    switch (dpsm) {
    case PSMCT32: case PSMZ32:
        for (uint32_t i = 0; i + 4 <= nbytes && trx_active; i += 4) { uint32_t v; memcpy(&v, q + i, 4); trx_pixel(v); }
        break;
    case PSMCT24: case PSMZ24: {                    /* 3 bytes a pixel: a pixel may straddle two pieces of data */
        static uint8_t carry[3];
        static uint32_t ncarry;
        uint32_t i = 0;
        if (trx_x == 0 && trx_y == 0 && trx_left == (uint32_t)(reg[0x52] & 0xFFF) * (uint32_t)((reg[0x52] >> 32) & 0xFFF))
            ncarry = 0;                             /* a new transfer */
        while (ncarry && ncarry < 3 && i < nbytes) carry[ncarry++] = q[i++];
        if (ncarry == 3) { trx_pixel(carry[0] | (carry[1] << 8) | (carry[2] << 16)); ncarry = 0; }
        for (; i + 3 <= nbytes && trx_active; i += 3) trx_pixel(q[i] | (q[i + 1] << 8) | (q[i + 2] << 16));
        while (i < nbytes && trx_active) carry[ncarry++] = q[i++];
        break;
    }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        for (uint32_t i = 0; i + 2 <= nbytes && trx_active; i += 2) { uint16_t v; memcpy(&v, q + i, 2); trx_pixel(v); }
        break;
    case PSMT8: case PSMT8H:
        for (uint32_t i = 0; i < nbytes && trx_active; i++) trx_pixel(q[i]);
        break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        for (uint32_t i = 0; i < nbytes && trx_active; i++) { trx_pixel(q[i] & 15); if (trx_active) trx_pixel(q[i] >> 4); }
        break;
    default: trx_active = 0; break;
    }
}

/* local -> local copy (TRXDIR 2) */
static void trx_local(void)
{
    uint64_t bb = reg[0x50], pos = reg[0x51], rg = reg[0x52];
    uint32_t sbp = (uint32_t)bb & 0x3FFF, sbw = (uint32_t)(bb >> 16) & 63, spsm = (uint32_t)(bb >> 24) & 63;
    uint32_t dbp = (uint32_t)(bb >> 32) & 0x3FFF, dbw = (uint32_t)(bb >> 48) & 63, dpsm = (uint32_t)(bb >> 56) & 63;
    uint32_t sx = (uint32_t)pos & 0x7FF, sy = (uint32_t)(pos >> 16) & 0x7FF, dx = (uint32_t)(pos >> 32) & 0x7FF, dy = (uint32_t)(pos >> 48) & 0x7FF;
    uint32_t w = (uint32_t)rg & 0xFFF, h = (uint32_t)(rg >> 32) & 0xFFF;
#ifdef HLE_PSP
    ge_materialise_upload(sbp, sbw, spsm, sy, h, 1, 0);   /* (a copy's source: always written first) */
    ge_materialise_upload(dbp, dbw, dpsm, dy, h, dx, w);
#endif
    ge_mark_upload(dbp, dbw, dpsm, dy, h);
    if (trace_on) { trace_flush(); LOG("gstrace: copy %llx pos %llx size %ux%u\n", (unsigned long long)bb, (unsigned long long)pos, w, h); }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            pix_write(dbp, dbw, dpsm, dx + x, dy + y, pix_read(sbp, sbw, spsm, sx + x, sy + y));
}

static void reg_write(uint32_t r, uint64_t v)
{
    if (r >= 0x64) return;
    st_regs[r]++;
    switch (r) {
    case 0x00: PRIM = v; nvq = 0; return;
    case 0x02: cur_st = v; reg[r] = v; return;
    case 0x04: reg[r] = v; vertex((int32_t)(v & 0xFFFF), (int32_t)((v >> 16) & 0xFFFF), (uint32_t)(v >> 32) & 0xFFFFFF, 1); return;
    case 0x05: reg[r] = v; vertex((int32_t)(v & 0xFFFF), (int32_t)((v >> 16) & 0xFFFF), (uint32_t)(v >> 32), 1); return;
    case 0x0C: reg[r] = v; vertex((int32_t)(v & 0xFFFF), (int32_t)((v >> 16) & 0xFFFF), (uint32_t)(v >> 32) & 0xFFFFFF, 0); return;
    case 0x0D: reg[r] = v; vertex((int32_t)(v & 0xFFFF), (int32_t)((v >> 16) & 0xFFFF), (uint32_t)(v >> 32), 0); return;
    case 0x3B: texa = v; reg[r] = v; return;
    case 0x06: case 0x07: reg[r] = v; clut_load(v); return;
    case 0x16: case 0x17: {                       /* TEX2: PSM and the CLUT fields of TEX0 */
        const uint64_t m = (63ull << 20) | (0x7FFFFFFull << 37);
        uint32_t t = r - 0x16 + 0x06;
        reg[t] = (reg[t] & ~m) | (v & m);
        reg[r] = v;
        clut_load(reg[t]);
        return;
    }
    case 0x53: reg[r] = v; if ((v & 3) == 0) trx_start(); else if ((v & 3) == 2) trx_local(); return;
    case 0x54: { uint8_t b[8]; memcpy(b, &v, 8); trx_data(b, 8); } return;   /* HWREG */
    default: reg[r] = v; return;
    }
}

/* ---------------------------------------------------------------- GIF packets */
static uint64_t gif_tag[2];

static uint32_t gif_loop, gif_nreg, gif_regi, gif_flg;
static int gif_in;                  /* inside a packet */

/* one qword of GIF data (PATH2 / PATH3) */
static void gif_qword(const uint8_t *q)
{
    if (!gif_in) {
        memcpy(gif_tag, q, 16);
        gif_loop = (uint32_t)gif_tag[0] & 0x7FFF;
        gif_flg = (uint32_t)(gif_tag[0] >> 58) & 3;
        gif_nreg = (uint32_t)(gif_tag[0] >> 60);
        if (!gif_nreg) gif_nreg = 16;
        gif_regi = 0;
        st_gif[gif_flg]++;
        if ((gif_tag[0] >> 46) & 1) reg_write(0x00, (gif_tag[0] >> 47) & 0x7FF);   /* PRE: PRIM */
        gif_in = gif_loop > 0;
        if (gif_flg == 2 && gif_in) {}
        return;
    }
    if (gif_flg == 0) {                                     /* PACKED */
        uint32_t rd = (uint32_t)(gif_tag[1] >> (4 * gif_regi)) & 15;
        uint64_t lo, hi;
        memcpy(&lo, q, 8);
        memcpy(&hi, q + 8, 8);
        uint32_t w0 = (uint32_t)lo, w1 = (uint32_t)(lo >> 32), w2 = (uint32_t)hi, w3 = (uint32_t)(hi >> 32);
        switch (rd) {
        case 0x0: reg_write(0x00, lo & 0x7FF); break;
        case 0x1: RGBAQ = (w0 & 0xFF) | ((uint64_t)(w1 & 0xFF) << 8) | ((uint64_t)(w2 & 0xFF) << 16) | ((uint64_t)(w3 & 0xFF) << 24)
                          | ((uint64_t)(uint32_t)0 << 32); { uint64_t qv; memcpy(&qv, &cur_q, 4); RGBAQ |= (qv & 0xFFFFFFFFu) << 32; } break;
        case 0x2: cur_st = lo; reg[0x02] = lo; memcpy(&cur_q, &w2, 4); break;
        case 0x3: reg[0x03] = (w0 & 0x3FFF) | ((uint64_t)(w1 & 0x3FFF) << 16); break;
        case 0x4: reg_write((hi >> 47) & 1 ? 0x0C : 0x04, (w0 & 0xFFFF) | ((uint64_t)(w1 & 0xFFFF) << 16) | ((uint64_t)((w2 >> 4) & 0xFFFFFF) << 32)); break;
        case 0x5: reg_write((hi >> 47) & 1 ? 0x0D : 0x05, (w0 & 0xFFFF) | ((uint64_t)(w1 & 0xFFFF) << 16) | ((uint64_t)w2 << 32)); break;
        case 0xA: reg[0x0A] = (uint64_t)((w3 >> 4) & 0xFF) << 56; break;
        case 0xE: reg_write((uint32_t)(hi & 0xFF), lo); break;
        case 0xF: break;
        default: reg_write(rd, lo); break;                  /* TEX0_1/2, CLAMP_1/2, XYZF3, XYZ3 ... */
        }
        if (++gif_regi >= gif_nreg) { gif_regi = 0; if (--gif_loop == 0) gif_in = 0; }
    } else if (gif_flg == 1) {                              /* REGLIST: two registers per qword */
        for (int h = 0; h < 2 && gif_in; h++) {
            uint64_t v;
            memcpy(&v, q + 8 * h, 8);
            uint32_t rd = (uint32_t)(gif_tag[1] >> (4 * gif_regi)) & 15;
            if (rd != 0xE && rd != 0xF) reg_write(rd == 1 ? 0x01 : rd, v);
            if (++gif_regi >= gif_nreg) { gif_regi = 0; if (--gif_loop == 0) gif_in = 0; }
        }
    } else {                                                /* IMAGE */
        trx_data(q, 16);
        if (--gif_loop == 0) gif_in = 0;
    }
}

/* where each qword of the VIF1 gather buffer came from in guest RAM (NULL: made here, e.g. a TTE tag): a DIRECT whose
   data is one contiguous run of guest RAM hands the GIF that pointer, so a whole-page upload can be deferred
   (gs_ge.h ge_upload_defer) instead of written */
#ifdef HLE_PSP
#define VSRC_QW (64u << 10) / 4
#else
#define VSRC_QW (64u << 10) / 4
#endif
static const uint8_t *vsrc[VSRC_QW];
static const uint8_t *direct_guest(uint32_t i, uint32_t k)
{
    uint32_t qi = i / 4;
    static int on = -1;
    if (on < 0) on = dbg_opt("gs_dguest", 1);
    if (!on || !k || qi + k > VSRC_QW) return NULL;
    const uint8_t *g = vsrc[qi];
    if (!g) return NULL;
    for (uint32_t j = 1; j < k; j++) if (vsrc[qi + j] != g + 16 * j) return NULL;
    return g;
}

/* nq contiguous qwords: image data goes to the transfer in one piece (the title uploads ~2.8 MB a frame; one call
   per qword decoded the transfer registers 16 bytes at a time) */
static void gif_block(const uint8_t *q, uint32_t nq, int guest)
{
#ifdef HLE_PSP
    SceUInt32 tg = sceKernelGetSystemTimeLow();
#endif
    while (nq) {
        if (gif_in && gif_flg == 2) {
            uint32_t k = nq < gif_loop ? nq : gif_loop;
#ifdef HLE_PSP
            if (trx_active && trx_x == 0 && trx_y == 0
                && (ge_upload_try_skip(q, 16 * k, trx_left) || (guest && ge_upload_defer(q, 16 * k, trx_left)))) {
                stat_uploads += 16 * k;                 /* the whole transfer was already in GS memory */
                trx_left = 0;
                trx_active = 0;
                gif_loop -= k;
                if (!gif_loop) gif_in = 0;
                q += 16 * k;
                nq -= k;
                continue;
            }
#endif
            trx_data(q, 16 * k);
            gif_loop -= k;
            if (!gif_loop) gif_in = 0;
            q += 16 * k;
            nq -= k;
        } else {
            gif_qword(q);
            q += 16;
            nq--;
        }
    }
#ifdef HLE_PSP
    t_gif_us += sceKernelGetSystemTimeLow() - tg;
#endif
}

/* ---------------------------------------------------------------- VIF1 */
/* VIF1 registers: write cycle, mask / row / col filling, double buffer (BASE, OFFSET -> TOPS, TOP, DBF), ITOP */
static uint32_t vif_cl = 1, vif_wl = 1, vif_mask, vif_row[4], vif_col[4], vif_mode;
static uint32_t vif_base, vif_ofst, vif_tops, vif_top, vif_dbf, vif_itops;
extern uint8_t vu1_mem[16384];
extern uint64_t vu1_stat_runs, vu1_stat_pairs, vu1_stat_kicks, vu1_stat_bad;
void vu1_mpg(uint32_t pair, const uint32_t *w, uint32_t npairs);
void vu1_mscal(uint32_t pair, uint32_t top, uint32_t itop);
void vu1_mscnt(uint32_t top, uint32_t itop);
static uint64_t st_unpack_qw;

/* PATH1: XGKICK of the GIF packets at VU1 quadword qaddr, up to the end of the packet with EOP */
void gs_path1_kick(const uint8_t *m, uint32_t qaddr)
{
    if (!gs_alloc()) return;
    gif_in = 0;
    for (int guard = 0; guard < 1024; guard++) {
        gif_qword(m + 16 * (qaddr++ & 0x3FF));
        if (!gif_in && ((gif_tag[0] >> 15) & 1)) break;
    }
    gif_in = 0;
}

static void vu_start(int cont, uint32_t pair)
{
    uint32_t top = vif_tops;
    vif_dbf ^= 1;
    vif_tops = vif_base + (vif_dbf ? vif_ofst : 0);
    vif_top = top;
    if (bench_ff) return;                       /* benchmark fast-forward: VU1 programs not run */
    if (!cont) vu_need_mscal = 0;
    else if (vu_need_mscal) return;                 /* after a fast-forward: no program state until an MSCAL */
#ifdef HLE_PSP
    {
        PZ_SCOPE(gs_nat);
        if (GE_ACTIVE && ge_vu1_native(cont, pair, top)) return;   /* programs 0 / 4 drawn by the GE (gs_ge_vu1.h) */
    }
#endif
    static int vu_all = -1;                     /* BT3_VU1_ALL=1: run VU1 every frame (its output only rasterised when drawn) */
    if (vu_all < 0) { const char *e = getenv("BT3_VU1_ALL"); vu_all = e && atoi(e); }
    if (!draw_on && !vu_all) return;            /* frames that are not dumped: the VU1 output is not needed */
#ifdef HLE_PSP
    SceUInt32 t0 = sceKernelGetSystemTimeLow();
    uint64_t pr0 = vu1_stat_pairs;
#endif
    {
#ifdef HLE_PSP
        PZ_SCOPE(gs_vu1i);
#endif
        if (cont) vu1_mscnt(top, vif_itops);
        else vu1_mscal(pair, top, vif_itops);
    }
#ifdef HLE_PSP
    vu_interp_us += sceKernelGetSystemTimeLow() - t0;
    vu_interp_runs++;
    vu_interp_pairs += vu1_stat_pairs - pr0;
    {                                          /* which programs are interpreted (by microcode hash), per bench */
        uint32_t h = vu1_micro_hash();
        int k = 0;
        while (k < 12 && vu_prog_h[k] && vu_prog_h[k] != h) k++;
        if (k < 12) { vu_prog_h[k] = h; vu_prog_us[k] += sceKernelGetSystemTimeLow() - t0; vu_prog_runs[k]++; }
    }
#endif
}

static uint32_t sext(uint32_t v, unsigned bits, int usn)
{
    if (usn || bits == 32) return v;
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

/* UNPACK: writes num vectors to VU1 memory; returns the data words it consumed */
static uint32_t vif_unpack(uint32_t cmd, uint32_t num, uint32_t imm, const uint32_t *w, uint32_t avail)
{
#ifdef HLE_PSP
    PZ_SCOPE(gs_unpack);
#endif
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, msk = (cmd >> 4) & 1;
    uint32_t addr = imm & 0x3FF, usn = (imm >> 14) & 1;
    if (imm & 0x8000) addr += vif_tops;
    if (!num) num = 256;
    uint32_t cl = vif_cl, wl = vif_wl ? vif_wl : 1;
    /* V4-32 without mask or row mode, every cycle written (cl = wl): the data are the quadwords, copied as they are
       (the battle unpacks ~1900 batches of vertices a frame this way) */
    static int v4fast = -1;
    if (v4fast < 0) v4fast = dbg_opt("gs_v4fast", 1);
    if (v4fast && vn == 3 && vl == 0 && !msk && vif_mode == 0 && cl == wl) {
        uint32_t nq = num < avail / 4 ? num : avail / 4;
        for (uint32_t k = 0; k < nq; ) {
            uint32_t d = (addr + k) & 0x3FF, m = nq - k;
            if (m > 1024 - d) m = 1024 - d;
            memcpy(vu1_mem + 16 * d, w + 4 * k, 16 * m);
            k += m;
        }
        st_unpack_qw += nq;
        return nq * 4;
    }
    unsigned ebits = vl == 3 ? 16 : 32u >> vl, ncomp = vl == 3 ? 1 : vn + 1;
    const uint8_t *src = (const uint8_t *)w;
    uint32_t bytes_avail = avail * 4, off = 0;
    for (uint32_t k = 0; k < num; k++) {
        uint32_t r = k % wl, dst;
        int data = 1;
        if (wl <= cl) dst = addr + (k / wl) * cl + r;
        else { dst = addr + k; data = r < cl; }
        uint32_t el[4] = {0, 0, 0, 0};
        if (data) {
            uint32_t vbytes = ncomp * ebits / 8;
            if (off + vbytes > bytes_avail) break;
            for (unsigned c = 0; c < ncomp; c++) {
                uint32_t v;
                if (ebits == 32) memcpy(&v, src + off + 4 * c, 4);
                else if (ebits == 16) { uint16_t h; memcpy(&h, src + off + 2 * c, 2); v = h; }
                else v = src[off + c];
                el[c] = v;
            }
            off += vbytes;
            if (vl == 3) {                                       /* V4-5: RGBA 5551 -> 4 x 8 bits */
                uint32_t h = el[0];
                el[0] = (h & 31) << 3; el[1] = ((h >> 5) & 31) << 3; el[2] = ((h >> 10) & 31) << 3; el[3] = (h >> 15) << 7;
            } else {
                for (unsigned c = 0; c < ncomp; c++) el[c] = sext(el[c], ebits, usn);
                if (vn == 0) el[1] = el[2] = el[3] = el[0];      /* S: one value to every lane */
            }
        }
        uint8_t *q = vu1_mem + 16 * (dst & 0x3FF);
        unsigned row = r > 3 ? 3 : r;
        for (unsigned c = 0; c < 4; c++) {
            unsigned m = msk ? (vif_mask >> (2 * (row * 4 + c))) & 3 : 0;
            uint32_t v;
            if (m == 3) continue;
            if (m == 1) v = vif_row[c];
            else if (m == 2) v = vif_col[row];
            else if (!data) v = vif_row[c];
            else {
                if (vl != 3 && vn != 0 && c > vn) continue;           /* V2 / V3: the missing lanes are left alone */
                v = el[c];
                if (vif_mode == 1) v += vif_row[c];
                else if (vif_mode == 2) { vif_row[c] += v; v = vif_row[c]; }
            }
            memcpy(q + 4 * c, &v, 4);
        }
        st_unpack_qw++;
    }
    return (off + 3) / 4;
}

/* the VIF1 stream of one DMA transfer: words[i0..nw). Not final (the gather buffer is full, more follows): stops
   before a command whose data is not all there and returns its index (a DIRECT's qwords are passed on as they come:
   vif_direct_left carries the rest of its count); final: everything is consumed */
static uint32_t vif_direct_left;
static uint32_t vif_stream(const uint32_t *w, uint32_t i0, uint32_t nw, int final)
{
    uint32_t i = i0;
    if (vif_direct_left) {                                                              /* a DIRECT goes on */
        {
            uint32_t k = (nw - i) / 4;
            if (k > vif_direct_left) k = vif_direct_left;
            const uint8_t *g = direct_guest(i, k);
            gif_block(g ? g : (const uint8_t *)(w + i), k, g != NULL);
            i += 4 * k;
            vif_direct_left -= k;
        }
        if (vif_direct_left && !final) return i;
        vif_direct_left = 0;
    }
    while (i < nw) {
        uint32_t at = i;
        uint32_t code = w[i++];
        if (!final) {                                   /* is the command's data complete? */
            uint32_t c7 = (code >> 24) & 0x7F, nm = (code >> 16) & 0xFF, need = 0;
            if (c7 == 0x20) need = 1;
            else if (c7 == 0x30 || c7 == 0x31) need = 4;
            else if (c7 == 0x4A) need = ((i + 1) & ~1u) - i + 2 * (nm ? nm : 256);
            else if (c7 >= 0x60) {
                uint32_t vn = (c7 >> 2) & 3, vl = c7 & 3, cnt = nm ? nm : 256, wl = vif_wl ? vif_wl : 1;
                uint32_t nd = wl <= vif_cl ? cnt : cnt - (cnt / wl) * (wl - vif_cl) - (cnt % wl > vif_cl ? cnt % wl - vif_cl : 0);
                uint32_t bits = vl == 3 ? 16 : (32u >> vl) * (vn + 1);
                need = (nd * bits + 31) / 32;
            }
            if (i + need > nw) return at;
        }
        uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
        st_vif[cmd]++;
        switch (cmd) {
        case 0x01: vif_cl = imm & 0xFF; vif_wl = (imm >> 8) & 0xFF; continue;          /* STCYCL */
        case 0x02: vif_ofst = imm & 0x3FF; vif_dbf = 0; vif_tops = vif_base; continue;  /* OFFSET */
        case 0x03: vif_base = imm & 0x3FF; continue;                                    /* BASE */
        case 0x04: vif_itops = imm & 0x3FF; continue;                                   /* ITOP */
        case 0x05: vif_mode = imm & 3; continue;                                        /* STMOD */
        case 0x14: case 0x15: vu_start(0, imm); continue;                               /* MSCAL / MSCALF */
        case 0x17: vu_start(1, 0); continue;                                            /* MSCNT */
        case 0x20: if (i < nw) vif_mask = w[i]; i += 1; continue;                       /* STMASK */
        case 0x30: case 0x31:                                                           /* STROW / STCOL */
            if (i + 4 <= nw) memcpy(cmd == 0x30 ? vif_row : vif_col, w + i, 16);
            i += 4;
            continue;
        case 0x4A: {                                                                    /* MPG */
            uint32_t np = num ? num : 256;
            i = (i + 1) & ~1u;
            if (i + 2 * np <= nw) vu1_mpg(imm, w + i, np);
            i += 2 * np;
            continue;
        }
        case 0x50: case 0x51: {                                                         /* DIRECT / DIRECTHL */
            i = (i + 3) & ~3u;
            uint32_t nq = imm ? imm : 65536, k = (nw - i) / 4;
            if (k > nq) k = nq;
            const uint8_t *g = direct_guest(i, k);
            gif_block(g ? g : (const uint8_t *)(w + i), k, g != NULL);
            i += 4 * k;
            if (k < nq && !final) { vif_direct_left = nq - k; return i; }
            continue;
        }
        default:
            if (cmd >= 0x60) {
                i += vif_unpack(cmd, num, imm, w + i, nw - i);
                continue;
            }
            continue;                                   /* NOP, MSKPATH3, MARK, FLUSH*: no effect here */
        }
    }
    return nw;
}

/* ---------------------------------------------------------------- DMA */
static uint64_t stat_dma_qw;

/* The VU1 microprograms are VIF packets inside the program's code region (.vutext, 0x2BF6B0..0x2C33C0). On the PSP
   that region is not loaded (the translated code replaces it) and the runtime keeps its own buffers there, so a DMA
   of a program (Vu1Pkt_LoadProgN: a "ref" into .vutext) read those buffers: the VU1 got garbage microcode (program 4
   hung the battle load). Those addresses are served from a copy read once from the game's ELF. */
#define VUTEXT_LO 0x002BF6B0u
#define VUTEXT_HI 0x002C33C0u
#ifdef HLE_PSP
static uint8_t vutext[VUTEXT_HI - VUTEXT_LO] __attribute__((aligned(16)));
static int vutext_state;                     /* 0 = not read yet, 1 = read, -1 = failed */
static void vutext_load(void)
{
    char path[320];
    snprintf(path, sizeof path, "%sSLUS_216.78", hle_plat_disc_root());
    FILE *f = fopen(path, "rb");
    vutext_state = -1;
    if (!f) { LOG("gs_soft: cannot open %s for the VU1 programs\n", path); return; }
    if (fseek(f, (long)(0x1000u + VUTEXT_LO - 0x100000u), SEEK_SET) == 0
        && fread(vutext, 1, sizeof vutext, f) == sizeof vutext) vutext_state = 1;
    fclose(f);
    LOG("gs_soft: VU1 programs (.vutext %08x..%08x) %s from %s\n", VUTEXT_LO, VUTEXT_HI,
        vutext_state > 0 ? "read" : "NOT read", path);
}
static uint8_t *gdma_qw(uint32_t a)
{
    uint32_t r = a & 0x0FFFFFF0u;
    if (r >= VUTEXT_LO && r < VUTEXT_HI && !(a & 0x80000000u)) {
        if (!vutext_state) vutext_load();
        if (vutext_state > 0) return vutext + (r - VUTEXT_LO);
    }
    return hle_dma_qw(a);
}
#else
#define gdma_qw hle_dma_qw
#endif

static uint32_t vbuf_start;          /* where vif_stream goes on in the gather buffer */
static uint32_t st_vbuf_flushes;

/* one qword into the VIF1 gather buffer; a full buffer is streamed first, its unfinished command kept */
static void vbuf_flush(uint32_t *vbuf, uint32_t *vn, uint32_t vmax)
{
    {
        uint32_t c = vif_stream(vbuf, vbuf_start, *vn, 0);
        uint32_t keep = *vn - c, base = c & 3;                     /* keep the word alignment (DIRECT, MPG) */
        if (keep + base + 4 > vmax) { LOG("gs_soft: VIF1 command larger than the buffer - dropped\n"); keep = 0; }
        memmove(vbuf + base, vbuf + c, keep * 4);
        memmove(vsrc, vsrc + (c - base) / 4, ((base + keep + 3) / 4) * sizeof vsrc[0]);
        vbuf_start = base;
        *vn = base + keep;
        st_vbuf_flushes++;
    }
}

static void vbuf_push(uint32_t *vbuf, uint32_t *vn, uint32_t vmax, const void *q, const uint8_t *src)
{
    if (*vn + 4 > vmax) vbuf_flush(vbuf, vn, vmax);
    memcpy(vbuf + *vn, q, 16);
    if (*vn / 4 < VSRC_QW) vsrc[*vn / 4] = src;
    *vn += 4;
}

/* qwords at addr that are plain guest RAM, contiguous (*p = the first): 0 = not RAM (scratchpad, registers, the
   .vutext copy, past the RAM end) - those take the per-qword path */
static uint32_t ram_run(uint32_t addr, uint8_t **p)
{
    *p = NULL;
    static int on = -1;
    if (on < 0) on = dbg_opt("gs_ramrun", 1);
    if (!on) return 0;
    if ((addr & 0x80000000u) || (addr & 0x70000000u) == 0x70000000u) return 0;
    uint32_t a = addr & 0x1FFFFFF0u;
    if (a >= hle_ram_end) return 0;
    if (a < VUTEXT_HI && a + 16 > VUTEXT_LO) return 0;
    uint32_t lim = a < VUTEXT_LO ? VUTEXT_LO : hle_ram_end;
    *p = hle_ram + a;
    return (lim - a) >> 4;
}

static void dma_data(int ch, uint32_t addr, uint32_t n, uint32_t *vbuf, uint32_t *vn, uint32_t vmax)
{
    /* contiguous runs of guest RAM in one piece: the GIF gets them directly, the VIF1 buffer by memcpy (the title
       moves ~200 MB in 10 s through here: one address translation and one 16-byte copy per qword was most of it) */
    uint32_t k = 0;
    while (k < n) {
        uint8_t *p;
        uint32_t run = ram_run(addr + 16 * k, &p);
        if (!run) {
            uint8_t *q = gdma_qw(addr + 16 * k);
            k++;
            if (!q) continue;
            stat_dma_qw++;
            if (ch == 2) gif_qword(q);
            else vbuf_push(vbuf, vn, vmax, q, q);
            continue;
        }
        if (run > n - k) run = n - k;
        stat_dma_qw += run;
        k += run;
        if (ch == 2) { gif_block(p, run, 1); continue; }
        while (run) {
            if (*vn + 4 > vmax) vbuf_flush(vbuf, vn, vmax);
            uint32_t m = (vmax - *vn) / 4;
            if (m > run) m = run;
            memcpy(vbuf + *vn, p, 16 * m);
            for (uint32_t j = 0; j < m; j++) if ((*vn / 4) + j < VSRC_QW) vsrc[*vn / 4 + j] = p + 16 * j;
            *vn += 4 * m;
            p += 16 * m;
            run -= m;
        }
    }
    return;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t *q = gdma_qw(addr + 16 * k);
        if (!q) continue;
        stat_dma_qw++;
        if (ch == 2) gif_qword(q);
        else vbuf_push(vbuf, vn, vmax, q, q);
    }
}

static unsigned vblanks, dumps;
static uint32_t st_vbuf_max;         /* largest VIF1 chain seen (words) */

/* GS local memory (4 MB) and the VIF1 gather buffer; on the PSP the memory may come from the volatile partition */
#ifdef HLE_PSP
#include "hle_mem_psp.h"
#define VBUF_WORDS (64u << 10)       /* 256 KB: streamed when full (vbuf_push) */
#else
#define VBUF_WORDS (64u << 10)       /* 256 KB: smaller than the PSP's, so the host exercises the flush path more */
#endif
static int gs_alloc(void)
{
    static int failed;
    if (gm) return 1;
    if (failed) return 0;
#ifdef HLE_PSP
    gm = hle_mem_vol_take(GSMEM, 0);                 /* ps2rc.cfg mem_volatile=1: the volatile partition first */
    if (!gm) gm = calloc(1, GSMEM);
#else
    gm = calloc(1, GSMEM);
#endif
    if (!gm) { failed = 1; LOG("gs_soft: no memory for the GS (4 MB): nothing is drawn\n"); return 0; }
    LOG("gs_soft: GS memory at %p\n", (void *)gm);
    return 1;
}
static unsigned dbg_dma_at;          /* BT3_DMA_DUMP=<vblank>: log every VIF1 tag of the transfers of that vblank */
static int dbg_dma;

/* ---------------------------------------------------------------- benchmark points (PSP)
   ps2rc.cfg bench_at=<game seconds> [bench_secs=10]: until that moment the game runs fast-forward (no graphics: the
   GS / VIF data is not processed, nothing is drawn, the pacing against the real clock is suspended so idle time is
   skipped); from it the game runs normally for bench_secs REAL seconds, then a "BENCH" line gives the game time
   advanced and the cost split, and the run ends. A repeatable start point for speed tests (a PPSSPP save state would
   also restore the old program code, so it cannot test a new build). Points used: 36 = title animation start,
   75 = demo battle start. */
static double bench_at = -1, bench_secs = 10;
static int bench_on;
static int64_t bench_t0_us;
static double bench_v0;
static uint64_t bench_flips0;
#ifdef HLE_PSP
extern int hle_plat_pace_suspend;
#endif

void hle_pack_gfx_dma(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    /* fast-forward to a benchmark point: the data are processed (uploads made once, e.g. a battle's models, must
       be in GS memory afterwards), only drawing and the VU1 programs are skipped */
    static int init;
    if (!init) { const char *e = getenv("BT3_DMA_DUMP"); dbg_dma_at = e ? (unsigned)atoi(e) : 0; init = 1; }
    if (!gs_alloc()) return;
    static uint32_t *vbuf;
    if (!vbuf) {
        static int vfail;
        if (vfail) return;
#ifdef HLE_PSP
        extern void *ge_kalloc(const char *name, uint32_t bytes);
        vbuf = ge_kalloc("vif1_buf", VBUF_WORDS * 4);   /* (not the guest code region: the game writes it) */
#endif
        if (!vbuf) vbuf = malloc(VBUF_WORDS * 4);
        if (!vbuf) { vfail = 1; LOG("gs_soft: no memory for the VIF1 buffer (%u KB)\n", VBUF_WORDS / 256); return; }
    }
    const uint32_t vmax = VBUF_WORDS;
#ifdef HLE_PSP
    if (ge_on < 0) ge_on = ge_init();                /* after the VIF1 buffer: both come from the code hole */
    SceUInt32 dma_t0 = sceKernelGetSystemTimeLow();
#endif
    uint32_t vn = 0;
    st_ch[ch]++;
    dbg_dma = dbg_dma_at && vblanks == dbg_dma_at;
    if (dbg_dma) LOG("dmadump: kick ch %d chcr %08x madr %08x qwc %u tadr %08x\n", ch, chcr, madr, qwc, tadr);
    int mode = (int)((chcr >> 2) & 3);
    if (mode == 0) {
        dma_data(ch, madr, qwc, vbuf, &vn, vmax);
    } else {
        int guard = 0, end = 0, asp = 0;
        uint32_t ret[2];
        if (qwc) dma_data(ch, madr, qwc, vbuf, &vn, vmax);    /* a chain restarts with the rest of the last tag */
        while (!end && guard++ < 200000) {
            uint8_t *tq = gdma_qw(tadr);
            if (!tq) break;
            uint64_t tag;
            memcpy(&tag, tq, 8);
            uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32) & 0x7FFFFFF0u;
            if ((tag >> 63) & 1) addr |= 0x80000000u;          /* SPR */
            uint32_t data, cur = tadr;
            switch (id) {
            case 0: data = addr; end = 1; break;
            case 1: data = tadr + 16; tadr = data + 16 * n; break;
            case 2: data = tadr + 16; tadr = addr; break;
            case 3: case 4: data = addr; tadr += 16; break;
            case 5: data = tadr + 16; if (asp < 2) ret[asp++] = data + 16 * n; tadr = addr; break;
            case 6: data = tadr + 16; if (asp) tadr = ret[--asp]; else end = 1; break;
            default: data = tadr + 16; end = 1; break;
            }
            if (ch == 1 && (chcr & 0x40)) {                     /* TTE: the tag's upper half is VIF codes (words 2-3) */
                uint32_t tw[4] = {0, 0, 0, 0};                  /* the lower half reaches the VIF as two NOPs: keeps */
                memcpy(tw + 2, tq + 8, 8);                      /* the qword alignment DIRECT data depends on */
                vbuf_push(vbuf, &vn, vmax, tw, NULL);
            }
            if (dbg_dma && ch == 1 && guard < 400)
                LOG("dmadump: tag @%08x id %u qwc %u addr %08x vif %08x %08x%s\n", cur, id, n, addr,
                    (uint32_t)(tag >> 0) == 0 ? 0 : ((const uint32_t *)tq)[2], ((const uint32_t *)tq)[3],
                    (((tag >> 31) & 1) && (chcr & 0x80)) ? " IRQ-stop" : "");
            dma_data(ch, data, n, vbuf, &vn, vmax);
            if (((tag >> 31) & 1) && (chcr & 0x80)) end = 1;
        }
    }
#ifdef HLE_PSP
    SceUInt32 tv = sceKernelGetSystemTimeLow();
#endif
    if (ch == 1 && vn > vbuf_start) vif_stream(vbuf, vbuf_start, vn, 1);
#ifdef HLE_PSP
    t_vifstream_us += sceKernelGetSystemTimeLow() - tv;
#endif
    vbuf_start = 0;
    vif_direct_left = 0;
#ifdef HLE_PSP
    ge_dma_us += sceKernelGetSystemTimeLow() - dma_t0;
#endif
    if (vn > st_vbuf_max) st_vbuf_max = vn;
}

/* ---------------------------------------------------------------- frame dumps (BMP) */

/* the 512 x 448 draw buffer (fbp 0x70) as colour, beside its alpha byte as grey (trace snapshots) */
static void snap_buffer(unsigned k, uint32_t fbp)
{
    const char *dir = getenv("BT3_FRAMES");
    if (!dir) dir = "/mnt/c/bt3ps2rc/work/runs/frames";
    char path[512];
    snprintf(path, sizeof path, "%s/snap_%04u.bmp", dir, k);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t w = 1024, h = 448, img = w * 3 * h;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t v;
    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    for (int32_t y = 447; y >= 0; y--) {
        for (uint32_t x = 0; x < 1024; x++) {
            uint32_t c = pix_read(fbp * 32, 8, PSMCT32, x & 511, (uint32_t)y);
            uint8_t px[3];
            if (x < 512) { px[0] = (uint8_t)(c >> 16); px[1] = (uint8_t)(c >> 8); px[2] = (uint8_t)c; }
            else px[0] = px[1] = px[2] = (uint8_t)(c >> 24);
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    LOG("gstrace: snapshot %u after the batch above\n", k);
}

#ifdef HLE_PSP
/* a 480 x 272 picture (stride 512) as a BMP next to the EBOOT */
static void psp_write_bmp(const char *name, const uint32_t *src)
{
    char sp[320];
    snprintf(sp, sizeof sp, "%s%s", hle_plat_disc_root(), name);
    FILE *sf = fopen(sp, "wb");
    if (!sf) return;
    uint32_t bw = 480, bh = 272, img = bw * bh * 3, v;
    uint8_t hdr[54] = {'B', 'M'};
    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &bw, 4);
    memcpy(hdr + 22, &bh, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, sf);
    static uint8_t row[480 * 3];
    for (int32_t y = 271; y >= 0; y--) {
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t c = src[y * 512 + x];
            row[3 * x] = (uint8_t)(c >> 16); row[3 * x + 1] = (uint8_t)(c >> 8); row[3 * x + 2] = (uint8_t)c;
        }
        fwrite(row, 1, sizeof row, sf);
    }
    fclose(sf);
    LOG("gs_soft: screenshot %s\n", sp);
}
#endif

/* the display registers, one line (burst captures) */
static void log_display(const char *what, unsigned k)
{
    LOG("burst %s %u: vblank %u vtime %.3f PMODE %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx SMODE2 %llx\n",
        what, k, vblanks, (double)hle_vtime / EE_HZ, (unsigned long long)hle_gs_priv(0x00),
        (unsigned long long)hle_gs_priv(0x70), (unsigned long long)hle_gs_priv(0x80), (unsigned long long)hle_gs_priv(0x90),
        (unsigned long long)hle_gs_priv(0xA0), (unsigned long long)hle_gs_priv(0x20));
}

static void dump_frame(void)
{
    if (!gm) return;
    uint64_t pm = hle_gs_priv(0x0000);                       /* PMODE: EN1 bit 0, EN2 bit 1 */
    uint32_t off = (pm & 2) ? 0x90 : 0x70;                   /* DISPFB2 / DISPFB1 */
    uint64_t fb = hle_gs_priv(off), disp = hle_gs_priv(off + 0x10);
    uint32_t fbp = (uint32_t)(fb & 0x1FF) * 32, fbw = (uint32_t)(fb >> 9) & 63, psm = (uint32_t)(fb >> 15) & 31;
    uint32_t dbx = (uint32_t)(fb >> 32) & 0x7FF, dby = (uint32_t)(fb >> 43) & 0x7FF;
    uint32_t magh = (uint32_t)(disp >> 23) & 15;
    uint32_t w = ((uint32_t)(disp >> 32) & 0xFFF) + 1, h = ((uint32_t)(disp >> 44) & 0x7FF) + 1;
    w /= (magh + 1);
    if (!fbw || w > 2048 || h > 2048 || w < 16 || h < 16) { w = 640; h = 448; if (!fbw) fbw = 10; }
    uint32_t fpsm = psm == 2 ? PSMCT16 : psm == 10 ? PSMCT16S : PSMCT32;
#ifdef HLE_PSP
    /* the displayed GS buffer, scaled (nearest) to 480 x 272, into a VRAM buffer after the debug console's */
    uint32_t *out = (uint32_t *)(((uintptr_t)sceGeEdramGetAddr() + 0x88000u) | 0x40000000u);
    if (GE_ACTIVE) out = NULL;                        /* the GE shows its own target (copied below for a shot) */
    else
    for (uint32_t y = 0; y < 272; y++) {
        uint32_t sy = dby + y * h / 272;
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t c = pix_read(fbp, fbw, fpsm, dbx + x * w / 480, sy);
            if (fpsm != PSMCT32) c = c16to32(c, 0);
            out[y * 512 + x] = c | 0xFF000000u;
        }
    }
    if (!GE_ACTIVE)
        sceDisplaySetFrameBuf((void *)((uintptr_t)out & ~0x40000000u), 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
    dumps++;
    const char *path = "screen";
    /* ps2rc.cfg gs_shot=N: every N-th picture also as <EBOOT dir>/shot_NNNN.bmp (480 x 272) */
    static int shot = -1;
    if (shot < 0) { const char *e = hle_cfg_get("gs_shot"); shot = e ? atoi(e) : 0; }
    if (shot > 0 && dumps % (unsigned)shot == 0 && GE_ACTIVE) out = ge_shown_pixels();
    if (shot > 0 && dumps % (unsigned)shot == 0 && out) {
        char nm[64];
        snprintf(nm, sizeof nm, "shot_%04u.bmp", dumps / (unsigned)shot);
        psp_write_bmp(nm, out);
    }
#else
    const char *dir = getenv("BT3_FRAMES");
    if (!dir) dir = "/mnt/c/bt3ps2rc/work/runs/frames";
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05u.bmp", dir, dumps++);
    FILE *f = fopen(path, "wb");
    if (!f) { LOG("gs_soft: cannot write %s\n", path); return; }
    uint32_t rowb = w * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * h;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t v;
    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = pix_read(fbp, fbw, fpsm, dbx + x, dby + (uint32_t)y);
            if (fpsm != PSMCT32) c = c16to32(c, 0);
            uint8_t px[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(px, 1, 3, f);
        }
        static const uint8_t z[3] = {0, 0, 0};
        fwrite(z, 1, pad, f);
    }
    fclose(f);
#endif
    if (!HLE_IS_PSP || dumps % 10 == 1) LOG("gs_soft: frame %u -> %s (%ux%u fbp %u fbw %u psm %u; pixels %llu prims %llu uploads %llu bytes, dma %llu qw)\n",
        vblanks, path, w, h, fbp, fbw, psm, (unsigned long long)stat_pixels, (unsigned long long)stat_prims,
        (unsigned long long)stat_uploads, (unsigned long long)stat_dma_qw);
    if (dumps % 10 == 1) {
        LOG("gs_soft: kicks vif1 %llu gif %llu; giftags packed %llu reglist %llu image %llu\n", (unsigned long long)st_ch[1],
            (unsigned long long)st_ch[2], (unsigned long long)st_gif[0], (unsigned long long)st_gif[1], (unsigned long long)st_gif[2]);
        char b[2048]; int n = 0;
        for (int k = 0; k < 128; k++) if (st_vif[k]) n += snprintf(b + n, sizeof b - n, " %02x:%llu", k, (unsigned long long)st_vif[k]);
        LOG("gs_soft: vif cmds%s\n", n ? b : " none");
        LOG("gs_soft: vu1 runs %llu pairs %llu xgkicks %llu unknown-insns %llu unpacked %llu qw\n",
            (unsigned long long)vu1_stat_runs, (unsigned long long)vu1_stat_pairs, (unsigned long long)vu1_stat_kicks,
            (unsigned long long)vu1_stat_bad, (unsigned long long)st_unpack_qw);
        n = 0;
        for (int k = 0; k < 0x64; k++) if (st_regs[k]) n += snprintf(b + n, sizeof b - n, " %02x:%llu", k, (unsigned long long)st_regs[k]);
        LOG("gs_soft: gs regs%s\n", n ? b : " none");
    }
}

/* boot init, before the core's MMIO buffers: on the PSP the 4 MB GS memory takes the volatile partition whole */
void hle_pack_mem_early(void) { gs_alloc(); }

void hle_pack_vblank(void)
{
    static unsigned every;
#ifdef HLE_PSP
    if (!every) { const char *e = hle_cfg_get("gs_every"); every = e ? (unsigned)atoi(e) : 30; if (!every) every = 30; }
#else
    if (!every) { const char *e = getenv("BT3_FRAME_EVERY"); every = e ? (unsigned)atoi(e) : 60; if (!every) every = 60; }
#endif
    static int all = -1;
    if (all < 0) { const char *e = getenv("BT3_DRAW_ALL"); all = e && atoi(e); }
    static int trace_at = -2;
    if (trace_at == -2) { const char *e = getenv("BT3_GS_TRACE"); trace_at = e ? atoi(e) : -1; }
#ifdef HLE_PSP
    if (bench_at < -0.5) {
        const char *e = hle_cfg_get("bench_at"), *n = hle_cfg_get("bench_secs");
        bench_at = e ? atof(e) : 0;
        if (n && atof(n) > 0) bench_secs = atof(n);
        if (bench_at > 0) { bench_ff = 1; hle_plat_pace_suspend = 1; LOG("BENCH: fast-forward to %.1f s\n", bench_at); }
    }
    ge_materialise_all();                            /* deferred uploads do not outlive the frame */
    if (bench_ff) {
        if ((double)hle_vtime / EE_HZ < bench_at) { vblanks++; return; }   /* no flips, no dumps */
        bench_ff = 0;
        hle_plat_pace_suspend = 0;
        vu_need_mscal = 1;
        bench_on = 1;
        bench_t0_us = (int64_t)sceKernelGetSystemTimeWide();
        bench_v0 = (double)hle_vtime / EE_HZ;
        bench_flips0 = ge_st_flips_get();
        stat_uploads = 0; up_hash_us = up_write_us = 0;
        ge_nat_us = ge_prim_us = ge_texlook_us = 0;
        vu_interp_us = 0; vu_interp_runs = vu_interp_pairs = 0;
        t_unpack_us = t_gif_us = t_vifstream_us = 0;
        memset(vu_prog_h, 0, sizeof vu_prog_h); memset(vu_prog_us, 0, sizeof vu_prog_us);
        memset(vu_prog_runs, 0, sizeof vu_prog_runs);
        LOG("BENCH: start at game %.2f s\n", bench_v0);
    } else if (bench_on) {
        double real = (double)((int64_t)sceKernelGetSystemTimeWide() - bench_t0_us) / 1e6;
        if (real >= bench_secs) {
            double gv = (double)hle_vtime / EE_HZ - bench_v0;
            LOG("BENCH: %.2f game s in %.2f real s = %.3fx real time, %llu frames shown (%.1f per real s)\n", gv, real,
                gv / real, (unsigned long long)(ge_st_flips_get() - bench_flips0),
                (double)(ge_st_flips_get() - bench_flips0) / real);
            ge_bench_report();
            LOG("BENCH: VIF unpack %u ms, GIF (incl. uploads) %u ms, final VIF stream (incl. VU1 + unpack + GIF) %u ms\n",
                t_unpack_us / 1000, t_gif_us / 1000, t_vifstream_us / 1000);
            LOG("BENCH: VU1 interpreter %u ms, %llu runs, %llu pairs\n", vu_interp_us / 1000,
                (unsigned long long)vu_interp_runs, (unsigned long long)vu_interp_pairs);
            for (int k = 0; k < 12 && vu_prog_h[k]; k++)
                LOG("BENCH:   microcode %08x: %u ms, %u runs\n", vu_prog_h[k], vu_prog_us[k] / 1000, vu_prog_runs[k]);
            hle_set_exit_reason("bench done");
            hle_exit(0);
        }
    }
    ge_vblank();                                     /* the GE: finish the frame, show the displayed target */
#endif
    {                                                /* gs_dump_at=V / BT3_GSDUMP=V: GS memory at vblank V to gsmem.bin */
        static int at = -2;
        if (at == -2) {
#ifdef HLE_PSP
            const char *e = hle_cfg_get("gs_dump_at");
#else
            const char *e = getenv("BT3_GSDUMP");
#endif
            at = e ? atoi(e) : -1;
        }
        if (at >= 0 && vblanks == (unsigned)at && gm) {
#ifdef HLE_PSP
            ge_materialise_all();
            FILE *f = fopen("gsmem.bin", "wb");
#else
            FILE *f = fopen("/mnt/c/bt3ps2rc/work/runs/gsmem_host.bin", "wb");
#endif
            if (f) { fwrite(gm, 1, GSMEM, f); fclose(f); LOG("gsdump: GS memory at vblank %u written%c", vblanks, 10); }
        }
    }
    /* bursts: every N seconds (ps2rc.cfg gs_burst=N / BT3_BURST=N) the pictures of 3 consecutive vblanks */
    static int burst = -1;
    static unsigned nburst, burst_n = 3, burst_at;
    if (burst < 0) {
#ifdef HLE_PSP
        const char *e = hle_cfg_get("gs_burst");
#else
        const char *e = getenv("BT3_BURST");
#endif
        burst = e ? atoi(e) * 60 : 0;
#ifdef HLE_PSP
        const char *f = hle_cfg_get("gs_burstf");      /* a period in vblanks instead, one picture each */
        if (f) { burst = atoi(f); burst_n = 1; }
        const char *a = hle_cfg_get("gs_burst_at");    /* the first vblank captured */
        if (a) burst_at = (unsigned)atoi(a);
#endif
    }
    if (burst > 0) {
        unsigned k = (vblanks + 1) % (unsigned)burst;
        if (k < burst_n && vblanks + 1 >= (unsigned)burst && vblanks >= burst_at) {
            log_display("frame", k);
#ifdef HLE_PSP
            if (GE_ACTIVE) {
                uint32_t *px = ge_shown_pixels();
                char nm[64];
                snprintf(nm, sizeof nm, "burst_%03u_%u.bmp", nburst, k);
                if (px) psp_write_bmp(nm, px);
                ge_burst_log(k);
            }
#else
            dump_frame();
#endif
            if (k == burst_n - 1) nburst++;
        }
    }
    if (++vblanks % every == 0 && (dumps < 200 || HLE_IS_PSP)) {
        if (trace_on) {
            trace_flush();
            snap_buffer(9000, 0);
            snap_buffer(9001, 0x70);
            LOG("gstrace: end; PMODE %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx BGCOLOR %llx\n",
                (unsigned long long)hle_gs_priv(0x00), (unsigned long long)hle_gs_priv(0x70),
                (unsigned long long)hle_gs_priv(0x80), (unsigned long long)hle_gs_priv(0x90),
                (unsigned long long)hle_gs_priv(0xA0), (unsigned long long)hle_gs_priv(0xE0));
        }
        LOG("gs_soft: vtime %.2f s\n", (double)hle_vtime / EE_HZ);
        dump_frame();
    }
    draw_on = all || GE_ACTIVE || every <= 4 || (vblanks % every) >= every - 4
              || (burst > 0 && ((vblanks % (unsigned)burst) >= (unsigned)burst - 4 || (vblanks % (unsigned)burst) < 3));
    trace_on = trace_at >= 0 && (int)dumps == trace_at && (vblanks % every) >= every - 4;
    if (trace_on) LOG("gstrace: begin vblank %u\n", vblanks);
}

#ifdef HLE_PSP
#include "gs_ge.h"
#endif

uint64_t gs_stat_uploads(void) { return stat_uploads; }

int gs_bench_logging(void) { return bench_on; }
