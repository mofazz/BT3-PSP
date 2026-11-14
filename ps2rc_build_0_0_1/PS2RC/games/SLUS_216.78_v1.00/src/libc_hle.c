/* libc_hle.c - BT3 (SLUS-216.78 v1.00): native versions of the game's hottest newlib leaf functions.
 *
 * The session logs' back-edge samples put the game thread inside memcmp (0x2A9988) for 80-99 % of all loop
 * iterations, in menus and in battle, with strcmp (0x2AA3B4) next; the stage and fighter code calls the software
 * square root __ieee754_sqrtf (a bit-by-bit loop) and the newlib sine / cosine / arctangent. Translated exactly (v0),
 * every byte of a memcmp is several context loads and stores. Here they are plain C on host memory.
 *
 * Rules (the same as the core's memcpy / memset hooks in hle/core/hle_sce.c):
 *  - only when every byte is plain guest RAM (or a KSEG / uncached mirror of it); anything else (scratchpad,
 *    hardware, a range past the window) runs the translated original (GUEST_FN);
 *  - same results as newlib: memcmp / strcmp return the difference of the first differing bytes as unsigned chars;
 *    strcpy / memmove return dst; strlen the length;
 *  - virtual time is charged about like the EE loops (it paces the game's own clock).
 * Math: inputs with the R5900's "exponent 255" bit patterns are read as +-FLT_MAX (the R5900 has no Inf / NaN),
 * results that would be Inf / NaN on the Allegrex are clamped the same way. sqrt of a negative number is 0.
 * ps2rc.cfg libc_hle=0 turns all of this off (every call runs the translated original). */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"

/* the translated originals (the prepare step renames each hooked function to fn_<addr>__guest) */
void GUEST_FN(memcmp)(R5900Ctx *c);
void GUEST_FN(strcmp)(R5900Ctx *c);
void GUEST_FN(strlen)(R5900Ctx *c);
void GUEST_FN(strcpy)(R5900Ctx *c);
void GUEST_FN(memmove)(R5900Ctx *c);
void GUEST_FN(sqrtf)(R5900Ctx *c);
void GUEST_FN(__ieee754_sqrtf)(R5900Ctx *c);
void GUEST_FN(sinf)(R5900Ctx *c);
void GUEST_FN(cosf)(R5900Ctx *c);
void GUEST_FN(atanf)(R5900Ctx *c);
void GUEST_FN(acosf)(R5900Ctx *c);
void GUEST_FN(asinf)(R5900Ctx *c);
void GUEST_FN(atan2f)(R5900Ctx *c);

static int lh_on = -1;
static int lh_enabled(void)
{
    if (lh_on < 0) {
        const char *e = hle_cfg_get("libc_hle");      /* ps2rc.cfg libc_hle=0 */
        lh_on = !(e && atoi(e) == 0);
        LOG("libc_hle: native memcmp / strcmp / strlen / strcpy / memmove / sqrtf / sinf / cosf / atan2f %s\n",
            lh_on ? "on" : "OFF (ps2rc.cfg libc_hle=0)");
    }
    return lh_on;
}

static uint8_t *lh_ram(uint32_t a, uint32_t n)       /* host pointer to n plain RAM bytes at guest a, or NULL */
{
    uint32_t seg = a >> 28;
    if (seg == 2 || seg == 3 || seg == 8 || seg == 9 || seg == 0xA) a &= 0x0FFFFFFFu;
    else if (seg != 0) return NULL;
    if (a < 0x4000u) return NULL;                    /* low 16 KB: leave to the original (scratchpad aliasing) */
    if (a >= hle_ram_end || n > hle_ram_end - a) return NULL;
    return hle_ram + a;
}

/* bytes from a up to the window end (for the string functions' bounded scans) */
static uint32_t lh_room(uint32_t a)
{
    uint32_t seg = a >> 28;
    if (seg == 2 || seg == 3 || seg == 8 || seg == 9 || seg == 0xA) a &= 0x0FFFFFFFu;
    else if (seg != 0) return 0;
    if (a < 0x4000u || a >= hle_ram_end) return 0;
    return hle_ram_end - a;
}

uint64_t lh_st_memcmp, lh_st_strcmp, lh_st_strlen, lh_st_strcpy, lh_st_memmove, lh_st_math;

HLE_FN(memcmp)                                       /* memcmp(a, b, n) -> <0 / 0 / >0 */
{
    uint32_t a = ARG(0), b = ARG(1), n = ARG(2);
    if (!n) { RET(0); return; }
    const uint8_t *ha, *hb;
    if (!lh_enabled() || !(ha = lh_ram(a, n)) || !(hb = lh_ram(b, n))) { GUEST_FN(memcmp)(c); return; }
    int r = 0;
    for (uint32_t i = 0; i < n; i++)
        if (ha[i] != hb[i]) { r = (int)ha[i] - (int)hb[i]; n = i + 1; break; }
    lh_st_memcmp++;
    hle_time_charge(16 + n / 2);
    RET((uint32_t)r);
}

