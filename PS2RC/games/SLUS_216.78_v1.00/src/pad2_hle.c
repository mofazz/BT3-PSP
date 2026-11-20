/* pad2_hle.c - BT3 (SLUS-216.78 v1.00): Sony libdbc / libpad2 / libvib replaced at the functions the game calls
 * (decomp src/sys/pad.c: Pad_Init, Pad_Update, Pad_GetVibProfile, Pad_SendActuator; addresses in hooks.def).
 *
 * On the PS2 these talk to DBCMAN.IRX / DS2U_D.IRX (SIF RPC server 0x80001300). Here port 0 is the platform's pad
 * (hle_plat_pad_read: the PSP's buttons and analogue stick, or the host's), port 1 is empty. The game's read layout
 * (include/sys/pad.h): data[0] = SELECT L3 R3 START UP RIGHT DOWN LEFT, data[1] = L2 R2 L1 R1 TRIANGLE CIRCLE CROSS
 * SQUARE (0 = down), data[2..5] = right X, right Y, left X, left Y - libpad's layout from its byte 2 on.
 */
#include <string.h>
#include "hle.h"

#define PAD2_STABLE 1
#define PAD2_NONE 0
#define MAX_SOCKETS 4
static int sock_port[MAX_SOCKETS];
static int nsock;

HLE_FN(sceDbcInit) { RET(1); }
HLE_FN(scePad2Init) { nsock = 0; RET(1); }

/* scePad2CreateSocket(param, work) -> socket: param = { option, port, slot, ... } (Pad2SocketParam) */
HLE_FN(scePad2CreateSocket)
{
    uint32_t param = ARG(0);
    if (nsock >= MAX_SOCKETS) { RET(-1); return; }
    int port = param ? (int)gw(param + 4) : 0;              /* Pad2SocketParam: +0 option, +4 port, +8 slot */
    sock_port[nsock] = port;
    LOG("scePad2CreateSocket(port %d) -> socket %d\n", port, nsock);
    RET(nsock++);
}

HLE_FN(scePad2GetState)                                  /* scePad2GetState(socket) */
{
    int s = (int)ARG(0);
    RET(s >= 0 && s < nsock && sock_port[s] == 0 ? PAD2_STABLE : PAD2_NONE);
}

HLE_FN(scePad2GetButtonProfile)                          /* scePad2GetButtonProfile(socket, profile) -> size */
{
    uint32_t prof = ARG(1);
    static const uint8_t all[4] = {0xFF, 0xFF, 0xFF, 0xFF};  /* every digital button + both sticks present */
    if (prof) gcopy_out(prof, all, 4);
    RET(4);
}

HLE_FN(scePad2Read)                                      /* scePad2Read(socket, data) -> size */
{
    int s = (int)ARG(0);
    uint32_t data = ARG(1);
    uint8_t d[6] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    if (s >= 0 && s < nsock && sock_port[s] == 0) {
        uint8_t st[4] = {0x80, 0x80, 0x80, 0x80};
        uint32_t held = (uint32_t)hle_plat_pad_read(st) & 0xFFFFu;   /* 1 = down, libpad bit order */
        uint32_t w = ~held & 0xFFFFu;
        d[0] = (uint8_t)w;
        d[1] = (uint8_t)(w >> 8);
        memcpy(d + 2, st, 4);
    }
    if (data) gcopy_out(data, d, 6);
    RET(6);
}

/* libvib: no motors here */
HLE_FN(sceVibGetProfile) { RET(0); }                     /* sceVibGetProfile(socket, profile) -> size 0 */
HLE_FN(sceVibSetActParam) { RET(1); }
