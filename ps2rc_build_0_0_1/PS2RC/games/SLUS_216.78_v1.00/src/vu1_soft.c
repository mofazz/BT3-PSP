/* vu1_soft.c - BT3 (SLUS-216.78 v1.00), HOST ONLY: an interpreter of the PS2 VU1 in micro mode, so the game's own VU1
 * microprograms (uploaded by VIF1 MPG, run by MSCAL / MSCNT) turn the model data into GS packets (XGKICK -> gs_soft.c).
 * Written for this pack from the public description of the VU (instruction formats, register files, the I / E bits,
 * branch delay slots, Q and clip-flag latency). The opcode tables were checked against the 1,928 instruction pairs of
 * BT3's nine microprograms and the decomp's listings (C:\bt3ps2rc\vu1_decode_check.py: 0 mismatches).
 *
 * Model: one instruction pair per cycle; an FMAC result is ready 4 cycles after issue and a read of a VF register that
 * is not ready stalls (advances the cycle); Q is written 7 cycles (13 for RSQRT) after DIV / SQRT, WAITQ waits for it;
 * a CLIP's flags reach the clip flag register 4 cycles after issue (FCAND / FCOR / FCEQ / FCGET read what has arrived);
 * the I register loaded by an [I] pair is seen from the next pair on; the upper and lower halves of a pair read the
 * registers as they were before the pair, and when both write one register the upper result is kept. Not modelled:
 * MAC / status flags, EFU (P), XGKICK timing (a kick is drawn at once), integer branch hazards. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "hle.h"

uint8_t vu1_mem[16384];                              /* data memory, 1024 quadwords */
static uint32_t micro[4096];                         /* micro memory, 2048 pairs: [2k] lower, [2k+1] upper */

typedef union { float f[4]; uint32_t u[4]; } V4;
static V4 vf[32], acc;
static uint16_t vi[16];
static uint32_t q_cur = 0x3F800000u, q_new, i_reg, p_reg;
static uint64_t cycle, q_ready, vf_ready[33];
static uint32_t cf;                                  /* clip flags, 24 bits */
static struct { uint64_t t; uint32_t f; } clipq[16];
static unsigned clip_head, clip_tail;
static uint32_t cont_pc;                             /* where MSCNT continues (byte address) */
uint64_t vu1_stat_runs, vu1_stat_pairs, vu1_stat_kicks, vu1_stat_bad;

void gs_path1_kick(const uint8_t *mem, uint32_t qaddr);   /* gs_soft.c */

