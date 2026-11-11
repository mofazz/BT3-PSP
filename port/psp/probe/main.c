/* BT3 PSP - Stage 0a probe.
 *
 * Answers, on the real console, the questions the port depends on, before any game code is built:
 *   1. how much user memory did we get (is the 64 MB mode on)?
 *   2. can a 28.3 MB game heap plus the program itself fit?
 *   3. what clock does the CPU really run at (the firmware's getters lie under an ARK overclock)?
 *   4. how fast are integer, float, divide and memory copy loops (a yardstick for the game's logic)?
 *   5. are the disc files where the port expects them?
 * Everything is also written to ms0:/PSP/GAME/BT3PROBE/probe.txt. Press CROSS to exit.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define BUILD "probe-0a-1"

PSP_MODULE_INFO("BT3PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(1024); /* small libc heap: the rest of the partition stays free for the measurements below */

static char sDisc[160] = "ms0:/PSP/GAME/bt3/disc";
static const char *const kRoots[] = {
    "ms0:/PSP/GAME/bt3/disc", "ms0:/bt3/disc", "ms0:/GAME/bt3/disc", "ms0:/game/bt3/disc",
    "ms0:/PSP/GAME/BT3/disc", "ms0:/PSP/GAME/BT3PROBE/disc", "ms0:/BT3/disc", "ms0:/PSP/bt3/disc", NULL};
#define LOGDIR "ms0:/PSP/GAME/BT3PROBE"

static FILE *sLog;

static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pspDebugScreenPrintf("%s\n", buf);
    if (sLog) {
        fprintf(sLog, "%s\n", buf);
        fflush(sLog);
    }
}

static int exit_cb(int a1, int a2, void *arg) {
    sceKernelExitGame();
    return 0;
}
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_exit(void) {
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, THREAD_ATTR_USER, NULL);
    if (t >= 0) {
        sceKernelStartThread(t, 0, NULL);
    }
}

static unsigned now_us(void) {
    return sceKernelGetSystemTimeLow();
}

/* the largest block the user partition will give, in 1 MB steps (allocated, then freed again) */
static unsigned largest_block_mb(void) {
    unsigned mb;
    for (mb = 56; mb > 0; mb--) {
        SceUID id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "probe", PSP_SMEM_Low, mb << 20, NULL);
        if (id >= 0) {
            sceKernelFreePartitionMemory(id);
            return mb;
        }
    }
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

/* the first of the usual places that holds SLUS_216.78 */
static int find_disc(void) {
    int i;
    char p[200];
    for (i = 0; kRoots[i]; i++) {
        snprintf(p, sizeof(p), "%s/SLUS_216.78", kRoots[i]);
        if (file_size(p) != 0xFFFFFFFFu) {
            snprintf(sDisc, sizeof(sDisc), "%s", kRoots[i]);
            return 1;
        }
    }
    return 0;
}

static void check_file(const char *rel) {
    char p[160];
    unsigned n;
    snprintf(p, sizeof(p), "%s/%s", sDisc, rel);
    n = file_size(p);
    if (n == 0xFFFFFFFFu) {
        say("  MISSING  %s", rel);
    } else {
        say("  %8u KB  %s", n >> 10, rel);
    }
}

/* ---- benchmarks: each returns microseconds ---- */
static unsigned bench_int(void) {
    volatile unsigned sink;
    unsigned a = 1, b = 2, c = 3, d = 4, i, t0 = now_us();
    for (i = 0; i < 4000000u; i++) { /* a dependent chain of 8 adds per turn */
        a += b; b += c; c += d; d += a; a += d; b += a; c += b; d += c;
    }
    sink = a ^ b ^ c ^ d;
    (void)sink;
    return now_us() - t0;
}

static unsigned bench_float(void) {
    volatile float sink;
    float x = 1.0f, y = 0.5f;
    unsigned i, t0 = now_us();
    for (i = 0; i < 1000000u; i++) {
        x = x * 1.0000001f + y;
        y = y * 0.9999999f - x * 0.0000001f;
    }
    sink = x + y;
    (void)sink;
    return now_us() - t0;
}

