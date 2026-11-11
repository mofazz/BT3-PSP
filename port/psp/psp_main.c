/*
 * PSP start-up, Stage 0b: the game's logic without a picture, timed.
 *
 * Wrapped by the linker: main -> __wrap_main (here), the game's own main -> __real_main. The demo fight of the
 * attract mode runs (headless.c, BT3_DEMO below); Vita_VBlank counts the game's frames and writes the time they took
 * into ms0:/PSP/GAME/BT3PSP/log.txt and onto the screen. CROSS leaves.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psppower.h>
#include "exc/ps2rc_exc.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("BT3PSP", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(3072); /* the C library's heap (the start-up's big buffers use the game heap, see psp_mem.c) */

#define HOME "ms0:/PSP/GAME/BT3PSP"
#define GAME_STACK 0x00400000 /* the Vita gives the game 8 MB; raise this if the log ends in a crash with no message */

extern int __real_main(int argc, char **argv);
extern void Vita_LoadGameData(const char *disc);
extern void Vita_MapMemory(int cdram);
extern void Vita_FloatMode(int fast);
extern int gPortFastFloat;
extern int gVitaGameThreadId;

extern volatile int gPspPicture; /* psp_gs.c: a frame has been shown, no more debug text */
static int sDemo;                /* 1: the attract-demo fight without a picture, timed (hold TRIANGLE at start, or have a demo.txt) */

char gVitaDisc[256] = "ms0:/PSP/GAME/bt3/disc";
static const char *const kDiscRoots[] = {"ms0:/PSP/GAME/bt3/disc", "ms0:/bt3/disc", "ms0:/GAME/bt3/disc", "ms0:/game/bt3/disc", NULL};

#define LOGFILE HOME "/log.txt"
#define RING 65536
static char sRing[RING];
static volatile unsigned sWr, sRd, sDropped;
static volatile int sBoot = 1; /* until the game's thread starts: every line is committed at once */
static unsigned long long sStart;
static volatile const char *sLastStub;

static unsigned long long now_us(void) {
    return (unsigned long long)sceKernelGetSystemTimeWide();
}

/* The card records a file's size when the file is closed: a log that stays open reads 0 bytes after a crash or a hang.
   So every commit opens, appends, closes. */
static void commit(const char *s, unsigned n) {
    SceUID fd = sceIoOpen(LOGFILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, s, n);
        sceIoClose(fd);
    }
}

/* runs before main: if this line is in the log, the program was loaded and started */
static void __attribute__((constructor(101))) early(void) {
    SceUID fd;
    sceIoMkdir(HOME, 0777);
    fd = sceIoOpen(LOGFILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        static const char s[] = "[      0 ms] early: the program started (constructors run)\n";
        sceIoWrite(fd, s, sizeof(s) - 1);
        sceIoClose(fd);
    }
}

void Vita_Log(const char *fmt, ...) {
    char buf[420], line[460];
    int n, i;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    n = snprintf(line, sizeof(line), "[%7u ms] %s", (unsigned)((now_us() - sStart) / 1000ull), buf);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    if (line[n - 1] != '\n') {
        line[n++] = '\n';
    }
    if (sBoot) {
        commit(line, (unsigned)n);
        return;
    }
    if (sWr - sRd + (unsigned)n > RING) {
        sDropped++;
        return;
    }
    for (i = 0; i < n; i++) {
        sRing[(sWr + (unsigned)i) % RING] = line[i];
    }
    sWr += (unsigned)n;
}

/* Writes what the ring holds plus `extra` in one commit. */
static void flush_ring(const char *extra) {
    static char tmp[RING + 512];
    unsigned wr = sWr, rd = sRd, n = 0, i;
    for (i = rd; i != wr; i++) {
        tmp[n++] = sRing[i % RING];
    }
    sRd = wr;
    if (extra) {
        unsigned k = (unsigned)strlen(extra);
        memcpy(tmp + n, extra, k);
        n += k;
    }
    if (n) {
        commit(tmp, n);
    }
}

static unsigned file_size(const char *path);

static void wait_cross(void) {
    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CROSS) {
            break;
        }
        sceKernelDelayThread(50000);
    }
}

void Vita_Fatal(const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Vita_Log("FATAL: %s\n", buf);
    pspDebugScreenPrintf("\n%s\n\nPress CROSS to leave.\n", buf);
    wait_cross();
    sceKernelExitGame();
    for (;;) {
    }
}

