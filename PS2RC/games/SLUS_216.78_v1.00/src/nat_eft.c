/* nat_eft.c - BT3 (SLUS-216.78 v1.00): A/B switches for the battle's particle effects (dev only). ps2rc.cfg
 * eft_off=bits: 1 = particles (0x186950, EftPtcl_Draw), 2 = ground dust (0x197E18 / 0x1982B8 / 0x198768 / 0x198A98 /
 * 0x199350 / 0x199A40, EftGndDust*_Draw), 4 = scrolling sky layer (0x135200, EftStageScroll_Draw), 8 = water surface (0x1384E0), 16 = stage models (0x115950 cull, 0x115DE0 draw), 32 = water palettes, effect task list, stage effects (0x13A388, 0x1AD3B0, 0x247578), 64 = fighters (0x10FF40), 128 = HUD (0x2129C8), 256 = stage effects 0x247660 / 0x247688, 512 = water palettes alone, 1024 = StgFx_DrawPre alone not drawn - the translated bodies run otherwise. Measures what an effect
 * costs; not a fix. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hle.h"
#include "hle_cfg.h"

static int eft_off(void)
{
    static int v = -1;
    if (v < 0) { const char *e = hle_cfg_get("eft_off"); v = e ? (int)strtol(e, NULL, 0) : 0; }
    return v;
}
#define EFT_SWITCH(addr, bit) \
    void fn_##addr##__ynguest(R5900Ctx *c); \
    void y_nat_##addr(R5900Ctx *c) { if (eft_off() & (bit)) return; fn_##addr##__ynguest(c); }
EFT_SWITCH(00186950, 1)
EFT_SWITCH(00197e18, 2)
EFT_SWITCH(001982b8, 2)
EFT_SWITCH(00198768, 2)
EFT_SWITCH(00198a98, 2)
EFT_SWITCH(00199350, 2)
EFT_SWITCH(00199a40, 2)
EFT_SWITCH(00135200, 4)
EFT_SWITCH(001384e0, 8)
EFT_SWITCH(0013a388, 32 | 512)
EFT_SWITCH(00247578, 32 | 1024)
EFT_SWITCH(0010ff40, 64)      /* BtlObjDraw_Draw: the fighters */
EFT_SWITCH(002129c8, 128)     /* BtlGame_Draw: the HUD */
EFT_SWITCH(00247660, 256)     /* stage effects after the objects */
EFT_SWITCH(00247688, 256)     /* StgFx_DrawPost */

/* The native map (gs_ge_map.h, map_native=1) draws the stage's static meshes itself: the game's culling of them is
 * dropped (StgModel_CullCell 0x1156E0 does nothing: the visibility table stays as StgModel_Cull cleared it, with only
 * the always-drawn parts and the falling debris), so StgModel_Draw queues no static program-4 meshes. Its first
 * program-4 setup (Vu1Pkt_LoadProg4 0x123600: the stage's matrices) is followed by a VIF MARK 0xB7E3 in the display
 * list, where the renderer draws the map. The sky layer, the animated objects and the debris are the game's. */
#ifdef HLE_PSP
int gs_map_active(void);
#else
static int gs_map_active(void) { return 0; }
#endif
static int stg_in_draw, stg_marked;
void fn_00115950__ynguest(R5900Ctx *c);
void y_nat_00115950(R5900Ctx *c) { if (eft_off() & 16) return; fn_00115950__ynguest(c); }
void fn_001156e0__ynguest(R5900Ctx *c);
void y_nat_001156e0(R5900Ctx *c) { if (gs_map_active()) return; fn_001156e0__ynguest(c); }
void fn_00115de0__ynguest(R5900Ctx *c);
void y_nat_00115de0(R5900Ctx *c)
{
    if (eft_off() & 16) return;
    stg_in_draw = 1; stg_marked = 0;
    fn_00115de0__ynguest(c);
    stg_in_draw = 0;
}
void fn_00123600__ynguest(R5900Ctx *c);
void y_nat_00123600(R5900Ctx *c)
{
    fn_00123600__ynguest(c);
    if (!stg_in_draw || stg_marked || !gs_map_active()) return;
    stg_marked = 1;
    uint32_t cur;
    memcpy(&cur, hle_ram + 0x2FE898u, 4);                   /* gDmaCur */
    const uint32_t q[4] = {0x10000000u, 0, 0x0700B7E3u, 0};   /* CNT 0 qwords; VIF MARK 0xB7E3, NOP */
    memcpy(hle_ram + (cur & 0x1FFFFFFu), q, 16);
    cur += 16;
    memcpy(hle_ram + 0x2FE898u, &cur, 4);
}
