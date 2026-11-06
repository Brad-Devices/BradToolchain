#include "ptx.h"
#include "brad/bradvector.h"
#include "brad/bvbc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Register budget ───
 *
 * S0-S15 are special (zero, lane id, block id, ...).  S16-S31 are the
 * kernel arguments.  S32-S223 belong to PTX virtual registers, S224-S239
 * are per-instruction temporaries (reused after every IR instruction), and
 * S240-S247 hold cached constants (1, 0x80000000).
 *
 * Bit copies use IADD d, s, S0 rather than BVAS MOV: the assembler lowers
 * MOV to ADDi with a zero immediate, which is a float add and therefore
 * canonicalises -0.0 and NaNs.  IADD with zero is a true 32-bit copy.
 */
#define S_VREG_BASE  32
#define S_VREG_TOP  223
#define S_TEMP_BASE 224
#define S_TEMP_TOP  239
#define S_CONST_BASE 240
#define S_CONST_TOP  247

struct label {
    char name[CVT_NAME];
    int  pc;
};

struct patch {
    int  pc;
    int  line;
    char name[CVT_NAME];
};

struct cg {
    struct cvt_module       *m;
    struct cvt_diag         *diag;
    int                      failed;
    int                      status;

    uint64_t                *insns;
    int                      n;

    struct label             labels[CVT_MAX_LABELS];
    int                      nlabels;

    struct patch             patches[CVT_MAX_PATCH];
    int                      npatch;

    int                      vreg_next;
    int                      temp_next;
    int                      const_next;

    int                      one_sreg;
    int                      sign_sreg;

    int                      preg_used[16];
    int                      ptemp_used[16];

    int                      max_sreg;
    int                      internal_seq;
};

static int fail(struct cg *cg, int status, int line, const char *fmt, ...)
{
    if (!cg->failed) {
        va_list ap;
        cg->failed = 1;
        cg->status = status;
        cg->diag->line = line;
        va_start(ap, fmt);
        vsnprintf(cg->diag->msg, sizeof(cg->diag->msg), fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ─── Emission ─── */

static int emit(struct cg *cg, uint8_t op, uint8_t pred, uint8_t dst,
                uint8_t src1, uint8_t src2, int8_t imm, uint8_t flags)
{
    if (cg->failed)
        return -1;
    if (cg->n >= BVBC_MAX_INSN)
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, 0,
                    "translated stream exceeds %d instructions",
                    BVBC_MAX_INSN);
    cg->insns[cg->n++] = BVEN(op, pred, dst, src1, src2, imm, flags, 0);
    return 0;
}

static int emit_branch(struct cg *cg, uint8_t op, uint8_t pred,
                       const char *target, int line)
{
    if (cg->failed)
        return -1;
    if (cg->npatch >= CVT_MAX_PATCH)
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                    "too many branches (limit %d)", CVT_MAX_PATCH);
    int pc = cg->n;
    if (emit(cg, op, pred, 0, 0, 0, 0, 0) < 0)
        return -1;
    cg->patches[cg->npatch].pc = pc;
    cg->patches[cg->npatch].line = line;
    snprintf(cg->patches[cg->npatch].name,
             sizeof(cg->patches[cg->npatch].name), "%s", target);
    cg->npatch++;
    return 0;
}

static int define_label(struct cg *cg, const char *name, int line)
{
    if (cg->failed)
        return -1;
    if (cg->nlabels >= CVT_MAX_LABELS)
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line, "too many labels");
    for (int i = 0; i < cg->nlabels; i++) {
        if (strcmp(cg->labels[i].name, name) == 0)
            return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                        "label '%s' defined twice", name);
    }
    snprintf(cg->labels[cg->nlabels].name,
             sizeof(cg->labels[cg->nlabels].name), "%s", name);
    cg->labels[cg->nlabels].pc = cg->n;
    cg->nlabels++;
    return 0;
}

static void internal_label(struct cg *cg, const char *tag, char *out)
{
    snprintf(out, CVT_NAME, "$cvt$%s$%d", tag, cg->internal_seq++);
}

/* ─── Registers ─── */

static int sreg_of(struct cg *cg, struct cvt_vreg *v, int line)
{
    if (cg->failed)
        return -1;
    if (v->type == TY_PRED) {
        fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
             "predicate '%s' used as a scalar", v->name);
        return -1;
    }
    if (v->sreg < 0) {
        if (cg->vreg_next > S_VREG_TOP) {
            fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                 "out of scalar registers for '%s' (max %d vregs)",
                 v->name, S_VREG_TOP - S_VREG_BASE + 1);
            return -1;
        }
        v->sreg = cg->vreg_next++;
        if (v->sreg > cg->max_sreg)
            cg->max_sreg = v->sreg;
    }
    return v->sreg;
}

/* ─── 64-bit register pairs ───
 *
 * The reference register file is 32-bit.  A 64-bit virtual register keeps
 * its low word in `sreg` and its high word in `sreg_hi`.  The high word is
 * allocated lazily: address-carrier registers (the common case) never pay
 * for it.  `hi_zero` records that the high word is known to be zero, which
 * is true for anything produced by 32-bit arithmetic, a 32-bit immediate,
 * or a parameter load.  Only a 64-bit global load makes the high word live.
 */
