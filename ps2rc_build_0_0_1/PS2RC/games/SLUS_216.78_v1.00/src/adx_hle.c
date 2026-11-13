/* adx_hle.c - BT3 (SLUS-216.78 v1.00): CRI ADX middleware (ADXT/PS2EE 9.71) replaced at its public entries.
 *
 * The game reads all of its data through CRI's ADXF library: three AFS archives ("partitions" 0..2,
 * DATA/PZS3US0..2.AFS), files addressed by (partition, index), read in 2048-byte sectors, asynchronously. On the PS2
 * CRI drives the IOP module CRI_ADXI.IRX (SIF RPC server 0x90000200); here the public entries the game calls are
 * hooked (hooks.def, addresses from the Tenkaichi3Decomp symbol file config/symbols/cri_adxf.txt) and served from the
 * disc map: an AFS file is (offset, size) from the archive's own table, a read finishes at once. Same behaviour as
 * the decomp's PC port (port/src/plat_file.c), written for the guest/host split of this runtime.
 *
 * ADXT (streamed music and voices) is SILENT for now: a player is the work area the game passes to ADXT_Create, a
 * start ends at once (status PLAYEND), volumes are remembered. Every start is logged with its file, so the streams the
 * game plays are recorded for a real player later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hle.h"

#define SECTOR 2048u
enum { ADXF_STAT_STOP = 1, ADXF_STAT_READING = 2, ADXF_STAT_READEND = 3, ADXF_STAT_ERROR = 4 };
enum { ADXT_STAT_STOP = 0, ADXT_STAT_PLAYING = 3, ADXT_STAT_PLAYEND = 5 };

/* ---- partitions: the AFS table of each archive (host copy) */
#define MAX_PT 8
static struct {
    int fd;                      /* hle_disc fd of the archive, -1 = not loaded */
    uint32_t count;
    uint32_t *tab;               /* count * (offset, size) */
    char name[64];
} pt[MAX_PT];
static int pt_init_done;

static void pt_init(void)
{
    if (pt_init_done) return;
    for (int i = 0; i < MAX_PT; i++) pt[i].fd = -1;
    pt_init_done = 1;
}

/* ---- open files: handles are guest addresses in a reserved low-RAM block, never dereferenced by the game */
#define HANDLE_BASE 0x00070000u
#define HANDLE_STEP 0x40u
#define MAX_FILES 64
static struct {
    int used;
    int fd;
    uint64_t base;               /* byte offset of the file inside the disc file fd */
    uint32_t size;               /* bytes */
    int32_t pos;                 /* sectors */
    int32_t stat;
} fl[MAX_FILES];

static uint32_t file_new(int fd, uint64_t base, uint32_t size)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (fl[i].used) continue;
        fl[i].used = 1;
        fl[i].fd = fd;
        fl[i].base = base;
        fl[i].size = size;
        fl[i].pos = 0;
        fl[i].stat = ADXF_STAT_STOP;
        return HANDLE_BASE + HANDLE_STEP * (uint32_t)i;
    }
    LOG("ADXF: more than %d files open\n", MAX_FILES);
    return 0;
}

static int file_index(uint32_t h)
{
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % HANDLE_STEP) return -1;
    uint32_t i = (h - HANDLE_BASE) / HANDLE_STEP;
    return i < MAX_FILES && fl[i].used ? (int)i : -1;
}

static uint32_t sectors(uint32_t bytes) { return (bytes + SECTOR - 1) / SECTOR; }

/* "pzs3us1.afs" / "\DATA\PZS3US1.AFS;1" -> "DATA/PZS3US1.AFS" (the game's root directory is data/) */
static void data_path(const char *name, char *out, size_t n)
{
    const char *p = name, *colon = strchr(name, ':');
    if (colon && colon - name < 8) p = colon + 1;
    while (*p == '\\' || *p == '/') p++;
    if (strncasecmp(p, "data/", 5) == 0 || strncasecmp(p, "data\\", 5) == 0) p += 5;
    snprintf(out, n, "DATA/%s", p);
}

