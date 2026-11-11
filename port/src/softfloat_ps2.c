/*
 * The PS2's single-precision arithmetic for the whole game.
 *
 * Every game source of the PC build is compiled with -msoft-float: the compiler emits no float instruction at all
 * and calls these routines for each float operation, with floats travelling as 32-bit patterns in integer
 * registers. The routines implement the Emotion Engine's float unit (the model of src/port/vu0_a.c): no NaN, no
 * infinity, no denormals, results truncated, division by zero gives the largest number. So the simulation does
 * the same arithmetic on every host, and none of the host's float behaviour (rounding mode, x87, NaN handling)
 * can leak into it. Doubles (rare in the game; software IEEE on the PS2 as well) are in plat_libm.c.
 *
 * NOT verified against a console; the open points are listed in docs/systems/math.md of the decompilation.
 */
#include <stdint.h>
#include "port/vu0_a.h"

#define SIGN 0x80000000u

/*
 * Arithmetic model. PORT_FLOAT_MODEL selects it for the whole program (the vector-library reference is compiled
 * with REF_VU0_EXTERN_ARITH and takes RefVu0_AddBits / MulBits / DivBits from here):
 *
 *   PORT_FLOAT_PCSX2 (default)  what the PCSX2 emulator does with its default settings, which is what the save
 *       states used for validation come from: IEEE single precision with denormals as zero, results rounded
 *       TOWARD ZERO for add / subtract / multiply, an FPU division rounded TO NEAREST, a vector-unit division
 *       toward zero, overflow and division by zero giving +/-FLT_MAX (0x7F7FFFFF).
 *       Evidence: with pure truncation, 1.0f / 30.0f (gFade) came out one unit lower than in the save state.
 *   PORT_FLOAT_HW  the hardware hypothesis of src/port/vu0_a.c (one guard bit, no sticky bit, largest number
 *       0x7FFFFFFF): never checked against a console.
 */
#define PORT_FLOAT_PCSX2 1
#define PORT_FLOAT_HW 2
#ifndef PORT_FLOAT_MODEL
#define PORT_FLOAT_MODEL PORT_FLOAT_PCSX2
#endif

#if PORT_FLOAT_MODEL == PORT_FLOAT_PCSX2

#define FMAX 0x7F7FFFFFu
#define EXP(x) (((x) >> 23) & 0xFF)
#define MANT(x) (((x) & 0x7FFFFF) | 0x800000)

/* An operand with exponent 255 (infinity / NaN patterns) counts as the largest number. */
static uint32_t in(uint32_t a) {
    return EXP(a) == 255 ? (a & SIGN) | FMAX : a;
}

/* Packs sign, exponent and a mantissa in [2^23, 2^24); out of range gives +/-FLT_MAX or +/-0. */
static uint32_t pack(uint32_t sign, int32_t e, uint32_t m) {
    if (e >= 255) {
        return sign | FMAX;
    }
    if (e <= 0) {
        return sign;
    }
    return sign | ((uint32_t)e << 23) | (m & 0x7FFFFF);
}

/* Rounds a mantissa with 32 extra bits (and "more below" in sticky) to nearest even, renormalising. */
static uint32_t round_pack(uint32_t sign, int32_t e, uint64_t m, int sticky, int nearest) {
    uint32_t hi = (uint32_t)(m >> 32);
    uint32_t lo = (uint32_t)m;

    if (nearest && (lo & 0x80000000u) && ((lo & 0x7FFFFFFFu) || sticky || (hi & 1))) {
        hi++;
        if (hi == 0x1000000) {
            hi >>= 1;
            e++;
        }
    }
    return pack(sign, e, hi);
}

