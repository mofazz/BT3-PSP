/* nat_libm.h - native sin / acos / sqrt of the game's math library (nat_libm.c) */
#ifndef NAT_LIBM_H
#define NAT_LIBM_H
#include <stdint.h>
uint32_t nat_sinf_bits(uint32_t x, int *ok);     /* 0x28F598; *ok = 0: run the translated body */
uint32_t nat_acosf_bits(uint32_t x, int *ok);    /* 0x11F780 (clamp) + 0x2909F8 */
uint32_t nat_sqrtf_bits(uint32_t x);             /* 0x291F30 */
#endif
