/* nat_task.c - BT3 (SLUS-216.78 v1.00): native walker of the battle effect task tree, 0x1AD150 (called from
 * BtlScene_Update). Written from the game's MIPS as PS2RC translates it: for each node of the sibling list at a0 + 4
 * (next = node + 48): if the node's class (node + 40) has an update (class + 0), gp - 22400 = node, call it with
 * a0 = node, and when the class has a hook at + 20 set bit 1 of the node's flag byte; then flag bit 0 -> 0x1ADA80
 * (node), else recurse into the children list at node + 36. ps2rc.cfg task_native=0: the translated body;
 * task_prof=1 (PSP, dev): time each update target and log the top ones every 600 calls of the root. */
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"
#ifdef __PSP__
#include <pspkernel.h>
#endif

void fn_001ad150__ynguest(R5900Ctx *c);
void fn_001ad3b0__ynguest(R5900Ctx *c);
void fn_001ada80(R5900Ctx *c);

static int tk_opt(const char *k, int def) { const char *e = hle_cfg_get(k); return e ? atoi(e) : def; }

#define TP_N 64
static struct { uint32_t fn, calls; uint64_t us; } tp[TP_N];
static int tprof;
static uint32_t tk_now(void)
{
#ifdef __PSP__
    return sceKernelGetSystemTimeLow();
#else
    return 0;
#endif
}
static void tp_add(uint32_t fn, uint32_t us)
{
    for (int i = 0; i < TP_N; i++) {
        if (tp[i].fn == fn || tp[i].fn == 0) { tp[i].fn = fn; tp[i].calls++; tp[i].us += us; return; }
    }
}
static void tp_dump(void)
{
    LOG("task_prof:");
    for (int k = 0; k < 10; k++) {
        int b = -1;
        for (int i = 0; i < TP_N; i++) if (tp[i].fn && (b < 0 || tp[i].us > tp[b].us)) b = i;
        if (b < 0 || tp[b].us == 0) break;
        LOG(" %06x:%u/%lluus", tp[b].fn, tp[b].calls, (unsigned long long)tp[b].us);
        tp[b].us = 0;
    }
    LOG("\n");
    for (int i = 0; i < TP_N; i++) tp[i].fn = 0, tp[i].calls = 0, tp[i].us = 0;
}

static void tk_walk(R5900Ctx *c, uint32_t list, uint32_t sp)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    const uint32_t gp = c->r[28].w[0];
    uint32_t n = rmb_lw(m_, list, 4);
    uint32_t r2 = 0, r3 = 0;
    while (n) {
        uint32_t cls = rmb_lw(m_, n, 40);
        r3 = rmb_lw(m_, cls, 0);
        if (r3) {
            rmb_sw(m_, gp, -22400, n);
            r3 = rmb_lw(m_, rmb_lw(m_, n, 40), 0);
            c->r[4].d[0] = SX32(n);
            c->r[29].d[0] = SX32(sp);
            c->r[31].d[0] = 0x1ad190;
            if (tprof == 1) { uint32_t t0 = tk_now(); rc_dispatch(c, r3); tp_add(r3, tk_now() - t0); }
            else rc_dispatch(c, r3);
            r3 = rmb_lw(m_, rmb_lw(m_, n, 40), 20);
            r2 = rmb_lb(m_, n, 0);
            if (r3) { rmb_sb(m_, n, 0, (uint8_t)(r2 | 2)); r2 = rmb_lb(m_, n, 0); }
        } else r2 = rmb_lb(m_, n, 0);
        const uint32_t next = rmb_lw(m_, n, 48);
        if (r2 & 1) {
            c->r[4].d[0] = SX32(n);
            c->r[29].d[0] = SX32(sp);
            c->r[31].d[0] = 0x1ad1c4;
            if (tprof == 1) { uint32_t t0 = tk_now(); fn_001ada80(c); tp_add(0x1ada80, tk_now() - t0); }
            else fn_001ada80(c);
            r2 = c->r[2].w[0]; r3 = c->r[3].w[0];
        } else {
            const uint32_t ch = rmb_lw(m_, n, 36);
            if (ch) { tk_walk(c, ch, sp - 32); r2 = c->r[2].w[0]; r3 = c->r[3].w[0]; }
        }
        n = next;
    }
    c->r[2].d[0] = SX32(r2);
    c->r[3].d[0] = SX32(r3);
}

void y_nat_001ad150(R5900Ctx *c)
{
    static int on = -1;
    static uint32_t ncall;
    if (on < 0) { on = tk_opt("task_native", 1); tprof = tk_opt("task_prof", 0); }
    if (!on) { fn_001ad150__ynguest(c); return; }
    const uint32_t sp0 = c->r[29].w[0];
    tk_walk(c, c->r[4].w[0], sp0 - 32);
    c->r[29].d[0] = SX32(sp0);
    if (tprof == 1 && ++ncall % 90 == 0) tp_dump();
}

/* 0x1AD3B0 (BtlTaskList_Draw): the same tree, each node whose class has a draw (class + 20) and whose flag byte has
   bit 1 set is drawn (a0 = node, a1 = the draw); flag bit 0 = no children walked. ps2rc.cfg eft_off bit 32: nothing
   drawn (A/B, nat_eft.c); task_prof=1 also times each draw. */
static void tk_draw(R5900Ctx *c, uint32_t list, uint32_t sp)
{
    const R5900Mem m_ = {c->ram, c->ram_mask};
    uint32_t n = rmb_lw(m_, list, 4);
    uint32_t r2 = 0, r3 = 0;
    while (n) {
        const uint32_t fn = rmb_lw(m_, rmb_lw(m_, n, 40), 20);
        r3 = rmb_lb(m_, n, 0);
        if (fn && (r3 & 2)) {
            c->r[4].d[0] = SX32(n);
            c->r[5].d[0] = SX32(fn);
            c->r[29].d[0] = SX32(sp);
            c->r[31].d[0] = 0x1ad3f0;
            if (tprof) { uint32_t t0 = tk_now(); rc_dispatch(c, fn); tp_add(fn | 0x80000000u, tk_now() - t0); }
            else rc_dispatch(c, fn);
            r3 = rmb_lb(m_, n, 0);
            r2 = r3 & 1;
        } else r2 = (fn && !(r3 & 2)) ? (r3 & 1) : (r3 & 1);
        const uint32_t next = rmb_lw(m_, n, 48);
        if (!r2) {
            const uint32_t ch = rmb_lw(m_, n, 36);
            if (ch) { r2 = ch; tk_draw(c, ch, sp - 32); r2 = c->r[2].w[0]; r3 = c->r[3].w[0]; }
        }
        n = next;
    }
    c->r[2].d[0] = SX32(r2);
    c->r[3].d[0] = SX32(r3);
}

void y_nat_001ad3b0(R5900Ctx *c)
{
    static int on = -1, off = 0;
    static uint32_t ncall;
    if (on < 0) {
        on = tk_opt("task_native", 1); tprof = tk_opt("task_prof", 0);
        const char *e = hle_cfg_get("eft_off"); off = e ? (int)strtol(e, NULL, 0) & 32 : 0;
    }
    if (off) return;
    if (!on) { fn_001ad3b0__ynguest(c); return; }
    const uint32_t sp0 = c->r[29].w[0];
    tk_draw(c, c->r[4].w[0], sp0 - 32);
    c->r[29].d[0] = SX32(sp0);
    if (tprof && ++ncall % 90 == 0) tp_dump();
}
