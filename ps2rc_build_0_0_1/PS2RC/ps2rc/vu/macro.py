"""VU0 macro mode (COP2) for the C back-end: C statements for the macro instructions and the EE<->VU0 transfers, and
the clean-lane analysis of back-end v3+ (operands known to be finite and not denormal skip the input clamp).

The C side lives in runtime/r5900_rt.h ("VU0 macro mode"): VU0 state in R5900Ctx.vu0, always_inline helpers
(vu0_fmac & co) that fold to straight-line code for the constant arguments emitted here. Semantics and the PCSX2
references: the "VU0 macro mode" section of r5900_rt.h.

Supported: FMAC add/sub/mul/madd/msub in every form (lane-wise, x/y/z/w broadcast, I, Q; Fd and ACC destinations),
VMOVE, VMR32, VABS, VNOP, VWAITQ, VRINIT/VRGET/VRNEXT/VRXOR, VCALLMS/VCALLMSR (rt_vu0_callms hook), LQC2, SQC2,
QMTC2, QMFC2, CFC2, CTC2 (+ .I/.NI). Everything else (MAX/MINI, OPMULA/OPMSUB, CLIP, FTOI/ITOF, DIV/SQRT/RSQRT,
integer and VU-memory instructions, BC2x, CTC2 to CMSAR1) is reported unsupported (stmt returns None).
"""
FIELDS = "xyzw"
_FAMILIES = ("vmadd", "vmsub", "vmul", "vadd", "vsub")          # longest prefixes first
_OP = {"vadd": "VU0_ADD", "vsub": "VU0_SUB", "vmul": "VU0_MUL", "vmadd": "VU0_MADD", "vmsub": "VU0_MSUB"}
_XFER = {"lqc2", "sqc2", "qmtc2", "qmfc2", "cfc2", "ctc2"}
ACC = 32                                                        # lane-state register index of ACC


def base_mnem(m):
    """Mnemonic without the dest suffix / interlock suffix: 'vmulax.xyz' -> 'vmulax', 'cfc2.i' -> 'cfc2'."""
    return m.split(".")[0]


def fmac(base):
    """(family, acc, kind) of an FMAC mnemonic base, else None. kind: '' lane-wise, 'x'..'w' broadcast, 'q', 'i'."""
    for fam in _FAMILIES:
        if base.startswith(fam):
            rest = base[len(fam):]
            acc = rest.startswith("a")
            kind = rest[1:] if acc else rest
            return (fam, acc, kind) if kind in ("", "x", "y", "z", "w", "q", "i") else None
    return None


def handles(insn):
    """True for the instructions this module translates (or rejects): COP2 macro mode and the VU0 transfers."""
    m = insn.mnem
    return m.startswith("v") or base_mnem(m) in _XFER


def _lanes(comps):
    return [FIELDS.index(ch) for ch in comps]


def _dest(comps):
    """C dest field mask: x = 8, y = 4, z = 2, w = 1 (the instruction's bits 24..21)."""
    return sum(8 >> i for i in _lanes(comps))


def _clean(st, n, i):
    return n == 0 or bool(st >> (4 * n + i) & 1)


