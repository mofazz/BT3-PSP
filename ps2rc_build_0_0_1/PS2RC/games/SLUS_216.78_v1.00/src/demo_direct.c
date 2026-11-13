/* demo_direct.c - BT3 (SLUS-216.78 v1.00): a test shortcut straight into the attract demo battle.
 *
 * ps2rc.cfg demo_direct=1 (PSP) or BT3_DEMO_DIRECT=1 (host): Progress_Main (overlay 2, 0x336A90: the menu overlay's
 * entry, which runs the boot sequence - partition load, card check, logos - and the title / menus) is replaced by the
 * game's own Demo_SetupBattle (0x2617F0, which the title calls after 1800 idle frames: one of nine CPU-vs-CPU
 * pairings), so Game_Main goes straight on to Battle_Main and the demo battle starts within seconds instead of
 * ~75 s of logos and title. Off by default: the normal path is the original function.
 * Names / addresses: the decomp's config/symbols (menu_a.txt, view_b.txt) and src/main.c, src/menu/menu_a_b.c,
 * src/battle/view_b_c.c. */
#include <stdlib.h>
#include "hle.h"
#ifdef HLE_PSP
#include "hle_cfg.h"
#endif

void ov2_fn_00336a90__guest(R5900Ctx *c);       /* Progress_Main, the original */
void fn_002617f0(R5900Ctx *c);                  /* Demo_SetupBattle */

static int demo_direct(void)
{
    static int v = -1;
    if (v < 0) {
#ifdef HLE_PSP
        const char *e = hle_cfg_get("demo_direct");
#else
        const char *e = getenv("BT3_DEMO_DIRECT");
#endif
        v = e ? atoi(e) : 0;
    }
    return v;
}

HLE_OV(2, 00336a90)
{
    if (!demo_direct()) { ov2_fn_00336a90__guest(c); return; }
    static unsigned n;
    LOG("demo_direct: Progress_Main replaced by Demo_SetupBattle (demo battle %u) at vtime %.2f s\n", ++n,
        (double)hle_vtime / EE_HZ);
    uint64_t ra = c->r[31].d[0];
    /* ps2rc.cfg demo_skip=N: the setup runs N more times first (each picks another of the nine pairings with the
       game's own random numbers, never the same twice in a row): a repeatable way to reach the other demos */
#ifdef HLE_PSP
    const char *k = hle_cfg_get("demo_skip");
#else
    const char *k = getenv("BT3_DEMO_SKIP");
#endif
    for (int i = k ? atoi(k) : 0; i > 0; i--) { fn_002617f0(c); c->r[31].d[0] = ra; }
    fn_002617f0(c);
    c->r[31].d[0] = ra;
}