static int sreg_hi(struct cg *cg, struct cvt_vreg *v, int line)
{
    if (cg->failed)
        return -1;
    if (v->type == TY_PRED || !cvt_is_64(v->type)) {
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                    "register '%s' has no 64-bit high word", v->name);
    }
    if (v->sreg_hi < 0) {
        if (cg->vreg_next > S_VREG_TOP) {
            return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                        "out of scalar registers for the high word of '%s'",
                        v->name);
        }
        v->sreg_hi = cg->vreg_next++;
        if (v->sreg_hi > cg->max_sreg)
            cg->max_sreg = v->sreg_hi;
    }
    return v->sreg_hi;
}

/* Read a 64-bit value's high word.  A zero-extended value needs no
 * register at all: S0 reads as zero every time. */
static int hi_read(struct cg *cg, struct cvt_vreg *v, int line, int *out)
{
    if (v->hi_zero) {
        *out = BV_S_ZERO;
        return 0;
    }
    int h = sreg_hi(cg, v, line);
    if (h < 0)
        return -1;
    *out = h;
    return 0;
}

/* The low word was just written by a 32-bit operation; zero-extend it. */
static int mark_hi_zero(struct cg *cg, struct cvt_vreg *v, uint8_t pred,
                        int line)
{
    if (!cvt_is_64(v->type))
        return 0;
    v->hi_zero = 1;
    if (v->sreg_hi >= 0)
        return emit(cg, BV_OP_IADD, pred, (uint8_t)v->sreg_hi,
                    BV_S_ZERO, BV_S_ZERO, 0, 0);
    return 0;
}

/* The value is about to become genuinely 64-bit (a global load). */
static int mark_hi_live(struct cg *cg, struct cvt_vreg *v, int line, int *out)
{
    int h = sreg_hi(cg, v, line);
    if (h < 0)
        return -1;
    v->hi_zero = 0;
    *out = h;
    return 0;
}

static int preg_of(struct cg *cg, struct cvt_vreg *v, int line)
{
    if (cg->failed)
        return -1;
    if (v->type != TY_PRED) {
        fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
             "'%s' is not a predicate", v->name);
        return -1;
    }
    if (v->preg < 0) {
        for (int i = 0; i < 16; i++) {
            if (!cg->preg_used[i] && !cg->ptemp_used[i]) {
                v->preg = i;
                cg->preg_used[i] = 1;
                return i;
            }
        }
        fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
             "out of predicate registers (max 16)");
        return -1;
    }
    return v->preg;
}

static int ptemp_alloc(struct cg *cg, int line)
{
    if (cg->failed)
        return -1;
    for (int i = 15; i >= 0; i--) {
        if (!cg->preg_used[i] && !cg->ptemp_used[i]) {
            cg->ptemp_used[i] = 1;
            return i;
        }
    }
    fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
         "out of temporary predicate registers");
    return -1;
}

static int alloc_temp(struct cg *cg, int line)
{
    if (cg->failed)
        return -1;
    if (cg->temp_next > S_TEMP_TOP) {
        fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
             "out of per-instruction temporaries");
        return -1;
    }
    return cg->temp_next++;
}

static int alloc_const(struct cg *cg, int line)
{
    if (cg->failed)
        return -1;
    if (cg->const_next > S_CONST_TOP) {
        fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
             "out of cached-constant registers");
        return -1;
    }
    return cg->const_next++;
}

/* ─── Constants ─── */

static int ensure_one(struct cg *cg, int line, int *out)
{
    if (cg->one_sreg < 0) {
        int s = alloc_const(cg, line);
        if (s < 0)
            return -1;
        /* NOT 0 = 0xFFFFFFFF; (-1)*(-1) = 1 */
        if (emit(cg, BV_OP_NOT, 0, (uint8_t)s, BV_S_ZERO, 0, 0, 0) < 0)
            return -1;
        if (emit(cg, BV_OP_IMUL, 0, (uint8_t)s, (uint8_t)s, (uint8_t)s,
                 0, 0) < 0)
            return -1;
        cg->one_sreg = s;
    }
    *out = cg->one_sreg;
    return 0;
}

static int ensure_sign(struct cg *cg, int line, int *out)
{
    if (cg->sign_sreg < 0) {
        int one, s;
        if (ensure_one(cg, line, &one) < 0)
            return -1;
        s = alloc_const(cg, line);
        if (s < 0)
            return -1;
        /* 0xFFFFFFFF -> 0x7FFFFFFF -> 0x80000000 */
        if (emit(cg, BV_OP_NOT, 0, (uint8_t)s, BV_S_ZERO, 0, 0, 0) < 0)
            return -1;
        if (emit(cg, BV_OP_SHR, 0, (uint8_t)s, (uint8_t)s, (uint8_t)one,
                 0, 0) < 0)
            return -1;
        if (emit(cg, BV_OP_IADD, 0, (uint8_t)s, (uint8_t)s, (uint8_t)one,
                 0, 0) < 0)
            return -1;
        cg->sign_sreg = s;
    }
    *out = cg->sign_sreg;
    return 0;
}