static unsigned bench_div(void) {
    volatile float sink;
    float x = 1.0f, d = 1.0001f;
    unsigned i, t0 = now_us();
    for (i = 0; i < 500000u; i++) {
        x = x / d + 1.0f;
    }
    sink = x;
    (void)sink;
    return now_us() - t0;
}

static unsigned bench_copy(void *a, void *b, unsigned bytes) {
    unsigned i, t0 = now_us();
    for (i = 0; i < 8; i++) {
        memcpy(b, a, bytes);
    }
    return now_us() - t0;
}

int main(int argc, char **argv) {
    SceCtrlData pad;
    unsigned t, big;
    SceUID heap;
    void *base = NULL;

    pspDebugScreenInit();
    setup_exit();
    sceIoMkdir(LOGDIR, 0777);
    sLog = fopen(LOGDIR "/probe.txt", "w");

    say("BT3 PSP Stage 0a probe  build %s", BUILD);
    say("firmware 0x%08X", (unsigned)sceKernelDevkitVersion());

    /* the official call at the very start: 333/333/166. Under an ARK overclock it does nothing (see below). */
    scePowerSetClockFrequency(333, 333, 166);
    say("clock getters say cpu=%d bus=%d (they report 333 even under an ARK overclock)", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());

    say("");
    say("[memory]");
    say("total free %u KB, max block %u KB", (unsigned)sceKernelTotalFreeMemSize() >> 10, (unsigned)sceKernelMaxFreeMemSize() >> 10);
    big = largest_block_mb();
    say("largest single block: %u MB   (>= 40 means the 64 MB mode is on)", big);

    heap = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "bt3heap", PSP_SMEM_Low, 29u << 20, NULL);
    if (heap >= 0) {
        base = sceKernelGetBlockHeadAddr(heap);
        say("29 MB game heap: OK at %p; max block after it: %u KB", base, (unsigned)sceKernelMaxFreeMemSize() >> 10);
        say("  (the translated code is ~4-13 MB, so this leaves the rest for textures, audio, buffers)");
    } else {
        say("29 MB game heap: FAILED (0x%08X) -> extra memory is off or the stick is damaged", (unsigned)heap);
    }

    say("");
    say("[speed]  (1 cycle per add assumed; compare 333 MHz runs with your ARK setting)");
    t = bench_int();
    say("int : %u us -> about %u MHz", t, (unsigned)(32000000ull / (t ? t : 1)));
    t = bench_float();
    say("float: 1M mul-add pairs in %u us (%u ns each)", t, t / 1000);
    t = bench_div();
    say("div  : 500k float divides in %u us (%u ns each)", t, t / 500);
    if (heap >= 0) {
        unsigned bytes = 4u << 20;
        unsigned char *a = (unsigned char *)base, *b = a + bytes;
        memset(a, 0x5A, bytes);
        t = bench_copy(a, b, bytes);
        say("copy : 8 x 4 MB memcpy in %u us -> %u MB/s", t, (unsigned)(32ull * 1000000ull / (t ? t : 1)));
    }

    say("");
    if (find_disc()) {
        say("[disc files found under %s]", sDisc);
    } else {
        say("[disc files: SLUS_216.78 not found in any of the usual places:]");
        { int i; for (i = 0; kRoots[i]; i++) say("  %s", kRoots[i]); }
    }
    check_file("SLUS_216.78");
    check_file("BIN/DBZP.BIN");
    check_file("DATA/PZS3US0.AFS");
    check_file("DATA/PZS3US1.AFS");
    check_file("DATA/PZS3US2.AFS");
    check_file("DATA/ZS3USOP.PSS");

    say("");
    say("Done. Log: " LOGDIR "/probe.txt.  Press CROSS to exit.");
    if (sLog) {
        fclose(sLog);
        sLog = NULL;
    }
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CROSS) {
            break;
        }
        sceKernelDelayThread(50000);
    }
    sceKernelExitGame();
    return 0;
}
