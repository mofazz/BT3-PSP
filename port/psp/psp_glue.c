/*
 * Stage 0b stand-ins for what the Vita's renderer, pads and sound provide: every hook the shared game code calls
 * (the Vita_* functions, the gVita* counters) does nothing and reports "idle / not wanted / zero". The game's logic
 * runs; nothing is drawn. Types are not repeated here: the functions are old-style so that the callers' own
 * prototypes decide (on the MIPS ABI the arguments are in registers the callee may ignore).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <pspkernel.h>
#include <pspctrl.h>

/* every stand-in is weak: if the game or the GS front end defines the same name, theirs is used */
#define W __attribute__((weak))

extern unsigned sceKernelGetSystemTimeLow(void);

/* ---- counters and flags the game code reads or writes ---- */
W int gVitaBisect, gVitaDmaFlush, gVitaTriple, gVitaDump, gVitaGsSkip, gVitaInMenus, gVitaStartMode, gVitaMenuReady, gVitaPerfOn,
    gVitaSeaMark, gVitaSeaOff, gVitaSurfCull, gVitaSurfMeas, gVitaUnpackVolatile, gVitaWide;
W unsigned gVitaBeList[3], gVitaChipGen, gVitaDp[8], gVitaFeMiss[4], gVitaFresh, gVitaFtWhere[4], gVitaOtRun[6], gVitaSurfN[10],
    gVitaUnpackSkipped, gVitaUnpack[2], gVitaUpSkipped, gVitaWaitSkips, gVitaWarmWaits;
W unsigned long long gVitaFeUs[2][2], gVitaSurfCyc[8], gVitaSurfUs[3], gVitaWall[6];
W volatile int gVitaProfMeasure, gVitaShotMark, gVitaZoneOn;
W volatile unsigned gVitaGridVb;
/* more of them, types as the renderer has them are not needed: generous storage of 8-byte units */
#define SPARE(n) W unsigned long long n[64]
SPARE(gVitaCc); SPARE(gVitaCcLeak); SPARE(gVitaCcMax); SPARE(gVitaCcWhy); SPARE(gVitaDirChk); SPARE(gVitaDpMiss);
SPARE(gVitaFeHash); SPARE(gVitaGifCnt); SPARE(gVitaGifCyc); SPARE(gVitaGifN); SPARE(gVitaGrCyc); SPARE(gVitaGrN);
SPARE(gVitaGsSkipF); SPARE(gVitaGsSkipR); SPARE(gVitaInMovie); SPARE(gVitaLazyUnpack); SPARE(gVitaLz);
SPARE(gVitaMenuMode); SPARE(gVitaMenuMoves); SPARE(gVitaPaints); SPARE(gVitaPlanes); SPARE(gVitaRef);
SPARE(gVitaShadeLog); SPARE(gVitaSnap); SPARE(gVitaSnapSame); SPARE(gVitaSnapSame2); SPARE(gVitaTriRun);
SPARE(gVitaTriRunCyc); SPARE(gVitaTriRunN); SPARE(gVitaUp);  SPARE(gVitaUpDyn);
SPARE(gVitaUpDyn2); SPARE(gVitaUpMiss); W unsigned gVitaUpQuick = 1; W unsigned gVitaUpCheck[64]; SPARE(gVitaUpStore); SPARE(gVitaUsUpload);
SPARE(gVitaXfadeSerial);

/* ---- functions that answer "no" / 0 ---- */
W int Vita_AsyncOk() { return 0; }
W int Vita_SkipWanted() { return 0; }
W int Vita_ChipPlan() { return 0; }
W int Vita_ChipResetDue() { return 0; }
W int Vita_FilmWait() { return 0; }
W int Vita_InBattle() { return 0; }
W int Vita_InFight() { return 0; }
W int Vita_MovieDirect() { return 0; }
W int Vita_MoviePlaying() { return 0; }
W int Vita_Pad2Present() { return 0; }
W int Vita_Pad2Setting() { return 0; }
W int Vita_TouchGetSpot() { return 0; }
W int Vita_WarmUp() { return 0; }
W int Vita_OptionGuide() { return 0; }
W int Vita_FlashGone() { return 0; }
/* ---- functions that hand their argument back ---- */
W void *Vita_TextFix(void *str, int kind) { (void)kind; return str; }
W unsigned short *Vita_PadLine(unsigned short *line, int pad) { (void)pad; return line; }
/* ---- functions that do nothing ---- */
W void Vita_ChipFill() {}
W void Vita_DumpFile() {}
W void Vita_FeListBegin() {}
W void Vita_FeListEnd() {}
W void Vita_FilmFxNote() {}
W void Vita_FilmTintNote() {}
W void Vita_FilmTrigger() {}
W void Vita_FlashClipBegin() {}
W void Vita_FlashClipEnd() {}
W void Vita_FlashQuad() {}
W void Vita_GsPresent() {}
W void Vita_IconArt() {}
W void Vita_ListDone() {}
W void Vita_ModsSummary() {}
W void Vita_MovieFrame() {}
W void Vita_MovieStats() {}
W void Vita_OptionArt() {}
W void Vita_OptionArtEnd() {}
W void Vita_OptionCaptions() {}
W void Vita_PaceBe() {}
W void Vita_PaceFe() {}
W void Vita_Pad2SettingSet() {}
W void Vita_PerfListBegin() {}
W void Vita_PerfListEnd() {}
W void Vita_PerfThreadStart() {}
W void Vita_ProfSwitch() {}
W void Vita_T116FeList() {}
W void Vita_T116FeListEnd() {}
W void Vita_TouchSetSpot() {}
W void Vita_UpLog() {}
W void Vita_VersusArt() {}
W void Vita_WatchThread() {}
W void Vita_Z() {}
W void Vita_FastLog() {}