/* The linker's stand-in for any function the stage does not have (build.py): it is told once in the log, counted, and
   answers 0. The count is written when the program ends. */
#define MISSING_MAX 160
static const char *sMissName[MISSING_MAX];
static unsigned sMissCount[MISSING_MAX];
static int sMissN;

void Port_Missing(const char *name) {
    int i;
    sLastStub = name;
    for (i = 0; i < sMissN; i++) {
        if (sMissName[i] == name) {
            sMissCount[i]++;
            return;
        }
    }
    if (sMissN < MISSING_MAX) {
        sMissName[sMissN] = name;
        sMissCount[sMissN++] = 1;
    }
    Vita_Log("stub called for the first time: %s\n", name);
}

static void missing_summary(void) {
    int i;
    for (i = 0; i < sMissN; i++) {
        Vita_Log("stub %-24s called %u times\n", sMissName[i], sMissCount[i]);
    }
}

/* ---- the environment variables the shared code reads ---- */
static char sSaves[300] = HOME "/saves"; /* the card folder: <folder of the EBOOT>/saves when that exists (see main) */

char *Psp_Getenv(const char *name) {
    const char *const kv[][2] = {
        {"BT3_DATA", HOME},
        {"BT3_SAVES", sSaves},
        {"BT3_SETTINGS", HOME "/bt3_settings.txt"},
        {NULL, NULL}};
    int i;
    if (sDemo && (strcmp(name, "BT3_DEMO") == 0 || strcmp(name, "BT3_UNCAPPED") == 0)) {
        return "1"; /* the demo fight, and no pacing: we want to see how fast the logic is */
    }
    for (i = 0; kv[i][0]; i++) {
        if (strcmp(name, kv[i][0]) == 0) {
            return (char *)kv[i][1];
        }
    }
    return NULL;
}

/* ---- the frame counter ---- */
static unsigned sFrames;
static unsigned long long sWinT0, sWorst, sLast;
static unsigned long long sTotalUs;
static unsigned sTotalFrames;

#define WINDOW 120

/* Frame skipping. The software renderer needs a long time for one picture, and the game's own clock (the memory card
   check counts 120 frames, fades count frames) only moves when the game's frames go by. So a frame is left undrawn
   when the game is behind the 60 Hz of the vertical blank; at most gPspSkipMax in a row (skip.txt: 0 = never skip).
   gs_core.c asks Vita_SkipWanted at the start of every frame; psp_gs.c counts the skipped ones in gPspSkipRun. */
volatile int gPspPhase;     /* where the game thread is (psp_gs.c / Vita_VBlank set it); the heartbeat writes it to state.txt: after a freeze it says where */
volatile int gPspPhaseArg;
volatile int gPspSkip;
volatile int gPspSkipRun;
volatile int gPspUseGu;         /* gu.txt: the sceGu back end (psp_gu.c) draws instead of the software renderer */
volatile int gPspDebug;         /* the overlay (SELECT + START) */
volatile int gPspStep = 2;       /* 1, 2 or 4: only every n-th pixel is drawn (step.txt) */
unsigned gPspPresentMs;          /* total time copying pictures to the screen */
int gPspSkipMax = 11;
static long long sDebt;          /* microseconds the game is behind the vertical blank */
static unsigned long long sAfterWait;
unsigned gPspSkipped, gPspDrawn;

int Vita_SkipWanted(int async) {
    (void)async;
    return gPspSkip;
}