static float uf(uint32_t u)                          /* VU float in: no Inf / NaN / denormals */
{
    uint32_t e = (u >> 23) & 0xFF;
    if (e == 0xFF) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    else if (e == 0) u &= 0x80000000u;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t fu(float f)                          /* result: clamped to the largest finite value */
{
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t e = (u >> 23) & 0xFF;
    if (e == 0xFF) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    else if (e == 0) u &= 0x80000000u;
    return u;
}

void vu1_mpg(uint32_t pair, const uint32_t *w, uint32_t npairs)
{
    for (uint32_t k = 0; k < npairs; k++) {
        uint32_t a = ((pair + k) & 2047) * 2;
        micro[a] = w[2 * k];
        micro[a + 1] = w[2 * k + 1];
    }
}

static void stall_for(unsigned r)
{
    if (r && r < 33 && vf_ready[r] > cycle) cycle = vf_ready[r];
}

static void commit_pending(void)
{
    if (q_ready && cycle >= q_ready) { q_cur = q_new; q_ready = 0; }
    while (clip_head != clip_tail && clipq[clip_head & 15].t <= cycle) {
        cf = ((cf << 6) | clipq[clip_head & 15].f) & 0xFFFFFFu;
        clip_head++;
    }
}

static V4 *mem(uint32_t qa) { return (V4 *)(vu1_mem + 16 * (qa & 0x3FF)); }

/* ---------------------------------------------------------------- upper */
typedef struct { int dst; unsigned mask; V4 v; } UpRes;   /* dst 0..31 VF, 32 ACC, -1 nothing */

static void upper(uint32_t w, UpRes *r)
{
    unsigned dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    unsigned op = w & 0x3F;
    V4 a = vf[fs], b = vf[ft];
    float bs = 0;
    int bmode = 0;                                   /* 0: b lanes, 1: scalar bs */
    int kind = -1;                                   /* 0 add 1 sub 2 madd 3 msub 4 max 5 mini 6 mul */
    int to_acc = 0;
    r->dst = -1;
    r->mask = dest;
    if (op < 0x3C) {
        if (op < 0x1C) { kind = op >> 2; bmode = 1; bs = uf(b.u[op & 3]); }
        else switch (op) {
        case 0x1C: kind = 6; bmode = 1; bs = uf(q_cur); break;
        case 0x1D: kind = 4; bmode = 1; bs = uf(i_reg); break;
        case 0x1E: kind = 6; bmode = 1; bs = uf(i_reg); break;
        case 0x1F: kind = 5; bmode = 1; bs = uf(i_reg); break;
        case 0x20: kind = 0; bmode = 1; bs = uf(q_cur); break;
        case 0x21: kind = 2; bmode = 1; bs = uf(q_cur); break;
        case 0x22: kind = 0; bmode = 1; bs = uf(i_reg); break;
        case 0x23: kind = 2; bmode = 1; bs = uf(i_reg); break;
        case 0x24: kind = 1; bmode = 1; bs = uf(q_cur); break;
        case 0x25: kind = 3; bmode = 1; bs = uf(q_cur); break;
        case 0x26: kind = 1; bmode = 1; bs = uf(i_reg); break;
        case 0x27: kind = 3; bmode = 1; bs = uf(i_reg); break;
        case 0x28: kind = 0; break;
        case 0x29: kind = 2; break;
        case 0x2A: kind = 6; break;
        case 0x2B: kind = 4; break;
        case 0x2C: kind = 1; break;
        case 0x2D: kind = 3; break;
        case 0x2E:                                   /* OPMSUB fd.xyz = ACC - fs.yzx * ft.zxy */
            r->dst = (int)fd;
            r->mask = dest & 0xE;
            for (int i = 0; i < 3; i++)
                r->v.u[i] = fu(uf(acc.u[i]) - uf(a.u[(i + 1) % 3]) * uf(b.u[(i + 2) % 3]));
            return;
        case 0x2F: kind = 5; break;
        default: vu1_stat_bad++; return;
        }
    } else {
        unsigned sp = ((w >> 6) & 31) << 2 | (w & 3);
        to_acc = 1;
        if (sp < 0x10) { kind = (sp >> 2) == 0 ? 0 : (sp >> 2) == 1 ? 1 : (sp >> 2) == 2 ? 2 : 3; bmode = 1; bs = uf(b.u[sp & 3]); }
        else if (sp >= 0x18 && sp < 0x1C) { kind = 6; bmode = 1; bs = uf(b.u[sp & 3]); }
        else switch (sp) {
        case 0x10: case 0x11: case 0x12: case 0x13: {          /* ITOF0/4/12/15 */
            static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
            r->dst = (int)ft;
            for (int i = 0; i < 4; i++) r->v.u[i] = fu((float)(int32_t)a.u[i] * sc[sp & 3]);
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {          /* FTOI0/4/12/15 */
            static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            r->dst = (int)ft;
            for (int i = 0; i < 4; i++) {
                float f = uf(a.u[i]) * sc[sp & 3];
                r->v.u[i] = f >= 2147483647.0f ? 0x7FFFFFFFu : f <= -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;
            }
            return;
        }
        case 0x1C: kind = 6; bmode = 1; bs = uf(q_cur); break;  /* MULAq */
        case 0x1D:                                              /* ABS */
            r->dst = (int)ft;
            for (int i = 0; i < 4; i++) r->v.u[i] = fu(fabsf(uf(a.u[i])));
            return;
        case 0x1E: kind = 6; bmode = 1; bs = uf(i_reg); break;  /* MULAi */
        case 0x1F: {                                            /* CLIP fs.xyz, ft.w */
            float wv = fabsf(uf(b.u[3]));
            uint32_t f = 0;
            for (int i = 0; i < 3; i++) {
                float x = uf(a.u[i]);
                if (x > wv) f |= 1u << (2 * i);
                if (x < -wv) f |= 2u << (2 * i);
            }
            clipq[clip_tail & 15].t = cycle + 4;
            clipq[clip_tail & 15].f = f;
            clip_tail++;
            return;
        }
        case 0x20: kind = 0; bmode = 1; bs = uf(q_cur); break;
        case 0x21: kind = 2; bmode = 1; bs = uf(q_cur); break;
        case 0x22: kind = 0; bmode = 1; bs = uf(i_reg); break;
        case 0x23: kind = 2; bmode = 1; bs = uf(i_reg); break;
        case 0x24: kind = 1; bmode = 1; bs = uf(q_cur); break;
        case 0x25: kind = 3; bmode = 1; bs = uf(q_cur); break;
        case 0x26: kind = 1; bmode = 1; bs = uf(i_reg); break;
        case 0x27: kind = 3; bmode = 1; bs = uf(i_reg); break;
        case 0x28: kind = 0; break;
        case 0x29: kind = 2; break;
        case 0x2A: kind = 6; break;
        case 0x2C: kind = 1; break;
        case 0x2D: kind = 3; break;
        case 0x2E:                                              /* OPMULA ACC.xyz = fs.yzx * ft.zxy */
            r->dst = 32;
            r->mask = dest & 0xE;
            for (int i = 0; i < 3; i++) r->v.u[i] = fu(uf(a.u[(i + 1) % 3]) * uf(b.u[(i + 2) % 3]));
            return;
        case 0x2F: return;                                      /* NOP */
        default: vu1_stat_bad++; return;
        }
    }
    r->dst = to_acc ? 32 : (int)fd;
    for (int i = 0; i < 4; i++) {
        float x = uf(a.u[i]), y = bmode ? bs : uf(b.u[i]), o;
        switch (kind) {
        case 0: o = x + y; break;
        case 1: o = x - y; break;
        case 2: o = uf(acc.u[i]) + x * y; break;
        case 3: o = uf(acc.u[i]) - x * y; break;
        case 4: o = x > y ? x : y; break;
        case 5: o = x < y ? x : y; break;
        default: o = x * y; break;
        }
        r->v.u[i] = fu(o);
    }
}

static unsigned upper_srcs(uint32_t w, unsigned *s1, unsigned *s2)
{
    *s1 = (w >> 11) & 31;
    *s2 = (w >> 16) & 31;
    return 1;
}

/* ---------------------------------------------------------------- lower */
static int16_t sx11(uint32_t w) { return (int16_t)((int16_t)(w << 5) >> 5); }

static void lane_store(V4 *m, const V4 *v, unsigned dest)
{
    for (int i = 0; i < 4; i++)
        if (dest & (8u >> i)) m->u[i] = v->u[i];
}

static void lane_load(int t, const V4 *m, unsigned dest)
{
    if (!t) return;
    for (int i = 0; i < 4; i++)
        if (dest & (8u >> i)) vf[t].u[i] = m->u[i];
}

static void set_vi(unsigned r, uint32_t v) { if (r) vi[r] = (uint16_t)v; }

/* returns 1 and *target when the instruction branches (taken) */
static int lower(uint32_t w, uint32_t pc, uint32_t *target)
{
    unsigned op = w >> 25, dest = (w >> 21) & 15, it = (w >> 16) & 31, is = (w >> 11) & 31;
    unsigned ft = it, fs = is;
    int16_t imm = sx11(w);
    it &= 15; is &= 15;
    switch (op) {
    case 0x00: lane_load((int)ft, mem((uint32_t)(vi[is] + imm)), dest); return 0;                 /* LQ */
    case 0x01: { V4 v = vf[fs]; lane_store(mem((uint32_t)(vi[it] + imm)), &v, dest); return 0; }  /* SQ */
    case 0x04: { V4 *m = mem((uint32_t)(vi[is] + imm));                                         /* ILW */
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) { set_vi(it, m->u[i]); break; } return 0; }
    case 0x05: { V4 *m = mem((uint32_t)(vi[is] + imm));                                         /* ISW */
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) m->u[i] = vi[it]; return 0; }
    case 0x08: set_vi(it, vi[is] + (((w >> 10) & 0x7800) | (w & 0x7FF))); return 0;            /* IADDIU */
    case 0x09: set_vi(it, vi[is] - (((w >> 10) & 0x7800) | (w & 0x7FF))); return 0;            /* ISUBIU */
    case 0x10: set_vi(1, (cf & 0xFFFFFF) == (w & 0xFFFFFF)); return 0;                          /* FCEQ */
    case 0x11: cf = w & 0xFFFFFF; clip_head = clip_tail; return 0;                              /* FCSET */
    case 0x12: set_vi(1, (cf & w & 0xFFFFFF) != 0); return 0;                                   /* FCAND */
    case 0x13: set_vi(1, ((cf | w) & 0xFFFFFF) == 0xFFFFFF); return 0;                          /* FCOR */
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B:
        if (op == 0x16 || op == 0x1A || op == 0x14 || op == 0x18) set_vi(it, 0);                 /* status/MAC: 0 */
        return 0;
    case 0x1C: set_vi(it, cf & 0xFFF); return 0;                                                /* FCGET */
    case 0x20: *target = pc + 8 + 8 * (int32_t)imm; return 1;                                    /* B */
    case 0x21: set_vi(it, (pc + 16) / 8); *target = pc + 8 + 8 * (int32_t)imm; return 1;         /* BAL */
    case 0x24: *target = (uint32_t)vi[is] * 8; return 1;                                         /* JR */
    case 0x25: { uint32_t t = (uint32_t)vi[is] * 8; set_vi(it, (pc + 16) / 8); *target = t; return 1; }   /* JALR */
    case 0x28: if (vi[it] == vi[is]) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x29: if (vi[it] != vi[is]) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x2C: if ((int16_t)vi[is] < 0) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x2D: if ((int16_t)vi[is] > 0) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x2E: if ((int16_t)vi[is] <= 0) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x2F: if ((int16_t)vi[is] >= 0) { *target = pc + 8 + 8 * (int32_t)imm; return 1; } return 0;
    case 0x40: break;
    default: vu1_stat_bad++; return 0;
    }
    unsigned f = w & 0x3F, id = (w >> 6) & 15;
    if (f < 0x3C) {
        switch (f) {
        case 0x30: set_vi(id, vi[is] + vi[it]); return 0;
        case 0x31: set_vi(id, vi[is] - vi[it]); return 0;
        case 0x32: set_vi(it, vi[is] + (int16_t)((int16_t)(((w >> 6) & 31) << 11) >> 11)); return 0;   /* IADDI imm5 */
        case 0x34: set_vi(id, vi[is] & vi[it]); return 0;
        case 0x35: set_vi(id, vi[is] | vi[it]); return 0;
        default: vu1_stat_bad++; return 0;
        }
    }
    unsigned sp = ((w >> 6) & 31) << 2 | (w & 3);
    unsigned fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    switch (sp) {
    case 0x30: { V4 v = vf[fs]; lane_load((int)ft, &v, dest); return 0; }                        /* MOVE */
    case 0x31: { V4 s = vf[fs], v; v.u[0] = s.u[1]; v.u[1] = s.u[2]; v.u[2] = s.u[3]; v.u[3] = s.u[0];
                 lane_load((int)ft, &v, dest); return 0; }                                        /* MR32 */
    case 0x34: lane_load((int)ft, mem(vi[is]), dest); set_vi(is, vi[is] + 1); return 0;          /* LQI */
    case 0x35: { V4 v = vf[fs]; lane_store(mem(vi[it]), &v, dest); set_vi(it, vi[it] + 1); return 0; }   /* SQI */
    case 0x36: set_vi(is, vi[is] - 1); lane_load((int)ft, mem(vi[is]), dest); return 0;          /* LQD */
    case 0x37: { set_vi(it, vi[it] - 1); V4 v = vf[fs]; lane_store(mem(vi[it]), &v, dest); return 0; }  /* SQD */
    case 0x38: case 0x3A: {                                                                      /* DIV / RSQRT */
        float a = uf(vf[fs].u[fsf]), b = uf(vf[ft].u[ftf]);
        if (sp == 0x3A) b = sqrtf(fabsf(b));
        q_new = b == 0.0f ? ((fu(a) ^ fu(b)) & 0x80000000u) | 0x7F7FFFFFu : fu(a / b);
        q_ready = cycle + (sp == 0x38 ? 7 : 13);
        return 0;
    }
    case 0x39: q_new = fu(sqrtf(fabsf(uf(vf[ft].u[ftf])))); q_ready = cycle + 7; return 0;      /* SQRT */
    case 0x3B: if (q_ready && q_ready > cycle) cycle = q_ready; commit_pending(); return 0;       /* WAITQ */
    case 0x3C: set_vi(it, vf[fs].u[fsf] & 0xFFFF); return 0;                                     /* MTIR */
    case 0x3D: { V4 v; for (int i = 0; i < 4; i++) v.u[i] = (uint32_t)(int32_t)(int16_t)vi[is];   /* MFIR */
                 lane_load((int)ft, &v, dest); return 0; }
    case 0x3E: { V4 *m = mem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8u >> i)) { set_vi(it, m->u[i]); break; }
                 return 0; }                                                                     /* ILWR */
    case 0x3F: { V4 *m = mem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8u >> i)) m->u[i] = vi[it];
                 return 0; }                                                                     /* ISWR */
    case 0x64: { V4 v; for (int i = 0; i < 4; i++) v.u[i] = p_reg; lane_load((int)ft, &v, dest); return 0; }   /* MFP */
    case 0x68: return 0;                                                                         /* XTOP: run() */
    case 0x69: return 0;                                                                         /* XITOP: run() */
    case 0x6C: vu1_stat_kicks++; gs_path1_kick(vu1_mem, vi[is]); return 0;                       /* XGKICK */
    case 0x7B: return 0;                                                                         /* WAITP */
    case 0x40: case 0x41: case 0x42: case 0x43: return 0;                                        /* R*: unused */
    default: vu1_stat_bad++; return 0;
    }
}

