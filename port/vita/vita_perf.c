/*
 * The processor's own event counters, for the log of the development build (and of the released one with
 * bt3.txt report=1). They need libperf (https://github.com/shoui520/libperf): a kernel plugin that gives retail
 * Vitas access to the Cortex-A9 performance monitor, and its user module, loaded from app0:libperf.suprx when it is
 * there (it is not in the VPK). Each thread of the port counts for itself: cycles, instructions, data and
 * instruction cache misses, mispredicted branches. Without them nothing is counted and nothing changes.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <string.h>
#include <psp2/perf.h>

int gVitaPerfOn;

/* Loads the user module; 0 if the kernel plugin is not there. Called once, before any thread counts. */
int Vita_PerfInit(void) {
    int status = 0;
    SceUID id = sceKernelLoadStartModule("app0:libperf.suprx", 0, NULL, 0, NULL, &status);
    gVitaPerfOn = id >= 0 && status == 0;
    return gVitaPerfOn ? 1 : (id < 0 ? id : status);
}

/* The calling thread starts counting (counter 0: data cache refills, 1: instruction cache refills, 2: instructions
   renamed, 3: mispredicted branches; 31: cycles). */
void Vita_PerfThreadStart(void) {
    if (!gVitaPerfOn) {
        return;
    }
    scePerfArmPmonStop(0);
    scePerfArmPmonReset(0);
    scePerfArmPmonSelectEvent(0, 0, 0x03);
    scePerfArmPmonSelectEvent(0, 1, 0x01);
    scePerfArmPmonSelectEvent(0, 2, 0x68);
    scePerfArmPmonSelectEvent(0, 3, 0x10);
    scePerfArmPmonStart(0);
}

/* The calling thread's counters now: cycles, data misses, instruction misses, instructions, mispredictions. */
void Vita_PerfRead(uint32_t v[5]) {
    if (!gVitaPerfOn) {
        memset(v, 0, 5 * sizeof(uint32_t));
        return;
    }
    scePerfArmPmonGetCounterValue(0, 31, &v[0]);
    scePerfArmPmonGetCounterValue(0, 0, &v[1]);
    scePerfArmPmonGetCounterValue(0, 1, &v[2]);
    scePerfArmPmonGetCounterValue(0, 2, &v[3]);
    scePerfArmPmonGetCounterValue(0, 3, &v[4]);
}

/* Adds what was counted since `last` to `sum` (64-bit) and moves `last` on. */
void Vita_PerfAccum(uint32_t last[5], uint64_t sum[5]) {
    uint32_t now[5];
    int i;

    Vita_PerfRead(now);
    for (i = 0; i < 5; i++) {
        sum[i] += (uint32_t)(now[i] - last[i]);
        last[i] = now[i];
    }
}