/* a + b, exact, then truncated toward zero (nearest = 0) or rounded to nearest even. */
static uint32_t add_core(uint32_t a, uint32_t b, int nearest) {
    uint64_t ma, mb;
    uint32_t sign;
    int32_t e, d;
    int sticky = 0;

    a = in(a);
    b = in(b);
    if (EXP(a) == 0 && EXP(b) == 0) {
        return a & b & SIGN;
    }
    if (EXP(b) == 0) {
        return a;
    }
    if (EXP(a) == 0) {
        return b;
    }
    if ((a & 0x7FFFFFFF) < (b & 0x7FFFFFFF)) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    sign = a & SIGN;
    e = (int32_t)EXP(a);
    d = e - (int32_t)EXP(b);
    ma = (uint64_t)MANT(a) << 32;
    mb = (uint64_t)MANT(b) << 32;
    if ((a ^ b) & SIGN) {
        /* the bits of b shifted out below 2^-32 make the exact difference smaller: one unit there, then truncate */
        uint64_t lost = d > 32;

        sticky = (int)lost;
        mb = d >= 64 ? 0 : mb >> d;
        ma -= mb + lost;
        if (ma == 0) {
            return 0; /* x - x is +0 */
        }
        while (!(ma & ((uint64_t)1 << 55))) {
            ma <<= 1;
            e--;
        }
    } else {
        sticky = d > 32;
        ma += d >= 64 ? 0 : mb >> d;
        if (ma & ((uint64_t)1 << 56)) {
            sticky |= (int)(ma & 1);
            ma >>= 1;
            e++;
        }
    }
    return round_pack(sign, e, ma, sticky, nearest);
}

/* a * b, truncated toward zero or rounded to nearest even. */
static uint32_t mul_core(uint32_t a, uint32_t b, int nearest) {
    uint32_t sign = (a ^ b) & SIGN;
    uint64_t m;
    int32_t e;

    a = in(a);
    b = in(b);
    if (EXP(a) == 0 || EXP(b) == 0) {
        return sign;
    }
    m = (uint64_t)MANT(a) * MANT(b);
    e = (int32_t)EXP(a) + (int32_t)EXP(b) - 127;
    if (m & ((uint64_t)1 << 47)) {
        e++;
        m <<= 8;  /* 24 result bits above bit 32 */
    } else {
        m <<= 9;
    }
    return round_pack(sign, e, m, 0, nearest);
}

/* Experiment switches (environment, read once): BT3_VU_NEAREST=1 rounds the vector unit's add / multiply /
   divide to nearest; BT3_FPU_NEAREST=1 does the same for the FPU's add / multiply. Default: toward zero. */
#include <stdlib.h>
static int mode(int which) {
    static const char *const name[4] = {"BT3_VU_NEAREST", "BT3_FPU_NEAREST", "BT3_VU_ADDHACK", "BT3_FPU_NOADDHACK"};
    static int m[4] = {-1, -1, -1, -1};

    if (m[which] < 0) {
        const char *e = getenv(name[which]);
        m[which] = e != NULL && e[0] == '1';
    }
    return m[which];
}
static uint32_t fpu_add(uint32_t a, uint32_t b);
uint32_t RefVu0_AddBits(uint32_t a, uint32_t b) { return mode(2) ? fpu_add(a, b) : add_core(a, b, mode(0)); }
uint32_t RefVu0_MulBits(uint32_t a, uint32_t b) { return mul_core(a, b, mode(0)); }

/* a / b; nearest = 1 rounds to nearest even (the FPU's div.s under PCSX2), 0 truncates (vdiv). */
static uint32_t divide(uint32_t a, uint32_t b, int nearest) {
    uint32_t sign = (a ^ b) & SIGN;
    uint64_t n, q, rem;
    uint32_t m;
    int32_t e;

    a = in(a);
    b = in(b);
    if (EXP(b) == 0) {
        return sign | FMAX; /* x / 0 and 0 / 0 */
    }
    if (EXP(a) == 0) {
        return sign;
    }
    n = (uint64_t)MANT(a) << 26;
    q = n / MANT(b);
    rem = n % MANT(b);
    e = (int32_t)EXP(a) - (int32_t)EXP(b) + 127;
    if (q >= ((uint64_t)1 << 26)) {
        rem |= q & 1;
        q >>= 1;
    } else {
        e--;
    }
    rem |= q & 1; /* q is now in [2^25, 2^26): one more bit goes to the sticky bits */
    q >>= 1;
    m = (uint32_t)(q >> 1); /* 24 bits; q & 1 is the round bit, rem the sticky bits */
    if (nearest && (q & 1) && (rem != 0 || (m & 1))) {
        m++;
        if (m == 0x1000000) {
            m >>= 1;
            e++;
        }
    }
    return pack(sign, e, m);
}

uint32_t RefVu0_DivBits(uint32_t a, uint32_t b) { return divide(a, b, mode(0)); }
uint32_t __divsf3(uint32_t a, uint32_t b) { return divide(a, b, 1); }