void Vita_VBlank(unsigned vblanks) {
    unsigned long long t = now_us(), dt;
    (void)vblanks;
    gPspPhase = 10; /* in Vita_VBlank */
    if (sTotalFrames < 8) {
        Vita_Log("vblank #%u reached\n", sTotalFrames + 1);
    }
    if (!sDemo) {
        if (sAfterWait != 0) {
            sDebt += (long long)(t - sAfterWait) - 16683;
            if (sDebt < 0) {
                sDebt = 0;
            } else if (sDebt > 3000000) {
                sDebt = 3000000;
            }
        }
        gPspSkip = gPspSkipMax > 0 && sDebt > 10000 && gPspSkipRun < gPspSkipMax;
        gPspPhase = 11; /* waiting for the vertical blank */
        sceDisplayWaitVblankStart(); /* the menus run at the console's 60 Hz */
        sAfterWait = now_us();
        gPspPhase = 12; /* back in the game */
    }
    if (sLast != 0) {
        dt = t - sLast;
        if (dt > sWorst) {
            sWorst = dt;
        }
    }
    sLast = t;
    if (sFrames == 0) {
        sWinT0 = t;
    }
    sFrames++;
    sTotalFrames++;
    if (sFrames >= WINDOW) {
        unsigned long long us = t - sWinT0;
        sTotalUs += us;
        Vita_Log("vblank %5u: %u vblanks in %llu ms = %llu us each; a 30 fps battle frame (2 vblanks) = %llu us of the 33333 allowed; worst vblank %llu us; pictures drawn %u, left out %u\n",
                 sTotalFrames, sFrames, us / 1000ull, us / sFrames, 2ull * (us / sFrames), sWorst, gPspDrawn, gPspSkipped);
        if (!gPspPicture) {
            pspDebugScreenSetXY(0, 6);
            pspDebugScreenPrintf("vblank %u: %llu us each (battle frame %llu us of 33333)   \n", sTotalFrames, us / sFrames, 2ull * (us / sFrames));
        }
        sFrames = 0;
        sWorst = 0;
    }
}

void __wrap_exit(int code) {
    Vita_Log("exit(%d) after %u vblanks\n", code, sTotalFrames);
    missing_summary();
    if (sTotalFrames > 0 && sTotalUs > 0) {
        unsigned n = (sTotalFrames / WINDOW) * WINDOW;
        if (n > 0) {
            Vita_Log("average over the whole run: %llu us per vblank, %llu us per 30 fps battle frame\n", sTotalUs / n, 2ull * (sTotalUs / n));
        }
    }
    pspDebugScreenPrintf("\nThe game's program ended (exit %d).\nLog: " HOME "/log.txt\nPress CROSS to leave.\n", code);
    flush_ring(NULL);
    wait_cross();
    ps2rc_exc_uninstall();
    sceKernelExitGame();
    for (;;) {
    }
}

/* ---- the debug overlay (SELECT + START): speed and memory, drawn on the picture ---- */
extern unsigned gPspPresented, gPspGuFrames, gPspGuPrims, gPspGuTexDecodes, gPspGuTexHits, gPspGuOtherTarget, gPspGuVuIgnored, gPspGuBigTex, gPspGsCalls;
extern unsigned long long gPspGsUs, gPspGuTexUs;
extern volatile int gPspGuActive;
static SceUID sGameTh = -1;