/* Materialise an arbitrary 32-bit pattern using only IADD/IMUL/NOT.
 * Doubling from 1 builds any value; v == 0 and v == ~0 get shorter paths. */
static int const_u32(struct cg *cg, uint32_t v, int line, int *out)
{
    if (cg->failed)
        return -1;

    if (v == 0) {
        int t = alloc_temp(cg, line);
        if (t < 0) return -1;
        if (emit(cg, BV_OP_ADDi, 0, (uint8_t)t, BV_S_ZERO, 0, 0, 0) < 0)
            return -1;
        *out = t;
        return 0;
    }
    if (v == 0xFFFFFFFFu) {
        int t = alloc_temp(cg, line);
        if (t < 0) return -1;
        if (emit(cg, BV_OP_NOT, 0, (uint8_t)t, BV_S_ZERO, 0, 0, 0) < 0)
            return -1;
        *out = t;
        return 0;
    }

    int one;
    if (ensure_one(cg, line, &one) < 0)
        return -1;

    int t = alloc_temp(cg, line);
    if (t < 0)
        return -1;
    /* t = 1, as a bit copy. */
    if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)one, BV_S_ZERO,
             0, 0) < 0)
        return -1;

    int msb = 31;
    while (msb >= 0 && !(v & (1u << msb)))
        msb--;
    for (int i = msb - 1; i >= 0; i--) {
        if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)t, (uint8_t)t,
                 0, 0) < 0)
            return -1;
        if (v & (1u << i)) {
            if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)t,
                     (uint8_t)one, 0, 0) < 0)
                return -1;
        }
    }
    *out = t;
    return 0;
}

static int const_float_bits(struct cg *cg, uint32_t bits, int line, int *out)
{
    float f;
    memcpy(&f, &bits, sizeof(f));

    if (bits == 0)
        return const_u32(cg, 0, line, out);

    if (bits == 0x80000000u) {          /* -0.0 */
        int t, sign;
        if (const_u32(cg, 0, line, &t) < 0)
            return -1;
        if (ensure_sign(cg, line, &sign) < 0)
            return -1;
        if (emit(cg, BV_OP_XOR, 0, (uint8_t)t, (uint8_t)t, (uint8_t)sign,
                 0, 0) < 0)
            return -1;
        *out = t;
        return 0;
    }

    if (f >= -128.0f && f <= 127.0f && (float)(int)f == f) {
        int t = alloc_temp(cg, line);
        if (t < 0) return -1;
        if (emit(cg, BV_OP_ADDi, 0, (uint8_t)t, BV_S_ZERO, 0,
                 (int8_t)(int)f, 0) < 0)
            return -1;
        *out = t;
        return 0;
    }

    /* Any other bit pattern: build the integer, not the float. */
    return const_u32(cg, bits, line, out);
}

/* ─── Operand lowering ─── */

static int src_typed(struct cg *cg, const struct cvt_operand *o,
                     enum cvt_type ty, int line, int *out)
{
    switch (o->kind) {
    case OPR_REG:
        *out = sreg_of(cg, o->reg, line);
        return *out < 0 ? -1 : 0;

    case OPR_SPECIAL:
        if (!strcmp(o->name, "%tid.x") || !strcmp(o->name, "%laneid")) {
            *out = BV_S_THREAD_ID;
            return 0;
        }
        if (!strcmp(o->name, "%ctaid.x")) {
            *out = BV_S_BLOCK_ID;
            return 0;
        }
        if (!strcmp(o->name, "%ntid.x"))
            return const_u32(cg, 32, line, out);
        if (!strcmp(o->name, "%nctaid.x"))
            return const_u32(cg, 1, line, out);
        return fail(cg, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "special register '%s' is outside the v0 subset",
                    o->name);

    case OPR_IMM:
        /* BVRT scalar registers are 32-bit.  A 64-bit type is an address
         * carrier here, so a constant that does not fit in 32 bits has no
         * faithful encoding and must be rejected rather than truncated. */
        if (ty == TY_B64 || ty == TY_U64 || ty == TY_S64) {
            int64_t s = (int64_t)o->imm;
            if (o->imm > UINT32_MAX &&
                (s < INT32_MIN || s > INT32_MAX))
                return fail(cg, BRAD_CVT_ERR_UNSUPPORTED, line,
                            "64-bit immediate 0x%llx does not fit the "
                            "32-bit reference register file",
                            (unsigned long long)o->imm);
        }
        if (o->imm_float)
            return const_float_bits(cg, (uint32_t)o->imm, line, out);
        if (cvt_is_float(ty)) {
            float f = (float)(int64_t)o->imm;
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            return const_float_bits(cg, bits, line, out);
        }
        return const_u32(cg, (uint32_t)o->imm, line, out);

    default:
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                    "operand is not a scalar source");
    }
}

static int dst_sreg(struct cg *cg, const struct cvt_operand *o, int line)
{
    if (o->kind != OPR_REG)
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                    "destination is not a register");
    return sreg_of(cg, o->reg, line);
}

