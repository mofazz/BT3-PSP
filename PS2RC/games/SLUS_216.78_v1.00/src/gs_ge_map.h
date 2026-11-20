/* gs_ge_map.h - BT3 (SLUS-216.78 v1.00), PSP ONLY, included by gs_ge_vu1.h: a native stage map with LOD.
 *
 * ps2rc.cfg map_native=1: when ms0:/PSP/GAME/PS2RC_BT3/maps/stage_NN.bt3m exists for the battle's stage
 * (BattleSetup 0x331DC8 + 28; made on the player's machine by work/stage_pack.py from their own disc: a stage
 * extracted with stage_dump, cut into cells with three levels of detail), the stage's program-4 geometry (its
 * static meshes) is not drawn from the game's VU1 batches: at the first program-4 batch (or mesh cache replay) of
 * each frame the map is drawn instead, once, with the projection the game set for the stage (VU1 0..3, through
 * nat_begin: target, depth, scissor as the game's) - every cell whose box is inside the camera's clip volume (VU1
 * 4..7) at the level its distance asks for (the camera's position is the null point of the clip matrix's x, y, w
 * rows); opaque materials first, then cut-outs, then blended ones. Every later program-4 batch of the frame is
 * dropped. The game itself is unchanged: it still culls and queues the original meshes (collisions, breakable
 * objects, the scrolling sky and the other programs draw as before).
 * File: work/stage_pack.py. map_lod=0/1/2 forces one level (tests); map_nocull=1 draws every cell. */
typedef struct { uint32_t w, h, pal, px; } MapTex;
typedef struct { int32_t tex; uint32_t kind, r0, r1; } MapMat;
typedef struct { float mn[3], mx[3], d1, d2; uint32_t nb[3], bo[3]; } MapCell;
typedef struct { uint32_t mat, nv, ni, vo, io, r[3]; } MapBatch;
static int map_on = -1, map_lod = -1, map_nocull, map_noclip;
static uint8_t *map_buf;
static uint32_t map_stage = 0xFFFFFFFFu, map_ncell, map_nmat, map_ntex, map_drawn = 0xFFFFFFFFu;
static const MapTex *map_tex;
static const MapMat *map_mat;
static const MapCell *map_cell;
static uint64_t map_st_frames, map_st_cells, map_st_tris, map_st_lod[3];

static int map_load(uint32_t stage)
{
    char path[96];
    snprintf(path, sizeof path, "ms0:/PSP/GAME/PS2RC_BT3/maps/stage_%02u.bt3m", stage);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { LOG("gs_map: no native map for stage %u (%s)\n", stage, path); return 0; }
    const uint32_t n = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    static uint32_t cap;                              /* (one buffer for the battle: a stage change reuses it) */
    if (!map_buf) {
        cap = n + 64;
        if ((uint32_t)ge_opt("map_kb", 0) << 10 > cap) cap = (uint32_t)ge_opt("map_kb", 0) << 10;
        extern uint32_t bt3_stage_change_to(uint32_t stage);   /* (pack_main.c: a destroyed stage, 15 or 3) */
        const uint32_t to = bt3_stage_change_to(stage);
        if (to != 0xFFu) {
            char p2[96];
            snprintf(p2, sizeof p2, "ms0:/PSP/GAME/PS2RC_BT3/maps/stage_%02u.bt3m", to);
            SceUID f2 = sceIoOpen(p2, PSP_O_RDONLY, 0);
            if (f2 >= 0) {
                const uint32_t n2 = (uint32_t)sceIoLseek(f2, 0, PSP_SEEK_END) + 64;
                sceIoClose(f2);
                if (n2 > cap) cap = n2;
            }
        }
        map_buf = ge_kalloc("native_map", cap);
    }
    if (n > cap) { LOG("gs_map: %s needs %u KB, the buffer has %u KB (map_kb)\n", path, n >> 10, cap >> 10);
                   sceIoClose(fd); return 0; }
    int ok = map_buf && sceIoRead(fd, map_buf, n) == (int)n;
    sceIoClose(fd);
    uint32_t h[8], cell_off;
    if (ok) { memcpy(h, map_buf, 32); memcpy(&cell_off, map_buf + n - 4, 4); }
    if (!ok || memcmp(h, "BT3M", 4) || h[1] != 1 || h[2] != stage) { LOG("gs_map: %s unusable\n", path); return 0; }
    map_ncell = h[3]; map_nmat = h[4]; map_ntex = h[5];
    map_tex = (const MapTex *)(map_buf + h[6]);
    map_mat = (const MapMat *)(map_buf + h[7]);
    map_cell = (const MapCell *)(map_buf + cell_off);
    sceKernelDcacheWritebackRange(map_buf, n);
    LOG("gs_map: stage %u: %u cells, %u materials, %u textures, %u KB (%s)\n", stage, map_ncell, map_nmat, map_ntex,
        n >> 10, path);
    return 1;
}
/* 1 = the native map replaces this stage's program-4 geometry */
static int map_active(void)
{
    if (map_on < 0) {
        map_on = ge_opt("map_native", 0);
        map_lod = ge_opt("map_lod", -1);
        map_nocull = ge_opt("map_nocull", 0);
        map_noclip = ge_opt("map_noclip", 0);
    }
    if (!map_on) return 0;
    uint32_t st;
    memcpy(&st, hle_ram + 0x331DC8u + 28, 4);
    if (st != map_stage) {
        map_stage = st;
        map_ncell = 0;
        if (st <= 0x40) map_load(st);
    }
    return map_ncell != 0;
}

