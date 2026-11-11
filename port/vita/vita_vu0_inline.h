/*
 * Forced in front of the vector library (src/port/vu0_a.c, vu0_b.c) in the Vita build.
 *
 * The first measurements on a Vita (2026-10-06) showed the fight at 41% of the console's speed with a call per
 * float operation and only 46% with the game's own float code inline: the time is in the vector library. Every
 * component of every vector operation went through three nested calls (f_add -> RefVu0_Add -> RefVu0_AddBits ->
 * the float unit) plus real memcpy calls for the bit casts, about 160,000 times per vertical blank.
 *
 * Here the three primitives are inline: the same arithmetic as vita_float.c's hardware model (the vector unit's
 * truncating add / multiply / divide on a float unit set to round toward zero; see there for what was checked),
 * with the same fall-back to the exact routine for operands that look like infinity or NaN. build.py compiles
 * the two files with this header and renames the library's names for the primitives to these.
 */
#ifndef VITA_VU0_INLINE_H
#define VITA_VU0_INLINE_H
#include <stdint.h>

extern int gPortFastFloat;
extern uint32_t PortX_VuAdd(uint32_t a, uint32_t b), PortX_VuMul(uint32_t a, uint32_t b), PortX_VuDiv(uint32_t a, uint32_t b);

typedef union { float f; uint32_t u; } VitaVuBits;
#define VV_INLINE static inline __attribute__((always_inline, unused))
#define VV_EXACT 0
#define VV_SLOW(a, b) __builtin_expect(VV_EXACT | (((a) & 0x7F800000u) == 0x7F800000u) | (((b) & 0x7F800000u) == 0x7F800000u), 0)

VV_INLINE uint32_t VitaVu_Out(uint32_t r) {
    return (r & 0x7F800000u) == 0x7F800000u ? (r & 0x80000000u) | 0x7F7FFFFFu : r;
}

VV_INLINE uint32_t VitaVu_AddBits(uint32_t a, uint32_t b) {
    VitaVuBits x, y;

    if (VV_SLOW(a, b)) {
        return PortX_VuAdd(a, b);
    }
    x.u = a;
    y.u = b;
    x.f = x.f + y.f;
    return VitaVu_Out(x.u);
}

VV_INLINE uint32_t VitaVu_MulBits(uint32_t a, uint32_t b) {
    VitaVuBits x, y;

    if (VV_SLOW(a, b)) {
        return PortX_VuMul(a, b);
    }
    x.u = a;
    y.u = b;
    x.f = x.f * y.f;
    return VitaVu_Out(x.u);
}

VV_INLINE uint32_t VitaVu_DivBits(uint32_t a, uint32_t b) {
    VitaVuBits x, y;

    if (VV_SLOW(a, b)) {
        return PortX_VuDiv(a, b);
    }
    if ((b & 0x7F800000u) == 0) {
        return ((a ^ b) & 0x80000000u) | 0x7F7FFFFFu; /* x / 0 and 0 / 0 */
    }
    if ((a & 0x7F800000u) == 0) {
        return (a ^ b) & 0x80000000u;
    }
    x.u = a;
    y.u = b;
    x.f = x.f / y.f;
    return VitaVu_Out(x.u);
}

/* The float forms vu0_b.c uses (RefVu0_Add ...; vu0_a.c defines its own from the three above). */
VV_INLINE float VitaVuF_Add(float a, float b) { VitaVuBits x, y; x.f = a; y.f = b; x.u = VitaVu_AddBits(x.u, y.u); return x.f; }
VV_INLINE float VitaVuF_Sub(float a, float b) { VitaVuBits x, y; x.f = a; y.f = b; x.u = VitaVu_AddBits(x.u, y.u ^ 0x80000000u); return x.f; }
VV_INLINE float VitaVuF_Mul(float a, float b) { VitaVuBits x, y; x.f = a; y.f = b; x.u = VitaVu_MulBits(x.u, y.u); return x.f; }
VV_INLINE float VitaVuF_Div(float a, float b) { VitaVuBits x, y; x.f = a; y.f = b; x.u = VitaVu_DivBits(x.u, y.u); return x.f; }
#endif