static int addr_parts(struct cg *cg, const struct cvt_operand *o, int line,
                      int *base, int *off)
{
    if (o->kind != OPR_ADDR)
        return fail(cg, BRAD_CVT_ERR_TRANSLATION, line,
                    "memory operand is not [register+offset]");

    *base = sreg_of(cg, o->base, line);
    if (*base < 0)
        return -1;
    *off = o->off;

    if (*off < -128 || *off > 127) {
        int t = alloc_temp(cg, line);
        int c;
        if (t < 0)
            return -1;
        if (const_u32(cg, (uint32_t)*off, line, &c) < 0)
            return -1;
        if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)*base, (uint8_t)c,
                 0, 0) < 0)
            return -1;
        *base = t;
        *off = 0;
    }
    return 0;
}

/* Effective address for a 64-bit access: materialise base+offset into one
 * register so the second word can be addressed with the immediate field. */
static int ea64(struct cg *cg, const struct cvt_operand *o, int line, int *ea)
{
    int base, off;
    if (addr_parts(cg, o, line, &base, &off) < 0)
        return -1;
    if (off != 0) {
        int t = alloc_temp(cg, line);
        int c;
        if (t < 0 || const_u32(cg, (uint32_t)off, line, &c) < 0)
            return -1;
        if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)base, (uint8_t)c,
                 0, 0) < 0)
            return -1;
        base = t;
    }
    *ea = base;
    return 0;
}

/* PTX requires an 8-byte-aligned address for a 64-bit global access.
 * BVRT memory is flat, so the check has to happen at run time: trap when
 * the low three address bits are non-zero. */
static int check_align8(struct cg *cg, int ea, int line)
{
    int t = alloc_temp(cg, line);
    int seven;
    if (t < 0 || const_u32(cg, 7, line, &seven) < 0)
        return -1;
    if (emit(cg, BV_OP_AND, 0, (uint8_t)t, (uint8_t)ea, (uint8_t)seven,
             0, 0) < 0)
        return -1;

    int p = ptemp_alloc(cg, line);
    if (p < 0)
        return -1;
    if (emit(cg, BV_OP_CMP, 0, (uint8_t)p, (uint8_t)t, BV_S_ZERO,
             0, BV_CC_EQ) < 0)
        return -1;

    char ok[CVT_NAME];
    internal_label(cg, "align8", ok);
    if (emit_branch(cg, BV_OP_BR_COND, (uint8_t)(p + 1), ok, line) < 0)
        return -1;
    if (emit(cg, BV_OP_TRAP, 0, 0, 0, 0, 0, 0) < 0)
        return -1;
    return define_label(cg, ok, line);
}

/* ─── Float comparison key ───
 *
 * The reference CMP is unsigned.  IEEE-754 binary32 bit patterns do not
 * order correctly as unsigned integers, so each operand is mapped to a
 * monotonic key:  key = (b & 0x80000000) ? ~b : (b | 0x80000000).
 * -0.0 is normalised to +0.0 first so that ±0 compare equal.  NaN
 * operands follow the total order of their bit patterns, not IEEE
 * unordered semantics; that limit is stated in the subset document.
 */
static int float_key(struct cg *cg, const struct cvt_operand *o, int line,
                     int *out)
{
    int s, sign;
    if (src_typed(cg, o, TY_F32, line, &s) < 0)
        return -1;
    if (ensure_sign(cg, line, &sign) < 0)
        return -1;

    int bn = alloc_temp(cg, line);
    if (bn < 0)
        return -1;
    if (emit(cg, BV_OP_IADD, 0, (uint8_t)bn, (uint8_t)s, BV_S_ZERO,
             0, 0) < 0)
        return -1;

    int pz = ptemp_alloc(cg, line);
    if (pz < 0)
        return -1;
    if (emit(cg, BV_OP_CMP, 0, (uint8_t)pz, (uint8_t)s, (uint8_t)sign,
             0, BV_CC_EQ) < 0)
        return -1;
    if (emit(cg, BV_OP_IADD, (uint8_t)(pz + 1), (uint8_t)bn, BV_S_ZERO,
             BV_S_ZERO, 0, 0) < 0)
        return -1;

    int t = alloc_temp(cg, line);
    if (t < 0)
        return -1;
    if (emit(cg, BV_OP_AND, 0, (uint8_t)t, (uint8_t)bn, (uint8_t)sign,
             0, 0) < 0)
        return -1;

    int pn = ptemp_alloc(cg, line);
    int pp = ptemp_alloc(cg, line);
    if (pn < 0 || pp < 0)
        return -1;
    if (emit(cg, BV_OP_CMP, 0, (uint8_t)pn, (uint8_t)t, BV_S_ZERO,
             0, BV_CC_NE) < 0)
        return -1;
    if (emit(cg, BV_OP_CMP, 0, (uint8_t)pp, (uint8_t)t, BV_S_ZERO,
             0, BV_CC_EQ) < 0)
        return -1;

    int k = alloc_temp(cg, line);
    if (k < 0)
        return -1;
    if (emit(cg, BV_OP_NOT, (uint8_t)(pn + 1), (uint8_t)k, (uint8_t)bn,
             0, 0, 0) < 0)
        return -1;
    if (emit(cg, BV_OP_OR, (uint8_t)(pp + 1), (uint8_t)k, (uint8_t)bn,
             (uint8_t)sign, 0, 0) < 0)
        return -1;

    *out = k;
    return 0;
}