/* ADXF_LoadPartitionNw(ptid, fname, dir, ptinfo): the archive's table, read now (status READEND at once) */
HLE_FN(ADXF_LoadPartitionNw)
{
    pt_init();
    int id = (int)ARG(0) & (MAX_PT - 1);
    char path[128];
    data_path(gstr(ARG(1)), path, sizeof path);
    uint32_t size = 0;
    int fd = hle_disc_open(path, &size);
    uint32_t hdr[2] = {0, 0};
    if (fd < 0 || hle_disc_read_fd(fd, 0, hdr, 8) != 8 || memcmp(hdr, "AFS", 4) != 0) {
        LOG("ADXF_LoadPartitionNw(%d, %s): not an AFS archive on the disc\n", id, path);
        RET(-1);
        return;
    }
    free(pt[id].tab);
    pt[id].fd = fd;
    pt[id].count = hdr[1];
    pt[id].tab = malloc((size_t)hdr[1] * 8u);
    snprintf(pt[id].name, sizeof pt[id].name, "%s", path);
    if (!pt[id].tab || hle_disc_read_fd(fd, 8, pt[id].tab, hdr[1] * 8u) != (int64_t)hdr[1] * 8) {
        LOG("ADXF_LoadPartitionNw(%d, %s): table read failed\n", id, path);
        pt[id].fd = -1;
        RET(-1);
        return;
    }
    LOG("ADXF partition %d: %s, %u files\n", id, path, pt[id].count);
    RET(0);
}

HLE_FN(ADXF_GetPtStat)                       /* ADXF_GetPtStat(ptid) */
{
    pt_init();
    RET(pt[ARG(0) & (MAX_PT - 1)].fd >= 0 ? ADXF_STAT_READEND : ADXF_STAT_STOP);
}

HLE_FN(ADXF_OpenAfs)                         /* ADXF_OpenAfs(ptid, flid) -> handle */
{
    pt_init();
    int id = (int)ARG(0) & (MAX_PT - 1);
    uint32_t flid = ARG(1);
    if (pt[id].fd < 0 || flid >= pt[id].count) {
        LOG("ADXF_OpenAfs(%d, %u): no such file\n", id, flid);
        RET(0);
        return;
    }
    RET(file_new(pt[id].fd, pt[id].tab[2 * flid], pt[id].tab[2 * flid + 1]));
}

HLE_FN(ADXF_Open)                            /* ADXF_Open(fname, atr) -> handle: a loose file of the data dir */
{
    char path[128];
    data_path(gstr(ARG(0)), path, sizeof path);
    uint32_t size = 0;
    int fd = hle_disc_open(path, &size);
    if (fd < 0) {
        LOG("ADXF_Open(%s): not on the disc\n", path);
        RET(0);
        return;
    }
    RET(file_new(fd, 0, size));
}

HLE_FN(ADXF_Close)
{
    int i = file_index(ARG(0));
    if (i >= 0) fl[i].used = 0;
    RET(0);
}

HLE_FN(ADXF_GetFsizeSct)
{
    int i = file_index(ARG(0));
    RET(i >= 0 ? sectors(fl[i].size) : 0);
}

/* ADXF_ReadNw(h, nsct, buf): nsct sectors at the current position; the part of the last sector behind the end of the
   file is zero. Done at once: status READEND. */
HLE_FN(ADXF_ReadNw)
{
    int i = file_index(ARG(0));
    int32_t nsct = (int32_t)ARG(1);
    uint32_t buf = ARG(2) & 0x1FFFFFFFu;
    if (i < 0 || nsct <= 0) { RET(0); return; }
    int32_t left = (int32_t)sectors(fl[i].size) - fl[i].pos;
    if (nsct > left) nsct = left;
    if (nsct <= 0) { fl[i].stat = ADXF_STAT_READEND; RET(0); return; }
    uint32_t want = (uint32_t)nsct * SECTOR;
    if (buf + want > hle_ram_end) {
        LOG("ADXF_ReadNw: buffer %08x + %u past guest RAM\n", buf, want);
        fl[i].stat = ADXF_STAT_ERROR;
        RET(0);
        return;
    }
    uint64_t off = (uint64_t)fl[i].pos * SECTOR;
    uint32_t avail = off < fl[i].size ? fl[i].size - (uint32_t)off : 0;
    uint32_t n = want < avail ? want : avail;
    uint8_t *dst = gptr(buf, want);
    int64_t got = n ? hle_disc_read_fd(fl[i].fd, fl[i].base + off, dst, n) : 0;
    if (got < 0) got = 0;
    memset(dst + got, 0, want - (uint32_t)got);
    fl[i].pos += nsct;
    fl[i].stat = ADXF_STAT_READEND;
    RET(nsct);
}