/* the camera: the point where the clip matrix's x, y and w rows are 0 (C columns at VU 4..7) */
static void map_cam(const float *C, float *pos)
{
    float r[3][4];
    for (int c = 0; c < 4; c++) { r[0][c] = C[4 * c + 0]; r[1][c] = C[4 * c + 1]; r[2][c] = C[4 * c + 3]; }
    float n[4];
    for (int i = 0; i < 4; i++) {                     /* cofactors: n_i = (-1)^i det(the 3 x 3 without column i) */
        int k[3], m = 0;
        for (int j = 0; j < 4; j++) if (j != i) k[m++] = j;
        float d = r[0][k[0]] * (r[1][k[1]] * r[2][k[2]] - r[1][k[2]] * r[2][k[1]])
                - r[0][k[1]] * (r[1][k[0]] * r[2][k[2]] - r[1][k[2]] * r[2][k[0]])
                + r[0][k[2]] * (r[1][k[0]] * r[2][k[1]] - r[1][k[1]] * r[2][k[0]]);
        n[i] = (i & 1) ? -d : d;
    }
    const float w = n[3] != 0.0f ? n[3] : 1e-6f;
    pos[0] = n[0] / w; pos[1] = n[1] / w; pos[2] = n[2] / w;
}
/* 1 = some of the box may be inside the clip volume (|x|, |y| <= w, w > 0) */
static int map_box_in(const float *C, const float *mn, const float *mx)
{
    unsigned allout = 0x1F;
    for (int k = 0; k < 8; k++) {
        const float p[3] = {(k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2]};
        float q[4];
        for (int r = 0; r < 4; r++) q[r] = C[r] * p[0] + C[4 + r] * p[1] + C[8 + r] * p[2] + C[12 + r];
        unsigned o = (q[0] > q[3]) | (q[0] < -q[3]) << 1 | (q[1] > q[3]) << 2 | (q[1] < -q[3]) << 3 | (q[3] <= 0.0f) << 4;
        allout &= o;
    }
    return allout == 0;
}
static float map_dist(const float *cam, const float *mn, const float *mx)
{
    float d2 = 0;
    for (int i = 0; i < 3; i++) {
        const float d = cam[i] < mn[i] ? mn[i] - cam[i] : cam[i] > mx[i] ? cam[i] - mx[i] : 0.0f;
        d2 += d * d;
    }
    return sqrtf(d2);
}

/* a batch of a cell that crosses the guard band: each vertex's out code once; triangles inside the band go to a
   short index list (drawn from the map file's own vertices), triangles off one side of the screen go away, the rest
   are cut (nat_tri_sh: near plane, band edges) - only those are copied into the display list (copying whole cells
   filled it: the list restarted under textures still in use) */