/* ─── Instruction lowering ─── */

static int gen_setp(struct cg *cg, const struct cvt_insn *in)
{
    int dp = preg_of(cg, in->dst.reg, in->line);
    if (dp < 0)
        return -1;

    enum cvt_type ty = in->type;
    if (cvt_is_float(ty)) {
        int ka, kb;
        if (float_key(cg, &in->a, in->line, &ka) < 0)
            return -1;
        if (float_key(cg, &in->b, in->line, &kb) < 0)
            return -1;
        return emit(cg, BV_OP_CMP, 0, (uint8_t)dp, (uint8_t)ka,
                    (uint8_t)kb, 0, (uint8_t)in->cc);
    }

    int a, b;
    if (src_typed(cg, &in->a, ty, in->line, &a) < 0)
        return -1;
    if (src_typed(cg, &in->b, ty, in->line, &b) < 0)
        return -1;

    if (ty == TY_S32 || ty == TY_S64) {
        int sign;
        if (ensure_sign(cg, in->line, &sign) < 0)
            return -1;
        int ta = alloc_temp(cg, in->line);
        int tb = alloc_temp(cg, in->line);
        if (ta < 0 || tb < 0)
            return -1;
        if (emit(cg, BV_OP_XOR, 0, (uint8_t)ta, (uint8_t)a, (uint8_t)sign,
                 0, 0) < 0)
            return -1;
        if (emit(cg, BV_OP_XOR, 0, (uint8_t)tb, (uint8_t)b, (uint8_t)sign,
                 0, 0) < 0)
            return -1;
        a = ta;
        b = tb;
    }

    return emit(cg, BV_OP_CMP, 0, (uint8_t)dp, (uint8_t)a, (uint8_t)b,
                0, (uint8_t)in->cc);
}

static int gen_selp(struct cg *cg, const struct cvt_insn *in)
{
    int d = dst_sreg(cg, &in->dst, in->line);
    if (d < 0)
        return -1;
    int cp = preg_of(cg, in->c.reg, in->line);
    if (cp < 0)
        return -1;

    int a, b;
    if (src_typed(cg, &in->a, in->type, in->line, &a) < 0)
        return -1;
    if (src_typed(cg, &in->b, in->type, in->line, &b) < 0)
        return -1;

    /* SEL's condition lives in FLAGS, separate from the execution
     * predicate, so this is a true per-lane select -- no divergent branch. */
    return emit(cg, BV_OP_SEL, 0, (uint8_t)d, (uint8_t)a, (uint8_t)b,
                0, (uint8_t)(cp + 1));
}

