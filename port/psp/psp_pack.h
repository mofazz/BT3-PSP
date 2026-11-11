/* bt3psp.tex, the textures converted on the PC (port/tools/bt3_psp_tex.py, crear_carpeta.html): index entry. */
#ifndef PSP_PACK_H
#define PSP_PACK_H
#include <stdint.h>

typedef struct {
    uint64_t key;          /* FNV-1a 64 of the PS2 pixel packet (TexEntry.pix, pixSize bytes) */
    uint32_t off, bytes;   /* from the data offset */
    uint16_t w, h;         /* stored size (padded, powers of two) */
    uint16_t tw, th;       /* the PS2 texture's size */
    uint8_t fmt, swz, shift, pad; /* fmt 0 = 8888, 1 = T8, 2 = T4, 3 = T8 + own palette; shift: halvings */
    uint32_t zero;
} PackEnt;

#define PACK_MARK 0x7E7E0001 /* the port's GS marker (gs_marker.c): arg = index, "the upload just done is this one" */

extern PackEnt *gPspPack;
extern int gPspPackN;
extern uint32_t gPspPackData;
extern int gPspPackFd;
#endif