static uint64_t map_st_cut, map_st_cuttri;
static void map_draw_cut(const MapBatch *b)
{
    static uint16_t oc[8192];
    const uint16_t *ix = (const uint16_t *)(map_buf + b->io);
    const GeV3 *vx = (const GeV3 *)(map_buf + b->vo);
    const uint32_t fmt = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D;
    if (b->nv > 8192) { sceGuDrawArray(GU_TRIANGLES, fmt, (int)b->ni, ix, vx); return; }
    for (uint32_t i = 0; i < b->nv; i++) oc[i] = (uint16_t)nat_out(&vx[i]);
    uint16_t *keep = ge_getmem(b->ni * 2u);
    uint32_t nk = 0;
    nat_cn = 0;
    for (uint32_t t = 0; t + 3 <= b->ni; t += 3) {
        const uint16_t a = ix[t], c1 = ix[t + 1], c2 = ix[t + 2];
        const int o = oc[a] | oc[c1] | oc[c2];
        if (!(o & 63)) { keep[nk++] = a; keep[nk++] = c1; keep[nk++] = c2; continue; }
        if (oc[a] & oc[c1] & oc[c2] & (64 | 128 | 256 | 512)) continue;
        if (nat_cn + 36 > NAT_CLIP_MAX) {               /* (the cutter's buffer full: out with what it holds) */
            GeV3 *v = ge_getmem(nat_cn * sizeof(GeV3));
            memcpy(v, nat_cv, nat_cn * sizeof(GeV3));
            sceGuDrawArray(GU_TRIANGLES, fmt & ~GU_INDEX_16BIT, (int)nat_cn, NULL, v);
            nat_cn = 0;
        }
        nat_tri_sh(&vx[a], &vx[c1], &vx[c2]);
        map_st_cuttri++;
    }
    if (nk) sceGuDrawArray(GU_TRIANGLES, fmt, (int)nk, keep, vx);
    if (nat_cn) {
        GeV3 *v = ge_getmem(nat_cn * sizeof(GeV3));
        memcpy(v, nat_cv, nat_cn * sizeof(GeV3));
        sceGuDrawArray(GU_TRIANGLES, fmt & ~GU_INDEX_16BIT, (int)nat_cn, NULL, v);
    }
    map_st_cut++;
}