HLE_FN(ADXF_Stop)
{
    int i = file_index(ARG(0));
    if (i >= 0) fl[i].stat = ADXF_STAT_STOP;
    RET(0);
}

HLE_FN(ADXF_GetStat)
{
    int i = file_index(ARG(0));
    RET(i >= 0 ? fl[i].stat : ADXF_STAT_ERROR);
}

HLE_FN(ADXF_Tell)
{
    int i = file_index(ARG(0));
    RET(i >= 0 ? fl[i].pos : 0);
}

/* ---- set-up and servicing: nothing to drive without the IOP module */
HLE_FN(ADXPS2_ExecVint) { RET(0); }
HLE_FN(ADXPS2_SetupDvdFs) { RET(0); }
HLE_FN(ADXPS2_LoadFcacheDvd) { RET(0); }
HLE_FN(ADXPS2_SetupThrd) { RET(0); }
HLE_FN(ADXM_ExecMain) { RET(0); }
HLE_FN(ADXM_WaitVsync) { hle_wait_vsync(); RET(0); }
HLE_FN(ADXERR_EntryErrFunc) { RET(0); }

/* the drive: a PS2 DVD is always present and idle (0x14 = PS2 DVD, 10 = paused) */
HLE_FN(sceCdGetDiskTypeSafe) { RET(0x14); }
HLE_FN(sceCdStatus) { RET(10); }

/* ---- ADXT: silent players. The player object is the game's work area (the game reads fields of it). */
#define MAX_PLAYERS 16
static struct { uint32_t adxt; int stat; int vol; } pl[MAX_PLAYERS];

static int player(uint32_t adxt)
{
    for (int i = 0; i < MAX_PLAYERS; i++) if (pl[i].adxt == adxt) return i;
    return -1;
}

HLE_FN(ADXT_Init) { RET(0); }

HLE_FN(ADXT_Create)                          /* ADXT_Create(maxnch, work, worksize) -> the work area */
{
    uint32_t work = ARG(1), size = ARG(2);
    if (work && size && (work & 0x1FFFFFFFu) + size <= hle_ram_end) memset(gptr(work & 0x1FFFFFFFu, size), 0, size);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (pl[i].adxt) continue;
        pl[i].adxt = work;
        pl[i].stat = ADXT_STAT_STOP;
        pl[i].vol = 0;
        break;
    }
    RET(work);
}

HLE_FN(ADXT_SetReloadSct) { RET(0); }
HLE_FN(ADXT_SetSvrFreq) { RET(0); }
HLE_FN(ADXT_SetOutputMono) { RET(0); }
HLE_FN(ADXT_SetOutPan) { RET(0); }

HLE_FN(ADXT_StartAfs)                        /* ADXT_StartAfs(adxt, ptid, flid): silent, ends at once */
{
    int i = player(ARG(0));
    LOG("ADXT start (silent): partition %u file %u\n", ARG(1), ARG(2));
    if (i >= 0) pl[i].stat = ADXT_STAT_PLAYEND;
    RET(0);
}

HLE_FN(ADXT_StartFname)                      /* ADXT_StartFname(adxt, fname): silent, ends at once */
{
    int i = player(ARG(0));
    LOG("ADXT start (silent): %s\n", gstr(ARG(1)));
    if (i >= 0) pl[i].stat = ADXT_STAT_PLAYEND;
    RET(0);
}

HLE_FN(ADXT_Stop)
{
    int i = player(ARG(0));
    if (i >= 0) pl[i].stat = ADXT_STAT_STOP;
    RET(0);
}

HLE_FN(ADXT_Pause) { RET(0); }

HLE_FN(ADXT_SetOutVol)
{
    int i = player(ARG(0));
    if (i >= 0) pl[i].vol = (int)ARG(1);
    RET(0);
}

HLE_FN(ADXT_GetOutVol)
{
    int i = player(ARG(0));
    RET(i >= 0 ? (uint32_t)pl[i].vol : 0);
}

HLE_FN(ADXT_GetStat)
{
    int i = player(ARG(0));
    RET(i >= 0 ? (uint32_t)pl[i].stat : ADXT_STAT_STOP);
}

HLE_FN(ADXSJD_GetSfreq) { RET(48000); }