/* ---- the sound chip and its remote calls (what port/src/gs/snd_se.c answers on the PC, without the mixing) ---- */
W int func_002967C0(int block, int cmd, int core, int mode, void *iopAddr, unsigned spuAddr, unsigned size) {
    (void)block; (void)core; (void)mode; (void)iopAddr; (void)spuAddr; (void)size;
    return cmd == 0x80F0 ? 1 : 0; /* "is the transfer over": always, the game waits for 1 (Battle_Load, Snd_IsUploadDone) */
}
W int sceSifCallRpc(void *client, int fno, int mode, void *send, int sendSize, void *recv, int recvSize, void *cb, void *arg) {
    (void)client; (void)fno; (void)mode; (void)send; (void)sendSize; (void)cb; (void)arg;
    if (recv != 0 && recvSize >= 4) {
        memset(recv, 0, 4); /* the reply word */
    }
    return 0;
}
typedef struct { unsigned data, addr; int size, mode; } PspSifDmaData;
W unsigned sceSifSetDma(PspSifDmaData *d, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (d[i].addr != 0 && d[i].data != 0 && d[i].size > 0) {
            memcpy((void *)(uintptr_t)d[i].addr, (void *)(uintptr_t)d[i].data, (size_t)d[i].size);
        }
    }
    return 1;
}
W int scePerfArmPmonGetCounterValue(int type, unsigned counter, unsigned *value) {
    (void)type; (void)counter;
    if (value) {
        *value = sceKernelGetSystemTimeLow() * 333u; /* (a stand-in cycle count: the game only reads it for its profile) */
    }
    return 0;
}
W int sceKernelChangeThreadCpuAffinityMask(int thid, int mask) { (void)thid; (void)mask; return 0; }

/* ---- the GS front end's GPU back end is not here: the software renderer (gs_core.c) draws ---- */
W int GsGpu_Init() { return 0; }
W void GsGpu_Draw() {}
W void GsGpu_DrawVu0() {}
W void GsGpu_DrawVu4() {}
W void GsGpu_DrawVu6() {}
W void GsGpu_FbUpload() {}
W void GsGpu_Native() {}
W void GsGpu_FrameEnd() {}

/* gs_core.c and gs_vu1.c ask for their developer switches through this (build.py renames getenv for them) */
extern volatile int gPspUseGu; /* psp_main.c: gu.txt */
W char *Vita_GsGetenv(const char *name) {
    if (strcmp(name, "BT3_GS") == 0) {
        return gPspUseGu ? "gpu" : "1"; /* "gpu": the sceGu back end (psp_gu.c); "1": the software renderer */
    }
    if (strcmp(name, "BT3_GS_NOSPLIT") == 0) {
        return "1"; /* no second thread for the back end */
    }
    return (char *)0;
}

/* ---- the PSP's pad as the game's first controller (PS2 packet: active-low buttons, sticks resting at 0x80) ---- */
#define B_SELECT 0x0001u
#define B_L3 0x0002u
#define B_R3 0x0004u
#define B_START 0x0008u
#define B_UP 0x0010u
#define B_RIGHT 0x0020u
#define B_DOWN 0x0040u
#define B_LEFT 0x0080u
#define B_L2 0x0100u
#define B_R2 0x0200u
#define B_L1 0x0400u
#define B_R1 0x0800u
#define B_TRIANGLE 0x1000u
#define B_CIRCLE 0x2000u
#define B_CROSS 0x4000u
#define B_SQUARE 0x8000u