HLE_FN(strcmp)                                       /* strcmp(a, b) */
{
    uint32_t a = ARG(0), b = ARG(1);
    uint32_t ra = lh_room(a), rb = lh_room(b), lim = ra < rb ? ra : rb;
    if (!lh_enabled() || !lim) { GUEST_FN(strcmp)(c); return; }
    const uint8_t *ha = hle_ram + (a & 0x0FFFFFFFu), *hb = hle_ram + (b & 0x0FFFFFFFu);
    uint32_t i = 0;
    while (i < lim && ha[i] == hb[i] && ha[i]) i++;
    if (i == lim) { GUEST_FN(strcmp)(c); return; }   /* ran into the window end: let the original decide */
    lh_st_strcmp++;
    hle_time_charge(16 + i);
    RET((uint32_t)((int)ha[i] - (int)hb[i]));
}

HLE_FN(strlen)                                       /* strlen(s) */
{
    uint32_t s = ARG(0), lim = lh_room(s);
    if (!lh_enabled() || !lim) { GUEST_FN(strlen)(c); return; }
    const uint8_t *h = hle_ram + (s & 0x0FFFFFFFu);
    const uint8_t *z = memchr(h, 0, lim);
    if (!z) { GUEST_FN(strlen)(c); return; }
    uint32_t n = (uint32_t)(z - h);
    lh_st_strlen++;
    hle_time_charge(16 + n / 2);
    RET(n);
}

HLE_FN(strcpy)                                       /* strcpy(dst, src) -> dst */
{
    uint32_t d = ARG(0), s = ARG(1), lim = lh_room(s);
    if (!lh_enabled() || !lim) { GUEST_FN(strcpy)(c); return; }
    const uint8_t *hs = hle_ram + (s & 0x0FFFFFFFu);
    const uint8_t *z = memchr(hs, 0, lim);
    uint8_t *hd;
    if (!z || !(hd = lh_ram(d, (uint32_t)(z - hs) + 1))) { GUEST_FN(strcpy)(c); return; }
    uint32_t n = (uint32_t)(z - hs) + 1;
    memmove(hd, hs, n);
    lh_st_strcpy++;
    hle_time_charge(16 + n / 2);
    RET(d);
}

HLE_FN(memmove)                                      /* memmove(dst, src, n) -> dst */
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    if (!n) { RET(d); return; }
    uint8_t *hd, *hs;
    if (!lh_enabled() || !(hd = lh_ram(d, n)) || !(hs = lh_ram(s, n))) { GUEST_FN(memmove)(c); return; }
    memmove(hd, hs, n);
    lh_st_memmove++;
    hle_time_charge(16 + n / 4);
    RET(d);
}

/* ---- math: R5900 float conventions in and out */
static float lh_in(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    if ((u & 0x7F800000u) == 0x7F800000u) return (u >> 31) ? -FLT_MAX : FLT_MAX;   /* no Inf / NaN on the EE */
    if ((u & 0x7F800000u) == 0) return (u >> 31) ? -0.0f : 0.0f;                  /* denormals are zero */
    return x;
}
static float lh_out(float x)
{
    if (isnan(x)) return 0.0f;
    if (isinf(x)) return x > 0 ? FLT_MAX : -FLT_MAX;
    if (x != 0.0f && fabsf(x) < FLT_MIN) return 0.0f;
    return x;
}
#define MATH1(name, expr)                                                       \
    HLE_FN(name)                                                                \
    {                                                                           \
        if (!lh_enabled()) { GUEST_FN(name)(c); return; }                       \
        float x = lh_in(FARG(0));                                               \
        lh_st_math++;                                                           \
        hle_time_charge(60);                                                    \
        FRET(lh_out(expr));                                                     \
    }

MATH1(sqrtf, x > 0.0f ? sqrtf(x) : 0.0f)
MATH1(__ieee754_sqrtf, x > 0.0f ? sqrtf(x) : 0.0f)
MATH1(sinf, sinf(x))
MATH1(cosf, cosf(x))
MATH1(atanf, atanf(x))
MATH1(acosf, acosf(x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x))
MATH1(asinf, asinf(x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x))

HLE_FN(atan2f)                                       /* atan2f(y, x): y in f12, x in f13 */
{
    if (!lh_enabled()) { GUEST_FN(atan2f)(c); return; }
    float y = lh_in(FARG(0)), x = lh_in(FARG(1));
    lh_st_math++;
    hle_time_charge(80);
    FRET(lh_out(atan2f(y, x)));
}
