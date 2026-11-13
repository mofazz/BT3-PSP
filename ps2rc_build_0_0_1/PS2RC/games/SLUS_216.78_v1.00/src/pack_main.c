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
