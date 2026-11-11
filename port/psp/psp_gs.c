/*
 * PSP picture, first version: the finished frame of the software GS (gs_core.c) is read out of the emulated GS memory,
 * scaled with nearest neighbour to the PSP's 480 x 272 and written into the screen buffer. Slow, but it shows the game.
 * (Stage 1 replaces this with sceGu sprites.)
 *
 * The game's frame is fbw * 64 pixels wide (512 for BT3) and 448 high. gs_core.c only draws every gPspStep-th pixel
 * (psp_main.c, step.txt), so the picture is read from those pixels: the source column and row are rounded down to a
 * multiple of the step. Rows that read the same source row are copied, not read again.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <stdint.h>
#include <string.h>

#define SRC_H 448
#define DST_W 480
#define DST_H 272

extern int gVitaGsSkipR;          /* gs_core.c: 1 = the frame just finished was not drawn */
extern volatile int gPspSkipRun;
extern volatile int gPspStep;
extern unsigned gPspSkipped, gPspDrawn, gPspPresentMs;
extern volatile int gPspPhase, gPspPhaseArg;

volatile int gPspPicture; /* 1 once a frame has been shown: the debug text then stays off the screen */
unsigned gPspPresented;
unsigned long long gPspGsUs; /* time spent in the GS front end consuming lists (the patched Port_GsVif1Chain, build.py) */
unsigned gPspGsCalls;
extern volatile int gPspGuActive;
extern volatile int gPspDebug;
extern void Psp_DebugOverlay(int vramOffset);

void Vita_GsPresent(uint32_t (*read)(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y), uint32_t bp, uint32_t fbw, uint32_t psm) {
    static uint16_t sx[DST_W];
    static uint32_t line[DST_W];
    static unsigned srcW, step;
    volatile uint32_t *fb = (volatile uint32_t *)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u); /* uncached: no cache to write back */
    int x, y, bits16 = (psm == 0x02 || psm == 0x0A), lastSy = -1;
    unsigned w = fbw * 64u, st = (unsigned)gPspStep;
    unsigned long long t0;

    if (gPspGuActive) {
        return; /* the GE draws the picture (psp_gu.c) */
    }

    if (gVitaGsSkipR) { /* not drawn: the screen keeps the picture before */
        gPspSkipRun++;
        gPspSkipped++;
        return;
    }
    gPspSkipRun = 0;
    gPspDrawn++;
    gPspPhase = 20; /* copying a picture to the screen */
    t0 = sceKernelGetSystemTimeWide();
    if (w < 256 || w > 640) {
        w = 512;
    }
    if (st != 1 && st != 2 && st != 4) {
        st = 2;
    }
    if (w != srcW || st != step) {
        for (x = 0; x < DST_W; x++) {
            sx[x] = (uint16_t)(((unsigned)x * w / DST_W) & ~(st - 1));
        }
        srcW = w;
        step = st;
    }
    if (!gPspPicture) {
        sceDisplaySetMode(0, DST_W, DST_H);
        sceDisplaySetFrameBuf(sceGeEdramGetAddr(), 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
        gPspPicture = 1;
    }
    for (y = 0; y < DST_H; y++) {
        gPspPhaseArg = y;
        int sy = (int)(((unsigned)y * SRC_H / DST_H) & ~(st - 1));
        volatile uint32_t *row = fb + y * 512;
        if (sy != lastSy) {
            int lastSx = -1;
            uint32_t c = 0;
            for (x = 0; x < DST_W; x++) {
                if ((int)sx[x] != lastSx) {
                    lastSx = (int)sx[x];
                    c = read(bp, fbw, psm, sx[x], (uint32_t)sy);
                    if (bits16) {
                        c = 0xFF000000u | ((c & 31u) << 3) | (((c >> 5) & 31u) << 11) | (((c >> 10) & 31u) << 19);
                    } else {
                        c |= 0xFF000000u; /* the GS's colour is already R in the low byte, like the PSP's 8888 */
                    }
                }
                line[x] = c;
            }
            lastSy = sy;
        }
        for (x = 0; x < DST_W; x++) {
            row[x] = line[x];
        }
    }
    gPspPhase = 21; /* picture done */
    gPspPresented++;
    if (gPspDebug) {
        Psp_DebugOverlay(0);
    }
    gPspPresentMs += (unsigned)((sceKernelGetSystemTimeWide() - t0) / 1000ull);
}
