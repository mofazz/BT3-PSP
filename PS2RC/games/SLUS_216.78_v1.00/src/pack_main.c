/* pack_main.c - BT3 (SLUS-216.78 v1.00): the game pack's side of the HLE interface (hle/core/hle.h).
 *
 * Code overlays (pack.toml [overlays]), both made resident from boot; the game copies / loads the same bytes before it
 * ever jumps in, so the translation and guest RAM agree:
 *   ov1 the kernel patches (convert/kpatch_overlay.py): code the game copies to 0x80075000 / 0x80076000 and runs as
 *       syscall handlers - the runtime's fixed window, always resident;
 *   ov2 the menu overlay BIN/DBZP.BIN (convert/dbzp_overlay.py) at 0x334C00, entered at Progress_Main (0x336A90) -
 *       the main window (BT3 has no other overlay there).
 */
#include <string.h>
#include <stdlib.h>
#include "hle.h"

rc_fn ov1_rc_lookup(uint32_t);
rc_fn ov2_rc_lookup(uint32_t);

/* boot, after the IOP: the pack's code-overlay table */
void hle_pack_boot_init(void)
{
    rc_overlay_lookup[1] = ov1_rc_lookup;
    rc_fixed_ov = 1;
    rc_fixed_lo = 0x80074FC0u;
    rc_fixed_hi = 0x80076740u;
    rc_overlay_lookup[2] = ov2_rc_lookup;
    rc_overlay_lo = 0x00334BC0u;
    rc_overlay_hi = 0x003BE71Cu;
    rc_overlay_current = 2;
}

/* end of run: the pack's report lines */
void hle_pack_report(void)
{
}

/* ---- IOP services (SIF RPC servers) of the game's modules.
 * 0x80000701: SDRDRV.IRX, Sony's sceSdRemote sound library server (the decomp's src/sys/snd.c calls it through
 * sceSdRemote). For now a SILENT stand-in: every function answers with a zeroed result (success for sceSdRemote)
 * and each distinct function number is logged once, so the calls the game makes are recorded for the real driver
 * model later. */
#define STUB_LOG_MAX 64
static struct { uint32_t srv, fno; unsigned n; } stub_seen[STUB_LOG_MAX];
static int stub_nseen;

static void stub_note(uint32_t srv, uint32_t fno, uint32_t ssize, uint32_t rsize)
{
    for (int i = 0; i < stub_nseen; i++)
        if (stub_seen[i].srv == srv && stub_seen[i].fno == fno) { stub_seen[i].n++; return; }
    if (stub_nseen < STUB_LOG_MAX) {
        stub_seen[stub_nseen].srv = srv;
        stub_seen[stub_nseen].fno = fno;
        stub_seen[stub_nseen].n = 1;
        stub_nseen++;
    }
    LOG("BT3 IOP stub: server %08x fno %u (0x%x) first call, send %u bytes, recv %u bytes\n", srv, fno, fno, ssize,
        rsize);
}

/* Heap_Create's heap top (textpatch.tsv): the game mallocs [end of the program, 0x1EFB000) at once, sized for the
   PS2's 32 MB (0x1EFB000 = RAM end - 0x105000: the stack and malloc's slack). Below 32 MB of guest RAM (the PSP
   window) the top moves down by the same amount, else that one malloc fails and the game runs with a NULL heap. */
uint32_t bt3_heap_top(uint32_t top)
{
    uint32_t cap = (hle_ram_end - 0x105000u) & ~0xFFFu;
    uint32_t r = top < cap ? top : cap;
    LOG("BT3 heap: Heap_Create top %08x -> %08x (guest RAM end %08x, heap %u KB)\n", top, r, hle_ram_end,
        (unsigned)((r - 0x3BE71Cu) >> 10));
    return r;
}

/* Stage buffer sized to the stage (textpatch.tsv, BtlLoad_StepInitial 0x127C40): the game takes 0x6CB800 bytes (the
   biggest stage file) for every battle. Here it takes the size of the stage it loads (file 0x171 + stage, 0x198 +
   stage split-screen = DATA/PZS3US1.AFS entry id - 1, rounded up to a sector) and, when the stage can change in
   battle (its MAPD info word changeKind: 1 -> stage 15, 2 -> stage 3; the change job always loads 0x171 + stage),
   at least the size of that one too. ps2rc.cfg stage_fit=0: the original size. */