static int gen_insn(struct cg *cg, const struct cvt_insn *in)
{
    cg->temp_next = S_TEMP_BASE;
    memset(cg->ptemp_used, 0, sizeof(cg->ptemp_used));

    uint8_t pred = 0;
    if (in->pred >= 0) {
        int p = preg_of(cg, &cg->m->vregs[in->pred], in->line);
        if (p < 0)
            return -1;
        pred = (uint8_t)(p + 1);
    }

    switch (in->op) {
    case OP_LABEL:
        return define_label(cg, in->dst.name, in->line);

    case OP_MOV: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int s;
        if (d < 0 || src_typed(cg, &in->a, in->type, in->line, &s) < 0)
            return -1;

        struct cvt_vreg *dv = in->dst.reg;
        if (!cvt_is_64(dv->type))
            return emit(cg, BV_OP_IADD, pred, (uint8_t)d, (uint8_t)s,
                        BV_S_ZERO, 0, 0);

        /* 64-bit destination: low word, then the high word if the source
         * has a live high half.  Otherwise the destination is
         * zero-extended. */
        if (emit(cg, BV_OP_IADD, pred, (uint8_t)d, (uint8_t)s,
                 BV_S_ZERO, 0, 0) < 0)
            return -1;

        if (cvt_is_64(in->type) && in->a.kind == OPR_REG &&
            cvt_is_64(in->a.reg->type) && !in->a.reg->hi_zero) {
            int sh = sreg_hi(cg, in->a.reg, in->line);
            int dh = sreg_hi(cg, dv, in->line);
            if (sh < 0 || dh < 0)
                return -1;
            dv->hi_zero = 0;
            return emit(cg, BV_OP_IADD, pred, (uint8_t)dh, (uint8_t)sh,
                        BV_S_ZERO, 0, 0);
        }
        return mark_hi_zero(cg, dv, pred, in->line);
    }

    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
    case OP_IADD: case OP_ISUB: case OP_IMUL: case OP_IDIV: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b;
        if (d < 0 ||
            src_typed(cg, &in->a, in->type, in->line, &a) < 0 ||
            src_typed(cg, &in->b, in->type, in->line, &b) < 0)
            return -1;
        uint8_t op;
        switch (in->op) {
        case OP_ADD:  op = BV_OP_ADD;  break;
        case OP_SUB:  op = BV_OP_SUB;  break;
        case OP_MUL:  op = BV_OP_MUL;  break;
        case OP_DIV:  op = BV_OP_DIV;  break;
        case OP_IADD: op = BV_OP_IADD; break;
        case OP_ISUB: op = BV_OP_ISUB; break;
        case OP_IMUL: op = BV_OP_IMUL; break;
        default:      op = BV_OP_IDIV; break;
        }
        if (emit(cg, op, pred, (uint8_t)d, (uint8_t)a, (uint8_t)b, 0, 0) < 0)
            return -1;
        /* No allowed arithmetic op produces a true 64-bit result (64-bit
         * mul/div/bitwise are rejected).  If the destination is a 64-bit
         * register -- e.g. `mul.wide.u32 %rd, %r, 8` -- zero-extend it. */
        return mark_hi_zero(cg, in->dst.reg, pred, in->line);
    }

    case OP_MAD: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b, c;
        if (d < 0 ||
            src_typed(cg, &in->a, in->type, in->line, &a) < 0 ||
            src_typed(cg, &in->b, in->type, in->line, &b) < 0 ||
            src_typed(cg, &in->c, in->type, in->line, &c) < 0)
            return -1;
        uint8_t op = cvt_is_float(in->type) ? BV_OP_MAD : BV_OP_IMAD;
        if (d == c) {
            /* MAD reads dst as the addend before writing it. */
            return emit(cg, op, pred, (uint8_t)d, (uint8_t)a, (uint8_t)b,
                        0, 0);
        }
        int t = alloc_temp(cg, in->line);
        if (t < 0)
            return -1;
        if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)c, BV_S_ZERO,
                 0, 0) < 0)
            return -1;
        if (emit(cg, op, 0, (uint8_t)t, (uint8_t)a, (uint8_t)b, 0, 0) < 0)
            return -1;
        return emit(cg, BV_OP_IADD, pred, (uint8_t)d, (uint8_t)t,
                    BV_S_ZERO, 0, 0);
    }

    case OP_MIN: case OP_MAX: case OP_ABS: case OP_NEG: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b = 0;
        if (d < 0 || src_typed(cg, &in->a, in->type, in->line, &a) < 0)
            return -1;
        int is1 = in->op == OP_ABS || in->op == OP_NEG;
        if (!is1 && src_typed(cg, &in->b, in->type, in->line, &b) < 0)
            return -1;
        uint8_t op = in->op == OP_MIN ? BV_OP_MIN :
                     in->op == OP_MAX ? BV_OP_MAX :
                     in->op == OP_ABS ? BV_OP_ABS : BV_OP_NEG;
        return emit(cg, op, pred, (uint8_t)d, (uint8_t)a, (uint8_t)b, 0, 0);
    }

    case OP_AND: case OP_OR: case OP_XOR: case OP_NOT: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b = 0;
        if (d < 0 || src_typed(cg, &in->a, in->type, in->line, &a) < 0)
            return -1;
        int is1 = in->op == OP_NOT;
        if (!is1 && src_typed(cg, &in->b, in->type, in->line, &b) < 0)
            return -1;
        uint8_t op = in->op == OP_AND ? BV_OP_AND :
                     in->op == OP_OR ? BV_OP_OR :
                     in->op == OP_XOR ? BV_OP_XOR : BV_OP_NOT;
        return emit(cg, op, pred, (uint8_t)d, (uint8_t)a, (uint8_t)b, 0, 0);
    }

    case OP_SHL: case OP_SHR: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b;
        if (d < 0 ||
            src_typed(cg, &in->a, in->type, in->line, &a) < 0 ||
            src_typed(cg, &in->b, in->type, in->line, &b) < 0)
            return -1;
        return emit(cg, in->op == OP_SHL ? BV_OP_SHL : BV_OP_SHR, pred,
                    (uint8_t)d, (uint8_t)a, (uint8_t)b, 0, 0);
    }

    case OP_REM: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a, b;
        if (d < 0 ||
            src_typed(cg, &in->a, in->type, in->line, &a) < 0 ||
            src_typed(cg, &in->b, in->type, in->line, &b) < 0)
            return -1;
        int t = alloc_temp(cg, in->line);
        if (t < 0)
            return -1;
        if (emit(cg, BV_OP_IDIV, 0, (uint8_t)t, (uint8_t)a, (uint8_t)b,
                 0, 0) < 0)
            return -1;
        if (emit(cg, BV_OP_IMUL, 0, (uint8_t)t, (uint8_t)t, (uint8_t)b,
                 0, 0) < 0)
            return -1;
        return emit(cg, BV_OP_ISUB, pred, (uint8_t)d, (uint8_t)a,
                    (uint8_t)t, 0, 0);
    }

    case OP_SETP:
        return gen_setp(cg, in);

    case OP_SELP:
        return gen_selp(cg, in);

    case OP_LOAD: {
        if (in->a.kind == OPR_PARAM) {
            int idx = -1;
            for (int i = 0; i < cg->m->nparams; i++) {
                if (strcmp(cg->m->params[i].name, in->a.name) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
                return fail(cg, BRAD_CVT_ERR_TRANSLATION, in->line,
                            "ld.param of unknown parameter '%s'",
                            in->a.name);
            int d = dst_sreg(cg, &in->dst, in->line);
            if (d < 0)
                return -1;
            if (emit(cg, BV_OP_IADD, pred, (uint8_t)d,
                     (uint8_t)(BV_S_ARG_BASE + idx), BV_S_ZERO, 0, 0) < 0)
                return -1;
            /* Parameters are 32-bit addresses; a 64-bit parameter load
             * zero-extends. */
            return mark_hi_zero(cg, in->dst.reg, pred, in->line);
        }

        int d = dst_sreg(cg, &in->dst, in->line);
        if (d < 0)
            return -1;

        if (in->width == 8) {
            int ea;
            if (ea64(cg, &in->a, in->line, &ea) < 0)
                return -1;
            /* If the destination is also the address base, the low-word
             * load would clobber the address before the high-word load
             * uses it.  Copy the effective address out first. */
            if (ea == d) {
                int t = alloc_temp(cg, in->line);
                if (t < 0)
                    return -1;
                if (emit(cg, BV_OP_IADD, 0, (uint8_t)t, (uint8_t)ea,
                         BV_S_ZERO, 0, 0) < 0)
                    return -1;
                ea = t;
            }
            if (check_align8(cg, ea, in->line) < 0)
                return -1;
            int dh;
            if (mark_hi_live(cg, in->dst.reg, in->line, &dh) < 0)
                return -1;
            if (emit(cg, BV_OP_LOAD, pred, (uint8_t)d, (uint8_t)ea,
                     0, 0, 0) < 0)
                return -1;
            return emit(cg, BV_OP_LOAD, pred, (uint8_t)dh, (uint8_t)ea,
                        0, 4, 0);
        }

        int base, off;
        if (addr_parts(cg, &in->a, in->line, &base, &off) < 0)
            return -1;
        if (emit(cg, BV_OP_LOAD, pred, (uint8_t)d, (uint8_t)base, 0,
                 (int8_t)off, 0) < 0)
            return -1;
        return mark_hi_zero(cg, in->dst.reg, pred, in->line);
    }

    case OP_STORE: {
        if (in->width == 8) {
            int lo;
            if (src_typed(cg, &in->a, in->type, in->line, &lo) < 0)
                return -1;
            int ea;
            if (ea64(cg, &in->dst, in->line, &ea) < 0)
                return -1;
            if (check_align8(cg, ea, in->line) < 0)
                return -1;
            int hi = BV_S_ZERO;
            if (in->a.kind == OPR_REG && cvt_is_64(in->a.reg->type) &&
                !in->a.reg->hi_zero) {
                if (hi_read(cg, in->a.reg, in->line, &hi) < 0)
                    return -1;
            }
            if (emit(cg, BV_OP_STORE, pred, (uint8_t)lo, (uint8_t)ea,
                     0, 0, 0) < 0)
                return -1;
            return emit(cg, BV_OP_STORE, pred, (uint8_t)hi, (uint8_t)ea,
                        0, 4, 0);
        }

        int base, off;
        int v;
        if (addr_parts(cg, &in->dst, in->line, &base, &off) < 0)
            return -1;
        if (src_typed(cg, &in->a, in->type, in->line, &v) < 0)
            return -1;
        return emit(cg, BV_OP_STORE, pred, (uint8_t)v, (uint8_t)base, 0,
                    (int8_t)off, 0);
    }

    case OP_BRA: {
        if (in->pred < 0)
            return emit_branch(cg, BV_OP_BR, 0, in->dst.name, in->line);

        int p = preg_of(cg, &cg->m->vregs[in->pred], in->line);
        if (p < 0)
            return -1;
        if (!in->pred_neg)
            return emit_branch(cg, BV_OP_BR_COND, (uint8_t)(p + 1),
                               in->dst.name, in->line);

        /* @!P bra L  ==  @P bra skip; bra L; skip: */
        char skip[CVT_NAME];
        internal_label(cg, "bra_skip", skip);
        if (emit_branch(cg, BV_OP_BR_COND, (uint8_t)(p + 1), skip,
                        in->line) < 0)
            return -1;
        if (emit_branch(cg, BV_OP_BR, 0, in->dst.name, in->line) < 0)
            return -1;
        return define_label(cg, skip, in->line);
    }

    case OP_RET:
    case OP_EXIT:
        return emit(cg, BV_OP_EXIT, 0, 0, 0, 0, 0, 0);

    case OP_BAR:
        return emit(cg, BV_OP_BAR, 0, 0, 0, 0, 0, 0);

    case OP_TRAP:
        return emit(cg, BV_OP_TRAP, 0, 0, 0, 0, 0, 0);

    case OP_SQRT: case OP_RSQRT: case OP_RCP:
    case OP_EXP2: case OP_LOG2: case OP_SIN: case OP_COS: {
        int d = dst_sreg(cg, &in->dst, in->line);
        int a;
        if (d < 0 || src_typed(cg, &in->a, in->type, in->line, &a) < 0)
            return -1;
        uint8_t op = in->op == OP_SQRT ? BV_OP_SQRT :
                     in->op == OP_RSQRT ? BV_OP_RSQRT :
                     in->op == OP_RCP ? BV_OP_RCP :
                     in->op == OP_EXP2 ? BV_OP_EXP2 :
                     in->op == OP_LOG2 ? BV_OP_LOG2 :
                     in->op == OP_SIN ? BV_OP_SIN : BV_OP_COS;
        return emit(cg, op, pred, (uint8_t)d, (uint8_t)a, 0, 0, 0);
    }

    default:
        return fail(cg, BRAD_CVT_ERR_UNSUPPORTED, in->line,
                    "internal: unhandled IR opcode %d", (int)in->op);
    }
}