def stmt(insn, st, R, W, mem_addr):
    """C statement(s) for one VU0 instruction, or None when unsupported. st: clean-lane state before the instruction
    (int bit 4*n+i = lane i of VF[n], ACC = n 32) or None (no knowledge: clamp every operand, clamp results).
    R/W/mem_addr: the back-end's GPR read / GPR write / effective-address helpers."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    # ---- transfers
    if base == "lqc2":
        t = o[0][1]
        return "rt_lq(c, %s, &c->vu0.vf[%d]);" % (mem_addr(o[1]), t) if t else ";"
    if base == "sqc2":
        t = o[0][1]
        if t == 0:
            return "{ const R5900Reg128 t_ = vu0_vf(c, 0); rt_sq(c, %s, &t_); }" % mem_addr(o[1])
        return "rt_sq(c, %s, &c->vu0.vf[%d]);" % (mem_addr(o[1]), t)
    if base == "qmtc2":
        rt, d = o[0][1], o[1][1]
        if d == 0:
            return ";"
        if rt == 0:
            return "c->vu0.vf[%d].d[0] = 0; c->vu0.vf[%d].d[1] = 0;" % (d, d)
        return "c->vu0.vf[%d] = c->r[%d];" % (d, rt)
    if base == "qmfc2":
        rt, d = o[0][1], o[1][1]
        return "c->r[%d] = vu0_vf(c, %d);" % (rt, d) if rt else ";"
    if base == "cfc2":
        return W(o[0][1], "vu0_cfc2(c, %d)" % o[1][1]) or ";"
    if base == "ctc2":
        if o[1][1] == 31:                          # CMSAR1: starts a VU1 micro program (not modelled)
            return None
        return "vu0_ctc2(c, %d, (uint32_t)%s);" % (o[1][1], R(o[0][1]))
    # ---- macro instructions
    f = fmac(base)
    if f is not None:
        fam, acc, kind = f
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        d = ACC if o[0][0] == "acc" else o[0][1]
        if d == 0:
            return ";"                             # VF00 is read-only (flags are not modelled)
        s = o[1][1]
        if kind == "q":
            t, bc = "VU0_QREG", 0
        elif kind == "i":
            t, bc = "VU0_IREG", 0
        else:
            t, bc = o[2][1], (FIELDS.index(kind) if kind else -1)
        cl, fast = 0, 0
        if st is not None:
            lanes = _lanes(comps)
            fast = 1
            if all(_clean(st, s, i) for i in lanes):
                cl |= 1
            if kind in ("q", "i"):
                pass
            elif kind and _clean(st, t, bc):
                cl |= 2
            elif not kind and all(_clean(st, t, i) for i in lanes):
                cl |= 2
            if fam in ("vmadd", "vmsub") and all(_clean(st, ACC, i) for i in lanes):
                cl |= 4
        return "vu0_fmac(c, %s, %s, 0x%x, %d, %s, %d, %d, %d);" % (
            _OP[fam], "VU0_ACC" if d == ACC else d, _dest(comps), s, t, bc, cl, fast)
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return ";"
        return "vu0_move(c, 0x%x, %d, %d, %d, %d);" % (_dest(o[0][2]), t, s, base == "vmr32", base == "vabs")
    if base in ("vnop", "vwaitq"):
        return ";"
    if base in ("vrinit", "vrxor"):
        s, fsf = o[1][1], FIELDS.index(o[1][2])
        return "vu0_%s(c, vu0_lane(c, %d, %d));" % (base[1:], s, fsf)
    if base == "vrget":
        t = o[0][1]
        return "vu0_fill(c, 0x%x, %d, c->vu0.vi[20]);" % (_dest(o[0][2]), t) if t else ";"
    if base == "vrnext":                           # the LFSR advances even when the result goes to VF00 (microVU)
        t = o[0][1]
        if t == 0:
            return "(void)vu0_rnext(c);"
        return "{ uint32_t r_ = vu0_rnext(c); vu0_fill(c, 0x%x, %d, r_); }" % (_dest(o[0][2]), t)
    ext = _extra(insn, base, o)
    if ext is not None:
        return ext
    # flush kind "vcm": the back-end's full ("fatal") flush, or with the lever vcm only the VU0 lanes
    if base == "vcallms":
        return "{ /*FLUSH@i%08x|vcm*/ rt_vu0_callms(c, 0x%08xu, 0x%xu); }" % (insn.pc, insn.pc, o[0][1])
    if base == "vcallmsr":
        return ("{ /*FLUSH@i%08x|vcm*/ rt_vu0_callms(c, 0x%08xu, (c->vu0.vi[27] & 0xFFFFu) * 8u); }"
                % (insn.pc, insn.pc))
    return None


# ---------------------------------------------------------------- MAX/MINI, OPMULA/OPMSUB, ITOF/FTOI, DIV, VIADDI,
# VLQD/VSQI (added for BT3, SLUS-216.78). Operands are clamped like the FMAC inputs (vu0_clamp); results are written to
# c->vu0 directly (back-end v0; lever vfloc functions see them through vf_writes). VU0 data memory (4 KB, 256 quads,
# addresses wrap) is rt_vu0_mem, set by the HLE to its VU memory model (0x11004000).
_ITOF_SCALE = {"0": "1.0f", "4": "16.0f", "12": "4096.0f", "15": "32768.0f"}


def _minmax(base):
    """(is_max, kind) of a VMAX / VMINI mnemonic base, else None. kind: '' lane-wise, 'x'..'w' broadcast, 'i'."""
    for fam, is_max in (("vmini", False), ("vmax", True)):
        if base.startswith(fam):
            kind = base[len(fam):]
            return (is_max, kind) if kind in ("", "x", "y", "z", "w", "i") else None
    return None


def _fl(expr):
    return "u2f(vu0_clamp(%s))" % expr


def _extra(insn, base, o):
    """C for the macro instructions above, or None when the instruction is none of them."""
    mm = _minmax(base)
    if mm is not None:
        is_max, kind = mm
        d, comps, s = o[0][1], o[0][2], o[1][1]
        if d == 0:
            return ";"
        parts = []
        for i in _lanes(comps):
            if kind == "i":
                t_expr = "vu0_lane(c, VU0_IREG, 0)"
            elif kind:
                t_expr = "vu0_lane(c, %d, %d)" % (o[2][1], FIELDS.index(kind))
            else:
                t_expr = "vu0_lane(c, %d, %d)" % (o[2][1], i)
            parts.append("{ uint32_t a_ = vu0_clamp(vu0_lane(c, %d, %d)), b_ = vu0_clamp(%s); r_[%d] = "
                         "(u2f(a_) %s u2f(b_)) ? a_ : b_; }" % (s, i, t_expr, i, ">" if is_max else "<"))
        stores = " ".join("c->vu0.vf[%d].w[%d] = r_[%d];" % (d, i, i) for i in _lanes(comps))
        return "{ uint32_t r_[4]; %s %s }" % (" ".join(parts), stores)
    if base in ("vopmula", "vopmsub"):
        s, t = o[1][1], o[2][1]
        prod = ["%s * %s" % (_fl("vu0_lane(c, %d, %d)" % (s, a)), _fl("vu0_lane(c, %d, %d)" % (t, b)))
                for a, b in ((1, 2), (2, 0), (0, 1))]                 # x = fs.y*ft.z, y = fs.z*ft.x, z = fs.x*ft.y
        if base == "vopmula":
            comps = o[0][1]
            return "{ float r_[3]; %s %s }" % (
                " ".join("r_[%d] = %s;" % (k, prod[k]) for k in range(3)),
                " ".join("c->vu0.acc.w[%d] = vu0_clamp(f2u(r_[%d]));" % (i, i) for i in _lanes(comps) if i < 3))
        d, comps = o[0][1], o[0][2]
        if d == 0:
            return ";"
        return "{ float r_[3]; %s %s }" % (
            " ".join("r_[%d] = %s - (%s);" % (k, _fl("c->vu0.acc.w[%d]" % k), prod[k]) for k in range(3)),
            " ".join("c->vu0.vf[%d].w[%d] = vu0_clamp(f2u(r_[%d]));" % (d, i, i) for i in _lanes(comps) if i < 3))
    for fam in ("vitof", "vftoi"):
        if base.startswith(fam) and base[len(fam):] in _ITOF_SCALE:
            sc = _ITOF_SCALE[base[len(fam):]]
            t, comps, s = o[0][1], o[0][2], o[1][1]
            if t == 0:
                return ";"
            if fam == "vitof":
                body = " ".join("c->vu0.vf[%d].w[%d] = f2u((float)(int32_t)s_.w[%d] / %s);" % (t, i, i, sc)
                                for i in _lanes(comps))
            else:
                body = " ".join("{ float f_ = u2f(vu0_clamp(s_.w[%d])) * %s; c->vu0.vf[%d].w[%d] = "
                                "f_ >= 2147483647.0f ? 0x7FFFFFFFu : f_ <= -2147483648.0f ? 0x80000000u : "
                                "(uint32_t)(int32_t)f_; }" % (i, sc, t, i) for i in _lanes(comps))
            return "{ const R5900Reg128 s_ = vu0_vf(c, %d); %s }" % (s, body)
    if base == "vdiv":                                       # vdiv Q, fs.fsf, ft.ftf
        s, fsf, t, ftf = o[1][1], FIELDS.index(o[1][2]), o[2][1], FIELDS.index(o[2][2])
        return ("{ const uint32_t a_ = vu0_clamp(vu0_lane(c, %d, %d)), b_ = vu0_clamp(vu0_lane(c, %d, %d)); "
                "c->vu0.vi[22] = (b_ & 0x7FFFFFFFu) == 0 ? (((a_ ^ b_) & 0x80000000u) | 0x7F7FFFFFu) : "
                "vu0_clamp(f2u(u2f(a_) / u2f(b_))); }" % (s, fsf, t, ftf))
    if base == "vsqrt":                                      # vsqrt Q, ft.ftf: Q = sqrt(|ft|)
        t, ftf = o[1][1], FIELDS.index(o[1][2])
        return ("{ const uint32_t b_ = vu0_clamp(vu0_lane(c, %d, %d)) & 0x7FFFFFFFu; "
                "c->vu0.vi[22] = vu0_clamp(f2u(__builtin_sqrtf(u2f(b_)))); }" % (t, ftf))
    if base == "vrsqrt":                                     # vrsqrt Q, fs.fsf, ft.ftf: Q = fs / sqrt(|ft|)
        s, fsf, t, ftf = o[1][1], FIELDS.index(o[1][2]), o[2][1], FIELDS.index(o[2][2])
        return ("{ const uint32_t a_ = vu0_clamp(vu0_lane(c, %d, %d)), b_ = vu0_clamp(vu0_lane(c, %d, %d)) & 0x7FFFFFFFu; "
                "c->vu0.vi[22] = b_ == 0 ? ((a_ & 0x80000000u) | 0x7F7FFFFFu) : "
                "vu0_clamp(f2u(u2f(a_) / __builtin_sqrtf(u2f(b_)))); }" % (s, fsf, t, ftf))
    if base == "vmfir":                                      # vmfir.dest ft, is: VI[is] sign-extended to the lanes
        t, comps, is_ = o[0][1], o[0][2], o[1][1]
        if t == 0:
            return ";"
        return "{ const uint32_t v_ = (uint32_t)(int32_t)(int16_t)%s; %s }" % (
            "c->vu0.vi[%d]" % is_ if is_ else "0", " ".join("c->vu0.vf[%d].w[%d] = v_;" % (t, i) for i in _lanes(comps)))
    if base == "viaddi":                                     # viaddi it, is, imm5
        it, is_, imm = o[0][1], o[1][1], o[2][1]
        if it == 0:
            return ";"
        return "c->vu0.vi[%d] = (c->vu0.vi[%d] + (uint32_t)(%d)) & 0xFFFFu;" % (it, is_, imm)
    if base == "vlqd":                                       # vlqd.dest ft, (--is)
        t, comps, is_ = o[0][1], o[0][2], o[1][1]
        dec = "c->vu0.vi[%d] = (c->vu0.vi[%d] - 1u) & 0xFFFFu; " % (is_, is_) if is_ else ""
        if t == 0:
            return "{ %s}" % dec
        return ("{ %suint32_t q_ = (%s & 0xFFu) * 16u; %s }" % (
            dec, "c->vu0.vi[%d]" % is_ if is_ else "0xFFFFu",
            " ".join("memcpy(&c->vu0.vf[%d].w[%d], rt_vu0_mem + q_ + %d, 4);" % (t, i, 4 * i) for i in _lanes(comps))))
    if base == "vsqi":                                       # vsqi.dest fs, (it++)
        s, comps, it = o[0][1], o[0][2], o[1][1]
        inc = "c->vu0.vi[%d] = (c->vu0.vi[%d] + 1u) & 0xFFFFu;" % (it, it) if it else ""
        return ("{ const R5900Reg128 v_ = vu0_vf(c, %d); uint32_t q_ = (c->vu0.vi[%d] & 0xFFu) * 16u; %s %s }" % (
            s, it, " ".join("memcpy(rt_vu0_mem + q_ + %d, &v_.w[%d], 4);" % (4 * i, i) for i in _lanes(comps)), inc))
    return None


def _extra_writes(base, o):
    """VF registers (32 = ACC) the instructions of _extra write, or None when base is none of them."""
    if _minmax(base) is not None or base in ("vopmsub", "vlqd", "vmfir") or \
            any(base.startswith(f) and base[len(f):] in _ITOF_SCALE for f in ("vitof", "vftoi")):
        return {o[0][1]} - {0}
    if base == "vopmula":
        return {ACC}
    if base in ("vdiv", "vsqrt", "vrsqrt", "viaddi", "vsqi"):
        return set()
    return None


# ---------------------------------------------------------------- lever vfloc
# VF01-VF31 and ACC lanes in C locals v<n>_<lane> / va_<lane> (uint32_t raw bits) inside an ABI-mode function; the
# back-end writes the lanes the function writes back to c->vu0 at every boundary (calls, returns, syscalls, fatal
# points, VCALLMS) and reloads them after calls and VCALLMS - the context holds the guest VU0 state wherever anything
# else can observe it. VF00, I, Q, R and the VI registers stay in the context.
def _lv(n, i):
    """Local (or constant) for lane i of VF[n]."""
    if n == 0:
        return "0x3F800000u" if i == 3 else "0u"
    return "v%d_%d" % (n, i)


def vf_writes(insn):
    """VF registers (32 = ACC) an instruction writes; -1 for VCALLMS (the micro subroutine may write any)."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base in ("vcallms", "vcallmsr"):
        return -1
    if base == "lqc2":
        return {o[0][1]} - {0}
    if base == "qmtc2":
        return {o[1][1]} - {0}
    f = fmac(base) if m.startswith("v") else None
    if f is not None:
        return {ACC if o[0][0] == "acc" else o[0][1]} - {0}
    if base in ("vmove", "vmr32", "vabs", "vrget", "vrnext"):
        return {o[0][1]} - {0}
    ew = _extra_writes(base, o)
    if ew is not None:
        return ew
    return set()