extern int Vita_InBattle(void);
extern volatile int gPspDebug; /* psp_main.c: the overlay on/off (SELECT + START) */
W int Port_PadRead(int socket, unsigned char *data) {
    static const struct { unsigned psp, ps2; } map[] = {
        {PSP_CTRL_START, B_START}, {PSP_CTRL_UP, B_UP}, {PSP_CTRL_RIGHT, B_RIGHT}, {PSP_CTRL_DOWN, B_DOWN}, {PSP_CTRL_LEFT, B_LEFT},
        {PSP_CTRL_TRIANGLE, B_TRIANGLE}, {PSP_CTRL_CIRCLE, B_CIRCLE}, {PSP_CTRL_CROSS, B_CROSS}, {PSP_CTRL_SQUARE, B_SQUARE}};
    SceCtrlData pad;
    unsigned btn = 0, i;

    if (socket != 0) {
        return 0; /* one controller */
    }
    memset(&pad, 0, sizeof(pad));
    {
        /* Every sample since the last call is combined: a tap shorter than one (slow) game frame still counts as a press.
           Called again within 4 ms (the game asks more than once a frame), the same answer is given. */
        static SceCtrlData last;
        static unsigned long long lastUs;
        unsigned long long now = sceKernelGetSystemTimeWide();
        if (lastUs == 0 || now - lastUs > 4000ull) {
            SceCtrlData buf[32];
            int n = sceCtrlReadBufferPositive(buf, 32), k;
            if (n > 0) {
                last = buf[n - 1];
                for (k = 0; k < n - 1; k++) {
                    last.Buttons |= buf[k].Buttons;
                }
            } else {
                sceCtrlPeekBufferPositive(&last, 1);
            }
            lastUs = now;
        }
        pad = last;
    }
    {
        int inb = Vita_InBattle(); /* in a battle the D-pad's up and down are R1 and R2 (the PSP has no R2); in the menus they stay up and down */
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (inb && (map[i].psp == PSP_CTRL_UP || map[i].psp == PSP_CTRL_DOWN)) {
                continue;
            }
            if (pad.Buttons & map[i].psp) {
                btn |= map[i].ps2;
            }
        }
        if (inb) {
            if (pad.Buttons & PSP_CTRL_UP) { btn |= B_R1; }
            if (pad.Buttons & PSP_CTRL_DOWN) { btn |= B_R2; }
        }
    }
    if (pad.Buttons & PSP_CTRL_SELECT) { /* SELECT + L / R = L2 / R2; SELECT alone = SELECT */
        if (pad.Buttons & PSP_CTRL_LTRIGGER) { btn |= B_L2; }
        if (pad.Buttons & PSP_CTRL_RTRIGGER) { btn |= B_R2; }
        if (!(pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) { btn |= B_SELECT; }
    } else {
        if (pad.Buttons & PSP_CTRL_LTRIGGER) { btn |= B_L1; }
        if (pad.Buttons & PSP_CTRL_RTRIGGER) { btn |= B_R1; }
    }
    {   /* SELECT + START together switch the debug overlay (and are not passed to the game) */
        static int prevBoth;
        int both = (pad.Buttons & PSP_CTRL_SELECT) && (pad.Buttons & PSP_CTRL_START);
        if (both && !prevBoth) {
            gPspDebug = !gPspDebug;
        }
        prevBoth = both;
        if (both) {
            btn &= ~(B_START | B_SELECT);
        }
    }
    memset(data, 0, 18);
    data[0] = (unsigned char)(~btn & 0xFF);
    data[1] = (unsigned char)(~(btn >> 8) & 0xFF);
    data[2] = 0x80; data[3] = 0x80;   /* right stick: the PSP has none */
    data[4] = pad.Lx; data[5] = pad.Ly; /* left stick (0x80 at rest, as the PS2's) */
    return 18;
}