void Psp_DebugOverlay(int off) {
    static unsigned long long t0;
    static unsigned f0, v0;
    static unsigned long long gs0, tx0;
    static unsigned fpsx10, vbl, gsms, txms;
    unsigned long long now = sceKernelGetSystemTimeWide();
    unsigned shown = gPspPresented + gPspGuFrames;

    if (t0 == 0) {
        t0 = now;
    }
    if (now - t0 >= 1000000ull) {
        unsigned long long dt = now - t0;
        unsigned df = shown - f0;
        fpsx10 = (unsigned)((unsigned long long)df * 10000000ull / dt);
        vbl = (unsigned)((unsigned long long)(sTotalFrames - v0) * 1000000ull / dt);
        gsms = df ? (unsigned)((gPspGsUs - gs0) / df / 1000ull) : 0;
        txms = df ? (unsigned)((gPspGuTexUs - tx0) / df / 1000ull) : 0;
        t0 = now; f0 = shown; v0 = sTotalFrames; gs0 = gPspGsUs; tx0 = gPspGuTexUs;
    }
    pspDebugScreenSetOffset(off);
    pspDebugScreenSetBackColor(0xFF000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("%s  shown %u.%u/s  vbl %u/s  GS %u ms/pic  tex %u ms/pic      \n", gPspGuActive ? "GU" : "SW", fpsx10 / 10, fpsx10 % 10, vbl, gsms, txms);
    pspDebugScreenPrintf("prims %u tex new %u hit %u  other-buf %u big %u vu %u    \n", gPspGuPrims, gPspGuTexDecodes, gPspGuTexHits, gPspGuOtherTarget, gPspGuBigTex, gPspGuVuIgnored);
    pspDebugScreenPrintf("RAM free %d KB (max %d)  game stack free %d KB    \n", (int)(sceKernelTotalFreeMemSize() >> 10), (int)(sceKernelMaxFreeMemSize() >> 10),
                         sGameTh >= 0 ? sceKernelGetThreadStackFreeSize(sGameTh) >> 10 : -1);
    pspDebugScreenPrintf("CPU %d bus %d MHz  battery %d%%  %s    \n", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetBatteryLifePercent(),
                         sDemo ? "timing run" : "menus/game");
    pspDebugScreenSetOffset(0);
}

static void exit_note(const char *s) {
    char b[160];
    int n = snprintf(b, sizeof b, "[%7u ms] %s\n", (unsigned)((now_us() - sStart) / 1000ull), s);
    commit(b, (unsigned)n);
}

static int exit_cb(int a1, int a2, void *arg) {
    int sr = 0, ur = 0;
    char b[120];
    (void)a1; (void)a2; (void)arg;
    exit_note("exit: callback started");
    if (sGameTh >= 0) { /* the game thread works flat out: stop it first so nothing is in the way of the exit */
        sceKernelTerminateDeleteThread(sGameTh);
        exit_note("exit: game thread stopped");
    }
    flush_ring(NULL);
    ps2rc_exc_uninstall2(1, &sr, &ur);
    snprintf(b, sizeof b, "exit: crash module stop %08x unload %08x", (unsigned)sr, (unsigned)ur);
    exit_note(b);
    exit_note("exit: sceKernelExitGame");
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    (void)args; (void)argp;
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

/* Call recorder: when built with BT3_PSP_TRACE (the default for now, build.py) every function of the game calls this on
   entry. The newest 64 addresses sit in a ring; the heartbeat thread saves them to crumbs.txt about every 50 ms, so after
   a crash the file names the functions that ran last (addr2func.py turns the addresses into names). */
#define CRUMBS 64
volatile unsigned gCrumb[CRUMBS];
volatile unsigned gCrumbN;
void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void *fn, void *site) {
    unsigned n = gCrumbN;
    (void)site;
    gCrumb[n % CRUMBS] = (unsigned)(uintptr_t)fn;
    gCrumbN = n + 1;
}
void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void *fn, void *site) {
    (void)fn;
    (void)site;
}