/* lower VF sources (for the stall check) */
static void lower_srcs(uint32_t w, unsigned *s1, unsigned *s2)
{
    unsigned op = w >> 25;
    *s1 = *s2 = 0;
    if (op == 0x01) *s1 = (w >> 11) & 31;                                       /* SQ fs */
    else if (op == 0x40 && (w & 0x3F) >= 0x3C) {
        unsigned sp = ((w >> 6) & 31) << 2 | (w & 3);
        if (sp == 0x30 || sp == 0x31 || sp == 0x35 || sp == 0x37 || sp == 0x3C) *s1 = (w >> 11) & 31;
        if (sp == 0x38 || sp == 0x3A) { *s1 = (w >> 11) & 31; *s2 = (w >> 16) & 31; }
        if (sp == 0x39) *s2 = (w >> 16) & 31;
    }
}

/* runs from pc (byte address in micro memory) to the end of the pair after the next E bit */
static void run(uint32_t pc, uint32_t top, uint32_t itop)
{
    int have_branch = 0, end_next = 0;
    uint32_t branch_to = 0, start_pc = pc;
    vu1_stat_runs++;
    for (unsigned steps = 0; steps < 2000000; steps++) {
        pc &= 0x3FF8;
        uint32_t lo = micro[pc / 4], up = micro[pc / 4 + 1];
        unsigned s1, s2, l1, l2;
        upper_srcs(up, &s1, &s2);
        stall_for(s1);
        stall_for(s2);
        if (!(up >> 31)) { lower_srcs(lo, &l1, &l2); stall_for(l1); stall_for(l2); }
        commit_pending();
        UpRes ur;
        upper(up, &ur);
        uint32_t tgt = 0;
        int br = 0;
        if (up >> 31) {
            /* [I]: the lower word is the I register's new value, seen from the next pair */
        } else {
            unsigned sp = ((lo >> 6) & 31) << 2 | (lo & 3);
            if ((lo >> 25) == 0x40 && (lo & 0x3F) >= 0x3C && sp == 0x68) set_vi((lo >> 16) & 15, top);
            else if ((lo >> 25) == 0x40 && (lo & 0x3F) >= 0x3C && sp == 0x69) set_vi((lo >> 16) & 15, itop);
            else br = lower(lo, pc, &tgt);
        }
        if (ur.dst >= 0) {
            V4 *d = ur.dst == 32 ? &acc : &vf[ur.dst];
            if (ur.dst != 0) {
                for (int i = 0; i < 4; i++) if (ur.mask & (8u >> i)) d->u[i] = ur.v.u[i];
                vf_ready[ur.dst] = cycle + 4;
            }
        }
        if (up >> 31) i_reg = lo;
        vf[0].u[0] = vf[0].u[1] = vf[0].u[2] = 0;
        vf[0].u[3] = 0x3F800000u;
        cycle++;
        vu1_stat_pairs++;
        uint32_t next = pc + 8;
        if (have_branch) { next = branch_to; have_branch = 0; }
        if (br) { have_branch = 1; branch_to = tgt; }
        if (end_next) { cont_pc = next; return; }
        if ((up >> 30) & 1) end_next = 1;
        pc = next;
    }
    {
        uint32_t h = 0x811C9DC5u;
        for (int k = 0; k < 128; k++) h = (h ^ micro[k]) * 0x01000193u;
        LOG("vu1_soft: program did not end (2 M pairs), started at pair %u (byte %04x), microcode hash %08x, top %u\n",
            start_pc / 8, start_pc, h, top);
    }
    cont_pc = pc;
}

void vu1_mscal(uint32_t pair, uint32_t top, uint32_t itop) { run(pair * 8, top, itop); }
void vu1_mscnt(uint32_t top, uint32_t itop) { run(cont_pc, top, itop); }

/* FNV-1a of the first 64 instruction pairs of micro memory: identifies the program loaded at 0 (gs_ge.h's native
   programs; values computed from the ELF's MPG packets) */
uint32_t vu1_micro_hash(void)
{
    uint32_t h = 0x811C9DC5u;
    for (int k = 0; k < 128; k++) h = (h ^ micro[k]) * 0x01000193u;
    return h;
}