def stmt_loc(insn, st, R, W, mem_addr):
    """stmt() with the VF / ACC lanes in C locals (lever vfloc)."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base == "lqc2":
        t = o[0][1]
        if not t:
            return ";"
        return "{ R5900Reg128 q_; rt_lq(c, %s, &q_); %s }" % (
            mem_addr(o[1]), " ".join("v%d_%d = q_.w[%d];" % (t, i, i) for i in range(4)))
    if base == "sqc2":
        t = o[0][1]
        return "{ R5900Reg128 q_; %s rt_sq(c, %s, &q_); }" % (
            " ".join("q_.w[%d] = %s;" % (i, _lv(t, i)) for i in range(4)), mem_addr(o[1]))
    if base == "qmtc2":
        rt, d = o[0][1], o[1][1]
        if d == 0:
            return ";"
        return " ".join("v%d_%d = %s;" % (d, i, "0u" if rt == 0 else "c->r[%d].w[%d]" % (rt, i)) for i in range(4))
    if base == "qmfc2":
        rt, d = o[0][1], o[1][1]
        return " ".join("c->r[%d].w[%d] = %s;" % (rt, i, _lv(d, i)) for i in range(4)) if rt else ";"
    if base in ("cfc2", "ctc2", "vnop", "vwaitq", "vcallms", "vcallmsr"):
        s = stmt(insn, st, R, W, mem_addr)
        if s is not None and base in ("vcallms", "vcallmsr"):
            s = s[:-1] + "/*VRELOAD*/ }"                 # the micro subroutine works on c->vu0
        return s
    f = fmac(base)
    if f is not None:
        fam, acc, kind = f
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        d = ACC if o[0][0] == "acc" else o[0][1]
        if d == 0:
            return ";"
        s = o[1][1]
        if kind == "q":
            t, bc = "Q", 0
        elif kind == "i":
            t, bc = "I", 0
        else:
            t, bc = o[2][1], (FIELDS.index(kind) if kind else -1)
        cl, fast = 0, 0
        if st is not None:
            lanes = _lanes(comps)
            fast = 1
            if all(_clean(st, s, i) for i in lanes):
                cl |= 1
            if kind in ("q", "i"):
                pass
            elif kind and _clean(st, t, bc):
                cl |= 2
            elif not kind and all(_clean(st, t, i) for i in lanes):
                cl |= 2
            if fam in ("vmadd", "vmsub") and all(_clean(st, ACC, i) for i in lanes):
                cl |= 4
        calc, assign = [], []
        for i in _lanes(comps):
            tv = ("c->vu0.vi[21]" if t == "I" else "c->vu0.vi[22]" if t == "Q" else
                  _lv(t, i if bc < 0 else bc))
            calc.append("x%d_ = vu0_fmac1(%s, va_%d, %s, %s, %d, %d)" % (i, _OP[fam], i, _lv(s, i), tv, cl, fast)
                        if fam in ("vmadd", "vmsub") else
                        "x%d_ = vu0_fmac1(%s, 0u, %s, %s, %d, %d)" % (i, _OP[fam], _lv(s, i), tv, cl, fast))
            assign.append("%s = x%d_;" % ("va_%d" % i if d == ACC else "v%d_%d" % (d, i), i))
        return "{ uint32_t %s; %s }" % (", ".join(calc), " ".join(assign))
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return ";"
        rot = 1 if base == "vmr32" else 0
        mask = " & 0x7FFFFFFFu" if base == "vabs" else ""
        lanes = _lanes(o[0][2])
        return "{ uint32_t %s; %s }" % (", ".join("x%d_ = %s%s" % (i, _lv(s, (i + rot) & 3), mask) for i in lanes),
                                       " ".join("v%d_%d = x%d_;" % (t, i, i) for i in lanes))
    if base in ("vrinit", "vrxor"):
        s, fsf = o[1][1], FIELDS.index(o[1][2])
        return "vu0_%s(c, %s);" % (base[1:], _lv(s, fsf))
    if base == "vrget":
        t = o[0][1]
        return " ".join("v%d_%d = c->vu0.vi[20];" % (t, i) for i in _lanes(o[0][2])) if t else ";"
    if base == "vrnext":
        t = o[0][1]
        if t == 0:
            return "(void)vu0_rnext(c);"
        return "{ uint32_t r_ = vu0_rnext(c); %s }" % " ".join("v%d_%d = r_;" % (t, i) for i in _lanes(o[0][2]))
    return None


# ---------------------------------------------------------------- clean-lane analysis (back-end v3+)
def transfer(insn, st):
    """Clean-lane state after `insn`. Clean = finite and not denormal (the input clamp is the identity): FMAC results
    (round-toward-zero cannot overflow to Inf, flush-to-zero removes denormals), R-register values (1.0 + mantissa),
    moves of clean lanes. LQC2/QMTC2 load unknown bits; a micro subroutine may write anything."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base == "lqc2" and o[0][1]:
        return st & ~(0xF << 4 * o[0][1])
    if base == "qmtc2" and o[1][1]:
        return st & ~(0xF << 4 * o[1][1])
    if base in ("vcallms", "vcallmsr"):
        return 0
    if not m.startswith("v"):
        return st
    if fmac(base) is not None:
        n = ACC if o[0][0] == "acc" else o[0][1]
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        if n == 0:
            return st
        return st | sum(1 << (4 * n + i) for i in _lanes(comps))
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return st
        rot = 1 if base == "vmr32" else 0
        new = st
        for i in _lanes(o[0][2]):
            bit = 1 << (4 * t + i)
            new = new | bit if _clean(st, s, (i + rot) & 3) else new & ~bit
        return new
    if base in ("vrget", "vrnext") and o[0][1]:
        return st | sum(1 << (4 * o[0][1] + i) for i in _lanes(o[0][2]))
    return st


def clean_states(img, start, addrs, jump_tables, func_starts):
    """{address: clean-lane state before the instruction} (None: unreachable) for one function, or None when the
    function has no VU0 instruction. Function entry and every call return start with nothing known."""
    import heapq

    from ..backend_c import fn_cfg
    if not any(img.word(a) >> 26 in (0x12, 0x36, 0x3E) for a in addrs):     # no COP2 / LQC2 / SQC2 word
        return None
    f = fn_cfg(img, start, addrs, jump_tables, func_starts)
    din = {a: None for a in f.addrs}
    if start not in din:
        return din
    din[start] = 0
    work, pending = [start], {start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = transfer(f.insn[a], din[a])
        for t, e in f.succ[a]:
            v = 0 if e is not None else out
            cur = din[t]
            new = v if cur is None else cur & v
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din