static void write_crumbs(void) {
    static char buf[CRUMBS * 10 + 64];
    unsigned n = gCrumbN, i, len;
    SceUID fd;
    len = (unsigned)snprintf(buf, sizeof(buf), "count=%u newest first:\n", n);
    for (i = 0; i < CRUMBS && i < n; i++) {
        len += (unsigned)snprintf(buf + len, sizeof(buf) - len, "%08x\n", gCrumb[(n - 1 - i) % CRUMBS]);
    }
    fd = sceIoOpen(HOME "/crumbs.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

/* A console that "turns off" may be a power fault rather than a program fault, so the log says what the battery and the
   clock were doing: percent, millivolts, temperature, charging, mains, and the clock the firmware reports. */
static void power_line(char *out, unsigned n) {
    snprintf(out, n, "battery %d%% %d mV %d C%s%s, cpu %d bus %d MHz", scePowerGetBatteryLifePercent(), scePowerGetBatteryVolt(), scePowerGetBatteryTemp(),
             scePowerIsBatteryCharging() ? " charging" : "", scePowerIsPowerOnline() ? " mains" : "", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
}

/* (sGameTh: defined above) */

/* state.txt: one short line, rewritten every 100 ms by the heartbeat. After a freeze or crash it holds the last moment
   the console was alive: the game's vblank count, the phase (see gPspPhase) and the pictures done. */
static void write_state(void) {
    char b[160];
    int n = snprintf(b, sizeof(b), "ms=%u vblanks=%u phase=%d arg=%d drawn=%u skipped=%u\n", (unsigned)((now_us() - sStart) / 1000ull), sTotalFrames,
                     (int)gPspPhase, (int)gPspPhaseArg, gPspDrawn, gPspSkipped);
    SceUID fd = sceIoOpen(HOME "/state.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, b, (unsigned)n);
        sceIoClose(fd);
    }
}

/* Once a second, from a thread that outranks the game: what the game has done so far. If the game hangs or spins this
   still runs, so the log shows whether the vblank count moves, and the last stand-in function that was called. */
extern unsigned gPspGuRtt, gPspGuTextured, gPspGuPlain, gPspGuFrames, gPspGuPrims, gPspGuTexDecodes, gPspGuTexHits, gPspGuOtherTarget, gPspGuVuIgnored, gPspGuBigTex;
extern unsigned long long gPspGuTexUs;
extern volatile int gPspGuActive;
extern int gGsMainFbp;

static int heartbeat_thread(SceSize args, void *argp) {
    unsigned last = 0, tick = 0, lastN = 0;
    char line[420];
    (void)args; (void)argp;
    for (;;) {
        unsigned v;
        sceKernelDelayThread(50000);
        tick++;
        if (tick % 2 == 0) {
            write_state();
        }
        if (tick % 4 == 0 && gCrumbN != lastN) {
            lastN = gCrumbN;
            write_crumbs();
        }
        if (tick % 20 != 0) {
            continue;
        }
        v = sTotalFrames;
        {
            char pw[120];
            power_line(pw, sizeof(pw));
            snprintf(line, sizeof(line), "[%7u ms] alive: %u vblanks (+%u), pictures drawn %u left out %u (copying them took %u ms), last stub %s, free %u KB, stack free %d KB, %u calls; %s\n",
                     (unsigned)((now_us() - sStart) / 1000ull), v, v - last, gPspDrawn, gPspSkipped, gPspPresentMs, sLastStub ? (const char *)sLastStub : "-",
                     (unsigned)sceKernelTotalFreeMemSize() >> 10, sGameTh >= 0 ? sceKernelGetThreadStackFreeSize(sGameTh) >> 10 : -1, gCrumbN, pw);
        }
        flush_ring(line);
        if (gPspGuActive) { /* what the GPU back end has done (psp_gu.c) */
            char g2[260];
            snprintf(g2, sizeof(g2), "[%7u ms] gu: pictures %u prims %u (textured %u plain %u), textures new %u hit %u (%u ms), left out: other buffer %u, too big %u, vu %u, frame-buffer-as-texture %u, gGsMainFbp %d\n",
                     (unsigned)((now_us() - sStart) / 1000ull), gPspGuFrames, gPspGuPrims, gPspGuTextured, gPspGuPlain, gPspGuTexDecodes, gPspGuTexHits, (unsigned)(gPspGuTexUs / 1000ull),
                     gPspGuOtherTarget, gPspGuBigTex, gPspGuVuIgnored, gPspGuRtt, gGsMainFbp);
            flush_ring(g2);
        }
        if (!gPspPicture) {
            pspDebugScreenSetXY(0, 9);
            pspDebugScreenPrintf("alive: %u vblanks (+%u/s)   last stub: %s            \n", v, v - last, sLastStub ? (const char *)sLastStub : "-");
        }
        last = v;
    }
    return 0;
}

/* ---- the crash catcher (PS2RC's ps2rc_exc.prx, see exc/): a user program that faults freezes the console, which then
   switches itself off. With the kernel module loaded (ARK) the fault lands here instead, on the faulting thread, and the
   registers go to crash.txt and log.txt. EPC is where it died: tools/addr2func.py BT3PSP.elf (put the address in a file). ---- */
static volatile int sGameThreadId = -1; /* the game thread's own id, set by the thread */
extern unsigned gPspPresented;

static void crash_report(uint32_t *r) {
    char b[1024], t[200];
    unsigned n = ps2rc_exc_format(r, b, sizeof b);
    SceUID fd;
    int onGame = (sGameTh >= 0 && sceKernelGetThreadId() == sGameThreadId);
    sBoot = 1; /* from now on every log line is written at once */
    snprintf(t, sizeof t, "CRASH: thread %s, %u vblanks, last stub %s, %u pictures shown\n", onGame ? "game" : "other (heartbeat/main)", sTotalFrames,
             sLastStub ? (const char *)sLastStub : "-", gPspPresented);
    fd = sceIoOpen(HOME "/crash.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, b, n);
        sceIoWrite(fd, t, (unsigned)strlen(t));
        sceIoClose(fd);
    }
    commit(b, n);
    commit(t, (unsigned)strlen(t));
    pspDebugScreenPrintf("\n%s%s\nPress CROSS to leave.\n", b, t);
    wait_cross();
    sceKernelExitGame();
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

/* Game_Main draws its first screens (graphics set-up, the memory card screens) before it calls Dma_InitBuffers, so gDmaCur
   is still 0 and the game writes its first display lists to address 0: a PS2 allows that, PPSSPP ignores it, a real PSP
   stops with a bus error (crash.txt: Dma_BeginDirect, v0 = 0). Until Dma_InitBuffers runs, the lists go to this scratch area. */
extern unsigned gDmaBuf[2], gDmaBufEnd[2];
extern int gDmaBufSize;
extern unsigned *gDmaCur;
static unsigned char sEarlyDma[256 * 1024] __attribute__((aligned(64)));

static void early_dma_buffers(void) {
    gDmaBuf[0] = gDmaBuf[1] = (unsigned)(uintptr_t)sEarlyDma;
    gDmaBufEnd[0] = gDmaBufEnd[1] = (unsigned)(uintptr_t)sEarlyDma + (unsigned)sizeof(sEarlyDma);
    gDmaBufSize = (int)sizeof(sEarlyDma);
    gDmaCur = (unsigned *)sEarlyDma;
}

static int game_thread(SceSize args, void *argp) {
    static char *argv[] = {"bt3", NULL};
    (void)args; (void)argp;
    gVitaGameThreadId = sceKernelGetThreadId();
    sGameThreadId = gVitaGameThreadId;
    Vita_Log("the game's thread runs; calling the game's main\n");
    Vita_FloatMode(gPortFastFloat);
    early_dma_buffers();
    __wrap_exit(__real_main(1, argv));
    return 0;
}

static unsigned file_size(const char *path) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    SceOff n;
    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return (unsigned)n;
}

int __wrap_main(int argc, char **argv) {
    SceUID th, cb;
    int i;
    char p[300];

    pspDebugScreenInit();
    cb = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, THREAD_ATTR_USER, NULL);
    if (cb >= 0) {
        sceKernelStartThread(cb, 0, NULL);
    }
    sceIoMkdir(HOME "/saves", 0777);
    sStart = now_us();
    Vita_Log("main reached\n");
    {
        /* clock.txt in the game's folder holds the CPU MHz to run at (222, 266, 333; default 333); the bus runs at half */
        int mhz = 333;
        SceUID cf = sceIoOpen(HOME "/clock.txt", PSP_O_RDONLY, 0);
        if (cf >= 0) {
            char t[16] = {0};
            sceIoRead(cf, t, sizeof(t) - 1);
            sceIoClose(cf);
            if (atoi(t) >= 33 && atoi(t) <= 333) {
                mhz = atoi(t);
            }
        }
        {
            /* A custom firmware may have replaced these calls (the getters can then lie either way), so the log also
               measures the real speed (compare the number between runs: 222 MHz takes 1.5x as long as 333 MHz). */
            int r1 = scePowerSetClockFrequency(mhz, mhz, mhz / 2);
            int r2 = scePowerSetCpuClockFrequency(mhz);
            int r3 = scePowerSetBusClockFrequency(mhz / 2);
            Vita_Log("clock calls returned %d %d %d\n", r1, r2, r3);
        }
        {
            /* skip.txt: the most pictures left out in a row (default 11, 0 = draw every frame) */
            SceUID sf = sceIoOpen(HOME "/skip.txt", PSP_O_RDONLY, 0);
            if (sf >= 0) {
                char t[16] = {0};
                sceIoRead(sf, t, sizeof(t) - 1);
                sceIoClose(sf);
                if (atoi(t) >= 0 && atoi(t) <= 60) {
                    gPspSkipMax = atoi(t);
                }
            }
            Vita_Log("most pictures left out in a row: %d\n", gPspSkipMax);
        }
        {
            /* step.txt: 1 = every pixel, 2 = every second pixel in x and y (default), 4 = every fourth (very blocky, fastest) */
            SceUID sf = sceIoOpen(HOME "/step.txt", PSP_O_RDONLY, 0);
            if (sf >= 0) {
                char t[16] = {0};
                sceIoRead(sf, t, sizeof(t) - 1);
                sceIoClose(sf);
                if (atoi(t) == 1 || atoi(t) == 2 || atoi(t) == 4) {
                    gPspStep = atoi(t);
                }
            }
            Vita_Log("pixel step: %d\n", (int)gPspStep);
        }
        Vita_Log("clock requested: %d MHz\n", mhz);
    }
    {
        char pw[120];
        power_line(pw, sizeof(pw));
        Vita_Log("%s\n", pw);
        sceKernelDelayThread(20000);
        power_line(pw, sizeof(pw));
        Vita_Log("after 20 ms: %s\n", pw);
        {
            /* speed test: 4 million dependent integer operations; ns per operation says what clock the CPU really runs at */
            volatile unsigned x = 1;
            unsigned i;
            unsigned long long t0 = now_us(), us;
            for (i = 0; i < 4000000u; i++) {
                x = x * 3u + 1u;
            }
            us = now_us() - t0;
            Vita_Log("cpu speed test: %llu ns per step (1.5x larger at 222 MHz than at 333 MHz)\n", us * 1000ull / 4000000ull);
        }
    }
    Vita_Log("BT3 PSP stage 0b (game logic, no picture), built " __DATE__ " " __TIME__ "\n");
    Vita_Log("free memory at start: %u KB, largest block %u KB\n", (unsigned)sceKernelTotalFreeMemSize() >> 10, (unsigned)sceKernelMaxFreeMemSize() >> 10);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        sDemo = (pad.Buttons & PSP_CTRL_TRIANGLE) != 0 || file_size(HOME "/demo.txt") != 0xFFFFFFFFu;
    }
    gPspUseGu = file_size(HOME "/gu.txt") != 0xFFFFFFFFu;
    Vita_Log("mode: %s\n", sDemo ? "timing run (the demo fight, no picture)" : gPspUseGu ? "menus/game, the PSP's GPU draws (gu.txt)" : "menus/game, the software renderer draws (slow; put a gu.txt in the game's folder for the GPU)");
    Vita_Log("memory plan (KB): game heap %u, game stack %u, C heap 3072, early DMA lists 256, GE list 192 + texture cache 2048 (GPU mode), GS memory 4096 + other GS arrays; the rest is the program itself\n",
             29000u, (unsigned)(GAME_STACK >> 10));
    {
        char prx[320], detail[256], exedir[260] = HOME;
        int er;
        if (argc > 0 && argv && argv[0]) {
            char *s;
            snprintf(exedir, sizeof(exedir), "%s", argv[0]);
            s = strrchr(exedir, '/');
            if (s) {
                *s = '\0';
            }
        }
        snprintf(prx, sizeof(prx), "%s/ps2rc_exc.prx", exedir);
        er = ps2rc_exc_install(prx, crash_report, detail, sizeof(detail));
        if (er < 0 && strcmp(exedir, HOME) != 0) { /* not beside the EBOOT: the game's folder */
            snprintf(prx, sizeof(prx), HOME "/ps2rc_exc.prx");
            er = ps2rc_exc_install(prx, crash_report, detail, sizeof(detail));
        }
        Vita_Log("crash handler: %s (%s)\n", er >= 0 ? "armed" : "NOT armed", detail);
    }
    pspDebugScreenPrintf("BT3 PSP: %s\nHold TRIANGLE at start for the timing run.\n", sDemo ? "timing run" : "starting the game ...");

    gPortFastFloat = 1; /* the processor's float unit in the PS2's rounding mode (Vita_FloatMode) */
    {
        /* where SLUS_216.78 may be: next to the EBOOT (its path is argv[0]), in a disc folder beside it, beside its folder
           (.../GAME/bt3/disc), and the usual places on the card */
        static char cand[12][300];
        int n = 0, found = 0;
        char dir[260] = "", parent[260] = "";
        if (argc > 0 && argv && argv[0]) {
            char *s;
            snprintf(dir, sizeof(dir), "%s", argv[0]);
            s = strrchr(dir, '/');
            if (s) {
                *s = '\0';
                snprintf(parent, sizeof(parent), "%s", dir);
                s = strrchr(parent, '/');
                if (s) {
                    *s = '\0';
                }
            }
        }
        if (dir[0]) {
            snprintf(cand[n++], sizeof(cand[0]), "%s/disc", dir);
            snprintf(cand[n++], sizeof(cand[0]), "%s", dir);
            snprintf(cand[n++], sizeof(cand[0]), "%s/DISC", dir);
        }
        if (parent[0]) {
            snprintf(cand[n++], sizeof(cand[0]), "%s/bt3/disc", parent);
        }
        for (i = 0; kDiscRoots[i] && n < 12; i++) {
            snprintf(cand[n++], sizeof(cand[0]), "%s", kDiscRoots[i]);
        }
        Vita_Log("started as: %s\n", argc > 0 && argv && argv[0] ? argv[0] : "(no path)");
        for (i = 0; i < n && !found; i++) {
            snprintf(p, sizeof(p), "%s/SLUS_216.78", cand[i]);
            if (file_size(p) != 0xFFFFFFFFu) {
                snprintf(gVitaDisc, sizeof(gVitaDisc), "%s", cand[i]);
                found = 1;
            } else {
                Vita_Log("no SLUS_216.78 in %s\n", cand[i]);
            }
        }
        if (!found) {
            Vita_Fatal("SLUS_216.78 was not found. Put your disc's files (SLUS_216.78, BIN, DATA) in a folder called disc next to the EBOOT, e.g.\n  %s/disc/SLUS_216.78", dir[0] ? dir : "ms0:/PSP/GAME/BT3PSP");
        }
    }
    Vita_Log("disc folder: %s\n", gVitaDisc);
    {
        /* the memory card's folder: "saves" next to the EBOOT if it is there, else the one in ms0:/PSP/GAME/BT3PSP, else a
           new one next to the EBOOT */
        char a[300], b[300];
        SceUID d;
        const char *base = "";
        {
            static char ebootDir[260];
            if (argc > 0 && argv && argv[0]) {
                char *sl;
                snprintf(ebootDir, sizeof(ebootDir), "%s", argv[0]);
                sl = strrchr(ebootDir, '/');
                if (sl) {
                    *sl = '\0';
                    base = ebootDir;
                }
            }
        }
        snprintf(a, sizeof(a), "%s/saves", base[0] ? base : HOME);
        snprintf(b, sizeof(b), "%s", HOME "/saves");
        d = sceIoDopen(a);
        if (d >= 0) {
            sceIoDclose(d);
            snprintf(sSaves, sizeof(sSaves), "%s", a);
        } else if ((d = sceIoDopen(b)) >= 0) {
            sceIoDclose(d);
            snprintf(sSaves, sizeof(sSaves), "%s", b);
        } else {
            sceIoMkdir(a, 0777);
            snprintf(sSaves, sizeof(sSaves), "%s", a);
        }
        Vita_Log("memory card folder: %s (the game looks for %s/card1/BASLUS-21678DBZT3/)\n", sSaves, sSaves);
        d = sceIoDopen(sSaves);
        if (d >= 0) {
            SceIoDirent e;
            int k = 0;
            memset(&e, 0, sizeof(e));
            while (k < 12 && sceIoDread(d, &e) > 0) {
                Vita_Log("  in it: %s\n", e.d_name);
                k++;
                memset(&e, 0, sizeof(e));
            }
            sceIoDclose(d);
        }
    }
    pspDebugScreenPrintf("Reading the game's data tables from %s ...\n", gVitaDisc);
    Vita_MapMemory(0); /* first: the start-up borrows the empty game heap for its files */
    Vita_Log("game heap mapped; free %u KB, largest block %u KB\n", (unsigned)sceKernelTotalFreeMemSize() >> 10, (unsigned)sceKernelMaxFreeMemSize() >> 10);
    Vita_LoadGameData(gVitaDisc);
    Vita_Log("game data tables read; free %u KB, largest block %u KB\n", (unsigned)sceKernelTotalFreeMemSize() >> 10, (unsigned)sceKernelMaxFreeMemSize() >> 10);
    {
        SceUID hb = sceKernelCreateThread("bt3_heartbeat", heartbeat_thread, 0x10, 0x4000, THREAD_ATTR_USER, NULL);
        if (hb >= 0) {
            sceKernelStartThread(hb, 0, NULL);
        }
    }
    Vita_Log("starting the game's thread (stack %d KB)\n", GAME_STACK >> 10);
    sBoot = 0; /* from here the log goes through the ring */
    th = sceKernelCreateThread("bt3_game", game_thread, 0x20, GAME_STACK, THREAD_ATTR_USER, NULL);
    sGameTh = th;
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        Vita_Fatal("cannot start the game's thread (error 0x%08X): not enough memory for its %d MB stack?", (unsigned)th, GAME_STACK >> 20);
    }
    pspDebugScreenPrintf("Running ...\n");
    sceKernelWaitThreadEnd(th, NULL);
    for (;;) {
        sceKernelDelayThread(1000000);
    }
    return 0;
}