#else /* PORT_FLOAT_HW: the primitives of src/port/vu0_a.c (compile it without REF_VU0_EXTERN_ARITH) */

uint32_t __divsf3(uint32_t a, uint32_t b) { return RefVu0_DivBits(a, b); }

#endif

#if PORT_FLOAT_MODEL == PORT_FLOAT_PCSX2
/*
 * The FPU's add.s / sub.s. The PS2 adder keeps only ONE bit of the smaller operand below the larger operand's
 * last place and no sticky bits; PCSX2 reproduces this (its FPU_ADD_SUB step) by clearing the smaller operand's
 * low bits before an ordinary truncating add: exponent difference d of 1..24 clears d - 1 bits, 25 or more
 * leaves only the sign. (The vector unit's adds do not get this treatment there.)
 */
static uint32_t fpu_add(uint32_t a, uint32_t b) {
    int32_t d = (int32_t)EXP(a) - (int32_t)EXP(b);

    if (mode(3)) {
        return add_core(a, b, mode(1));
    }
    if (d >= 25) {
        b &= SIGN;
    } else if (d > 0) {
        b &= 0xFFFFFFFFu << (d - 1);
    } else if (d <= -25) {
        a &= SIGN;
    } else if (d < 0) {
        a &= 0xFFFFFFFFu << (-d - 1);
    }
    return add_core(a, b, mode(1));
}
uint32_t __addsf3(uint32_t a, uint32_t b) { return fpu_add(a, b); }
uint32_t __subsf3(uint32_t a, uint32_t b) { return fpu_add(a, b ^ SIGN); }
uint32_t __mulsf3(uint32_t a, uint32_t b) { return mul_core(a, b, mode(1)); }
#else
uint32_t __addsf3(uint32_t a, uint32_t b) { return RefVu0_AddBits(a, b); }
uint32_t __subsf3(uint32_t a, uint32_t b) { return RefVu0_AddBits(a, b ^ SIGN); }
uint32_t __mulsf3(uint32_t a, uint32_t b) { return RefVu0_MulBits(a, b); }
#endif
uint32_t __negsf2(uint32_t a) { return a ^ SIGN; }

/* Comparisons: every pattern is an ordinary number (c.eq.s / c.lt.s / c.le.s never see "unordered"). */
static int cmp(uint32_t a, uint32_t b) {
    return RefVu0_LtBits(a, b) ? -1 : RefVu0_LtBits(b, a) ? 1 : 0;
}
int __eqsf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __nesf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __ltsf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __lesf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __gtsf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __gesf2(uint32_t a, uint32_t b) { return cmp(a, b); }
int __unordsf2(uint32_t a, uint32_t b) { (void)a; (void)b; return 0; }

/* cvt.w.s: truncates; out of range gives the largest / smallest integer. */
int32_t __fixsfsi(uint32_t a) {
    int32_t e = (int32_t)((a >> 23) & 0xFF) - 127;
    uint32_t m = (a & 0x7FFFFF) | 0x800000;
    uint32_t v;

    if (e < 0) {
        return 0;
    }
    if (e >= 31) {
        return (a & SIGN) ? (int32_t)0x80000000u : 0x7FFFFFFF;
    }
    v = e >= 23 ? m << (e - 23) : m >> (23 - e);
    return (a & SIGN) ? -(int32_t)v : (int32_t)v;
}

/* The PS2 compiler's float -> unsigned: values of 2^31 and above go through (x - 2^31) and get the top bit. */
uint32_t __fixunssfsi(uint32_t a) {
    if (!RefVu0_LtBits(a, 0x4F000000u)) {
        return (uint32_t)__fixsfsi(RefVu0_SubBits(a, 0x4F000000u)) | SIGN;
    }
    return (uint32_t)__fixsfsi(a);
}

/* cvt.s.w: truncates to 24 bits. */
uint32_t __floatsisf(int32_t v) { return RefVu0_ItofBits(v, 0); }

/* unsigned -> float as the PS2 compiler does it: convert as signed, add 2^32 when the top bit was set. */
uint32_t __floatunsisf(uint32_t v) {
    uint32_t f = RefVu0_ItofBits((int32_t)v, 0);

    return (v & SIGN) ? RefVu0_AddBits(f, 0x4F800000u) : f;
}