/* ─── Patch resolution ─── */

static int resolve_patches(struct cg *cg)
{
    for (int i = 0; i < cg->npatch; i++) {
        const struct patch *pt = &cg->patches[i];
        int target = -1;
        for (int j = 0; j < cg->nlabels; j++) {
            if (strcmp(cg->labels[j].name, pt->name) == 0) {
                target = cg->labels[j].pc;
                break;
            }
        }
        if (target < 0)
            return fail(cg, BRAD_CVT_ERR_TRANSLATION, pt->line,
                        "undefined label '%s'", pt->name);
        int d = target - (pt->pc + 1);
        if (d < -128 || d > 127)
            return fail(cg, BRAD_CVT_ERR_TRANSLATION, pt->line,
                        "branch to '%s' is %d instructions away; the ISA "
                        "has an 8-bit branch offset (limit ±127)",
                        pt->name, d);
        uint64_t w = cg->insns[pt->pc];
        w &= ~(UINT64_C(0xFF) << BV_IMM_SHIFT);
        w |= ((uint64_t)(uint8_t)(int8_t)d) << BV_IMM_SHIFT;
        cg->insns[pt->pc] = w;
    }
    return 0;
}

/* ─── Entry point ─── */

int cvt_codegen(struct cvt_module *m, void **out, size_t *out_len,
                struct brad_cvt_kernel_info *info, struct cvt_diag *diag)
{
    struct cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.m = m;
    cg.diag = diag;
    cg.status = BRAD_CVT_ERR_TRANSLATION;
    cg.one_sreg = -1;
    cg.sign_sreg = -1;
    cg.vreg_next = S_VREG_BASE;
    cg.temp_next = S_TEMP_BASE;
    cg.const_next = S_CONST_BASE;