static void map_draw(void)
{
    /* the game's state for the stage (target, depth test, scissor) and its projection, without a texture */
    const uint64_t p0 = reg[0x00], p1 = reg[0x1B];
    reg[0x00] &= ~0x50ull; reg[0x1B] &= ~0x50ull;     /* (TME, ABE off) */
    const GeTex *t;
    const int prim = nat_begin(0, &t);
    reg[0x00] = p0; reg[0x1B] = p1;
    if (prim < 0) return;
    float C[16], cam[3];
    memcpy(C, vu1_mem + 16 * 4, 64);
    map_cam(C, cam);
    static uint8_t lev[1024], band[1024];
    uint32_t nvis = 0;
    /* hierarchical LOD (stage_pack.py): a block (d2 = -1) is drawn whole at its LOD2 when the camera is past its d1,
       else its cells (d2 = -2, the nb[0] that follow it from bo[1]) each at their LOD1 */
    uint32_t blk_end = 0;
    int blk_near = 0;
    for (uint32_t i = 0; i < map_ncell && i < 1024; i++) {
        const MapCell *c = &map_cell[i];
        if (c->d2 == -2.0f) {                          /* a block's cell: only when its block is near and in view */
            if (i >= blk_end || !blk_near) { lev[i] = 0xFF; continue; }
            if (!map_nocull && !map_box_in(C, c->mn, c->mx)) { lev[i] = 0xFF; continue; }
            McBatch bx;
            memcpy(bx.bmin, c->mn, 12); memcpy(bx.bmax, c->mx, 12);
            const int in = map_noclip ? 1 : mc_box_inside(&bx);
            if (in < 0 && !map_nocull) { lev[i] = 0xFF; continue; }
            band[i] = (uint8_t)(in > 0);
            lev[i] = 1;
            map_st_lod[1]++;
            nvis++;
            continue;
        }
        if (c->d2 == -1.0f) {                          /* a block */
            blk_end = c->bo[1] + c->nb[0];
            blk_near = 0;
            lev[i] = 0xFF;
            if (!map_nocull && !map_box_in(C, c->mn, c->mx)) continue;
            McBatch bx;
            memcpy(bx.bmin, c->mn, 12); memcpy(bx.bmax, c->mx, 12);
            const int in = map_noclip ? 1 : mc_box_inside(&bx);
            if (in < 0 && !map_nocull) continue;
            if (map_lod != 2 && (map_lod == 1 || map_dist(cam, c->mn, c->mx) < c->d1)) { blk_near = 1; continue; }
            band[i] = (uint8_t)(in > 0);
            lev[i] = 2;
            map_st_lod[2]++;
            nvis++;
            continue;
        }
        if (!map_nocull && !map_box_in(C, c->mn, c->mx)) { lev[i] = 0xFF; continue; }
        /* the GE guard band: a cell wholly inside it is drawn as it is; one that crosses it (the ground under the
           camera) goes through the renderer's triangle cutter (nat_draw3: near plane and band edges), or the GE
           drops its big triangles whole - holes in the ground next to the camera */
        McBatch bx;
        memcpy(bx.bmin, c->mn, 12); memcpy(bx.bmax, c->mx, 12);
        const int in = map_noclip ? 1 : mc_box_inside(&bx);
        if (in < 0 && !map_nocull) { lev[i] = 0xFF; continue; }
        band[i] = (uint8_t)(in > 0);
        const float d = map_dist(cam, c->mn, c->mx);
        /* two levels: the reduced one (LOD1) near, the lowest (LOD2) past the first distance; a cell without a level
           (the backdrop, the scattered pieces: one level only) takes the one it has */
        int lv = map_lod >= 0 ? map_lod : d < c->d1 ? 1 : 2;
        while (lv > 0 && !c->nb[lv]) lv--;
        lev[i] = (uint8_t)lv;
        map_st_lod[lev[i]]++;
        nvis++;
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuDepthMask(0);
    int cur_tex = -2, cur_kind = -1;
    for (uint32_t pass = 0; pass < 3; pass++)          /* opaque, cut-out, blended */
        for (uint32_t i = 0; i < map_ncell && i < 1024; i++) {
            if (lev[i] == 0xFF) continue;
            const MapCell *c = &map_cell[i];
            const MapBatch *b = (const MapBatch *)(map_buf + c->bo[lev[i]]);
            for (uint32_t k = 0; k < c->nb[lev[i]]; k++, b++) {
                const MapMat *m = &map_mat[b->mat < map_nmat ? b->mat : 0];
                if (m->kind != pass) continue;
                if ((int)m->kind != cur_kind) {
                    cur_kind = (int)m->kind;
                    if (cur_kind == 2) { sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); }
                    else sceGuDisable(GU_BLEND);
                    if (cur_kind == 0) sceGuDisable(GU_ALPHA_TEST);
                    else { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, cur_kind == 1 ? 0x7F : 0x00, 0xFF); }
                }
                if (m->tex != cur_tex) {
                    cur_tex = m->tex;
                    if (cur_tex >= 0 && (uint32_t)cur_tex < map_ntex) {
                        const MapTex *x = &map_tex[cur_tex];
                        sceGuEnable(GU_TEXTURE_2D);
                        sceGuClutLoad(32, map_buf + x->pal);
                        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
                        sceGuTexImage(0, (int)x->w, (int)x->h, (int)x->w, map_buf + x->px);
                        sceGuTexFlush();
                    } else sceGuDisable(GU_TEXTURE_2D);
                }
                if (band[i])
                    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT |
                                   GU_TRANSFORM_3D, (int)b->ni, map_buf + b->io, map_buf + b->vo);
                else map_draw_cut(b);
                map_st_tris += b->ni / 3;
            }
        }
    /* the GE state the renderer tracks is no longer what it thinks: every cached piece set again */
    ge_sh.valid = 0;
    memset(ge_bkey, 0xFF, sizeof ge_bkey);
    nat_bm.valid = 0;
    ge_setup_gen++;
    map_st_frames++;
    map_st_cells += nvis;
    if (map_st_frames % 300 == 1)
        LOG("gs_map: frame %llu: %u of %u cells drawn (camera %.0f %.0f %.0f); levels so far %llu / %llu / %llu, %llu "
            "triangles per frame, %llu triangles cut\n", (unsigned long long)map_st_frames, nvis, map_ncell, cam[0], cam[1], cam[2],
            (unsigned long long)map_st_lod[0], (unsigned long long)map_st_lod[1], (unsigned long long)map_st_lod[2],
            (unsigned long long)(map_st_tris / map_st_frames), (unsigned long long)map_st_cuttri);
}
int gs_map_active(void) { return map_active(); }
/* a program-4 batch or replay: 1 = handled by the native map (drawn once per frame, then dropped) */
static int map_take(void)
{
    if (!map_active()) return 0;
    if (map_drawn != (uint32_t)ge_st_flips) {
        map_drawn = (uint32_t)ge_st_flips;
        if (!gs_skipdraw) map_draw();
    }
    return 1;
}
