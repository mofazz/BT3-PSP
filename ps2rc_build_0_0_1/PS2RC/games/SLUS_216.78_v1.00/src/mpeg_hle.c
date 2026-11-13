/* mpeg_hle.c - BT3 (SLUS-216.78 v1.00): Sony libmpeg, stopgap. The core has no IPU (MPEG-2 decoder) model, so the first
 * sceMpegGetPicture of the opening movie (Movie_Run, src/sys/movie.c of the decomp) polls IPU_CMD forever. Until an IPU
 * decoder exists, the stream reports its end at once: Movie_Run leaves its loop on the first sceMpegIsEnd, stops the
 * ADX track, fades and closes (exactly the path of a START skip). Movies affected: zs3usop.pss (opening), zs3used.pss
 * (ending). Remove these two hooks from hooks.def when the IPU is modelled. */
#include "hle.h"

static unsigned movies;

HLE_FN(sceMpegIsEnd)                                     /* sceMpegIsEnd(mp) -> 1: movie skipped */
{
    if (!movies++) LOG("mpeg_hle: no IPU model - movie skipped (sceMpegIsEnd -> 1)\n");
    RET(1);
}

HLE_FN(sceMpegGetPicture) { RET(0); }                    /* sceMpegGetPicture(mp, rgb, mbcount): no picture */