    cg.insns = calloc(BVBC_MAX_INSN, sizeof(*cg.insns));
    if (!cg.insns) {
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "out of memory");
        return BRAD_CVT_ERR_MEMORY;
    }

    for (int i = 0; i < m->ninsns; i++) {
        if (gen_insn(&cg, &m->insns[i]) < 0)
            break;
    }

    /* A landing pad for labels that sit at the end of the kernel. */
    if (!cg.failed)
        emit(&cg, BV_OP_EXIT, 0, 0, 0, 0, 0, 0);

    if (!cg.failed)
        resolve_patches(&cg);

    if (cg.failed) {
        free(cg.insns);
        return cg.status;
    }

    struct bvbc_image img;
    memset(&img, 0, sizeof(img));
    img.insns = cg.insns;
    img.ninsn = (uint32_t)cg.n;
    if (bvbc_add_kernel(&img, m->name, 0, (uint32_t)m->nparams) < 0) {
        free(cg.insns);
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg),
                 "cannot add kernel '%s' to image", m->name);
        return BRAD_CVT_ERR_TRANSLATION;
    }

    size_t sz = bvbc_serialize_size(&img);
    uint8_t *buf = malloc(sz);
    if (!buf) {
        free(cg.insns);
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "out of memory");
        return BRAD_CVT_ERR_MEMORY;
    }
    if (bvbc_pack(&img, buf, sz) != sz) {
        free(buf);
        free(cg.insns);
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "container serialization failed");
        return BRAD_CVT_ERR_TRANSLATION;
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", m->name);
        info->num_regs = (unsigned)(cg.max_sreg + 1);
        info->shared_mem_bytes = m->shared_bytes;
        info->thread_count = 32;
        info->block_count = 1;
        info->instruction_count = cg.n;
    }

    free(cg.insns);
    *out = buf;
    *out_len = sz;
    return BRAD_CVT_OK;
}

int cvt_ptx_translate(const char *src, size_t len,
                      void **out, size_t *out_len,
                      struct brad_cvt_kernel_info *info,
                      struct cvt_diag *diag)
{
    struct cvt_module m;
    struct cvt_diag d;
    int st;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len) {
        if (diag) {
            diag->line = 0;
            snprintf(diag->msg, sizeof(diag->msg),
                     "translate requires out and out_len");
        }
        return BRAD_CVT_ERR_PARSE;
    }
    if (!diag)
        diag = &d;

    st = cvt_parse(src, len, &m, diag);
    if (st != 0)
        return st;

    st = cvt_codegen(&m, out, out_len, info, diag);
    free(m.insns);
    return st;
}