#include "hle_cfg.h"
static uint32_t bt3_afs_entry(int fd, uint32_t idx, uint32_t *off)
{
    uint32_t e[2] = {0, 0};
    if (hle_disc_read_fd(fd, 8u + 8u * idx, e, 8) != 8) return 0;
    if (off) *off = e[0];
    return e[1];
}
static uint32_t bt3_stage_change_kind(int fd, uint32_t idx)
{
    uint32_t off = 0, hdr[3], mapd[6], kind = 0;
    if (!bt3_afs_entry(fd, idx, &off)) return 0;
    if (hle_disc_read_fd(fd, off, hdr, 12) != 12) return 0;            /* count, member 0, member 1 */
    if (hle_disc_read_fd(fd, off + hdr[2], mapd, 24) != 24 || mapd[0] != 0x4450414Du) return 0;   /* 'MAPD' */
    if (mapd[4] < 1 || hle_disc_read_fd(fd, off + hdr[2] + mapd[5] + 12, &kind, 4) != 4) return 0;
    return kind;
}
/* the stage a battle on `stage` can change to (15 or 3), 0xFF none: the native map reserves room for it */
uint32_t bt3_stage_change_to(uint32_t stage)
{
    uint32_t asz = 0;
    int fd = hle_disc_open("DATA\\PZS3US1.AFS", &asz);
    if (fd < 0 || stage < 1 || stage > 0x40) { if (fd >= 0) hle_disc_close(fd); return 0xFFu; }
    const uint32_t kind = bt3_stage_change_kind(fd, 0x171u + stage - 1);
    hle_disc_close(fd);
    return kind == 1 ? 15u : kind == 2 ? 3u : 0xFFu;
}
uint32_t bt3_stage_buf(uint32_t def)
{
    const char *e = hle_cfg_get("stage_fit");
    if (e && atoi(e) == 0) return def;
    uint32_t stage = gw(0x331DC8u + 28), split = gw(0x331DC8u + 36) == 1, asz = 0;
    int fd = hle_disc_open("DATA\\PZS3US1.AFS", &asz);
    if (fd < 0 || stage > 0x40) { LOG("BT3 stage buffer: stage %u, AFS not opened: %u KB\n", stage, def >> 10); return def; }
    uint32_t id = (split ? 0x198u : 0x171u) + stage, need = bt3_afs_entry(fd, id - 1, NULL);
    uint32_t kind = bt3_stage_change_kind(fd, 0x171u + stage - 1), to = kind == 1 ? 15u : kind == 2 ? 3u : 0xFFu;
    if (to != 0xFFu) {
        uint32_t n2 = bt3_afs_entry(fd, 0x171u + to - 1, NULL);
        if (n2 > need) need = n2;
    }
    hle_disc_close(fd);
    need = (need + 0x7FFu) & ~0x7FFu;
    if (!need || need > def) need = def;
    LOG("BT3 stage buffer: stage %u%s, change -> %d: %u KB instead of %u KB\n", stage, split ? " (split)" : "",
        to == 0xFFu ? -1 : (int)to, need >> 10, def >> 10);
    return need;
}

static int sdr_silent(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                      uint32_t rsize)
{
    (void)c; (void)mode; (void)send;
    stub_note(0x80000701u, fno, ssize, rsize);
    if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
    /* 0x80F0 = sceSdVoiceTransStatus (decomp snd.c: polled each frame while a bank body uploads, 1 = finished).
       Answering 0 kept every pending bank "uploading" forever: the battle loading screen never ended. The IOP is
       not timed, so the transfer (0x80D0) is over at once. */
    if (fno == 0x80F0u && recv && rsize >= 4) sw_(recv, 1);
    return 0;
}

/* 0x2000004: SOUNDS.IRX, the game's own sound-effect driver (decomp src/sys/snd.c, docs/systems/audio.md: every
   command blocks and answers in gSndRpc->reply). SILENT stand-in as above: reply 0 for every command. */
static int snd_silent(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                      uint32_t rsize)
{
    (void)c; (void)mode; (void)send;
    stub_note(0x02000004u, fno, ssize, rsize);
    if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
    return 0;
}

/* 0x80000595: libcdvd N-commands (CDVDMAN on the IOP). The game's own CD reads (its file layer, decomp src/sys/file.c)
   go through here rather than through a library function the core hooks.
     fno 1  READ: send = { lsn, sectors, buffer, mode, ... } (24 bytes) -> sectors * 2048 bytes from the disc to buffer
     fno 14 DISKREADY: reply word 2 (SCECdComplete)
   Every other function answers zero and is logged (stub_note). The read completes at once (the IOP is not timed). */
static unsigned cd_reads;

static int cd_ncmd(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                   uint32_t rsize)
{
    (void)c; (void)mode;
    if (fno == 1 && send && ssize >= 12) {
        uint32_t lsn = gw(send), nsect = gw(send + 4), buf = gw(send + 8) & 0x1FFFFFFFu;
        if (cd_reads < 8)
            LOG("BT3 CD read #%u: lsn %u, %u sectors -> %08x (raw %08x %08x %08x %08x %08x %08x)\n", cd_reads, lsn,
                nsect, buf, gw(send), gw(send + 4), gw(send + 8), gw(send + 12), gw(send + 16), gw(send + 20));
        cd_reads++;
        if (nsect == 0 || buf + nsect * 2048u > hle_ram_end || hle_disc_read_lsn(lsn, nsect, gptr(buf, nsect * 2048u))) {
            LOG("BT3 CD read: lsn %u x %u -> %08x failed (outside the disc map or guest RAM)\n", lsn, nsect, buf);
            if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
            return 0;
        }
        if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
        return 0;
    }
    if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
    if (fno == 14 && recv && rsize >= 4) {
        sw_(recv, 2);
        return 0;
    }
    stub_note(0x80000595u, fno, ssize, rsize);
    return 0;
}

/* the game's IOP-side services (SIF RPC servers): hle_rpc_register(server_id, handler) */
void hle_pack_iop_init(void)
{
    hle_rpc_register(0x80000701u, sdr_silent);
    hle_rpc_register(0x02000004u, snd_silent);
    hle_rpc_register(0x80000595u, cd_ncmd);
}

void hle_pack_iop_report(void)
{
    for (int i = 0; i < stub_nseen; i++)
        LOG("BT3 IOP stub: server %08x fno 0x%x called %u times\n", stub_seen[i].srv, stub_seen[i].fno,
            stub_seen[i].n);
}