/* ---- the GS, headless: the game's drawing lists are taken and dropped (replaced by gs_core.c when it is linked) ---- 
W int GsGpu_Enabled() { return 0; }
W void Port_GsVif1Chain() {}
W void Port_GsGifChannel() {}


/* ---- what the Vita's renderer, pads and sound library define and the shared code calls (found by the first link) ---- */
W unsigned gPortDmaBufs[3];                  /* src/sys/dma.c: the game's three list buffers (addresses) */
W unsigned Port_DmaSpare(unsigned size) { (void)size; return 0; } /* the third list buffer: not used without a picture thread */
W void Port_GsSync() {}
W int Port_ClashSquare() { return 0; }
W void Port_ClashSquareSet() {}
W void Port_SeamExact() {}
W int Port_ShadowSil() { return 0; }
W void Port_ShadowSilEnd() {}
W int sceGsPutDrawEnv() { return 0; }
W int sceGsSetDefDBuff() { return 0; }
W int sceGsSwapDBuff() { return 0; }
W unsigned sceKernelGetProcessTimeLow() { return sceKernelGetSystemTimeLow(); }
/* the disc's boot file: found when its name is the Tenkaichi 3 one (src/sys/file.c: Disc_Identify) */
extern char *strstr(const char *, const char *);
W int sceCdSearchFile(void *file, char *name) { (void)file; return name != 0 && strstr(name, "216.78") != 0; }
/* CRI sound (ADXT) and movies (MPEG): nothing is played in this stage */
static unsigned char sAdxHandle[256] __attribute__((aligned(16)));
W void *ADXT_Create() { return sAdxHandle; }
W int ADXT_GetOutVol() { return 0; }
W int ADXT_GetStat() { return 0; }
W void ADXT_Init() {}
W void ADXT_Pause() {}
W void ADXT_SetOutPan() {}
W void ADXT_SetOutVol() {}
W void ADXT_SetOutputMono() {}
W void ADXT_SetReloadSct() {}
W void ADXT_SetSvrFreq() {}
W void ADXT_StartAfs() {}
W void ADXT_StartFname() {}
W void ADXT_Stop() {}
W int sceMpegAddCallback() { return 0; }
W int sceMpegAddStrCallback() { return 0; }
W int sceMpegCreate() { return 0; }
W int sceMpegDelete() { return 0; }
W int sceMpegDemuxPss() { return 0; }
W int sceMpegGetPicture() { return 0; }
W int sceMpegInit() { return 0; }
W int sceMpegIsEnd() { return 1; }
W int sceMpegReset() { return 0; }

/* ---- time, when the C library has none (weak: the library's own wins) ---- */
extern int sceKernelDelayThread(unsigned usec);
__attribute__((weak)) int clock_gettime(clockid_t clk, struct timespec *ts) {
    unsigned long long t = sceKernelGetSystemTimeWide();
    (void)clk;
    ts->tv_sec = (time_t)(t / 1000000ull);
    ts->tv_nsec = (long)((t % 1000000ull) * 1000ull);
    return 0;
}
__attribute__((weak)) int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    sceKernelDelayThread((unsigned)(req->tv_sec * 1000000 + req->tv_nsec / 1000));
    return 0;
}

/* ---- settings: all defaults, nothing stored (plat_settings.c needs pthreads, which this stage does without) ---- */
W int Port_Setting(const char *name, int def) { (void)name; return def; }
W void Port_SettingSave(const char *name, int value) { (void)name; (void)value; }
W void Port_SettingsWrite(void) {}

/* ---- 64-bit atomics: the MIPS32 compiler calls these; the PSP has one core, so interrupts off is atomic enough ---- */
extern unsigned sceKernelCpuSuspendIntr(void);
extern void sceKernelCpuResumeIntr(unsigned flags);
W unsigned long long __atomic_load_8(const volatile void *p, int order) {
    unsigned long long v;
    unsigned s = sceKernelCpuSuspendIntr();
    (void)order;
    v = *(const volatile unsigned long long *)p;
    sceKernelCpuResumeIntr(s);
    return v;
}
W void __atomic_store_8(volatile void *p, unsigned long long v, int order) {
    unsigned s = sceKernelCpuSuspendIntr();
    (void)order;
    *(volatile unsigned long long *)p = v;
    sceKernelCpuResumeIntr(s);
}
W unsigned long long __atomic_fetch_add_8(volatile void *p, unsigned long long v, int order) {
    unsigned long long o;
    unsigned s = sceKernelCpuSuspendIntr();
    (void)order;
    o = *(volatile unsigned long long *)p;
    *(volatile unsigned long long *)p = o + v;
    sceKernelCpuResumeIntr(s);
    return o;
}

/* ---- the opening and ending movies (MPEG-2 in zs3usop.pss / zs3used.pss): nothing here decodes video, so the player would
   upload garbage frames and never end. The game goes straight on (build.py wraps the two functions). ---- */
extern void Vita_Log(const char *fmt, ...);
void __wrap_Movie_PlayOpening(void) { Vita_Log("opening movie skipped (no video decoder on the PSP yet)\n"); }
void __wrap_Movie_PlayEnding(void) { Vita_Log("ending movie skipped (no video decoder on the PSP yet)\n"); }
