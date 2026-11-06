#include "ptx.h"
#include "brad/bradvector.h"   /* BV_CC_* */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    struct cvt_lexer  lx;
    struct cvt_module *m;
    struct cvt_diag   *diag;
    int                failed;
    int                status;
    int                seen_entry;
};

/* ─── Diagnostics ─── */

static int fail(struct parser *p, int status, int line, const char *fmt, ...)
{
    if (!p->failed) {
        va_list ap;
        p->failed = 1;
        p->status = status;
        p->diag->line = line;
        va_start(ap, fmt);
        vsnprintf(p->diag->msg, sizeof(p->diag->msg), fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* The IR stores names in CVT_NAME-byte fields.  Truncating an identifier
 * would let two symbols alias, so a too-long name is a parse error. */
static int set_name(struct parser *p, char *dst, size_t cap,
                    const char *src, int line)
{
    size_t n = strlen(src);
    if (n >= cap)
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "identifier '%s' is longer than %zu characters",
                    src, cap - 1);
    memcpy(dst, src, n + 1);
    return 0;
}

/* ─── Token helpers ─── */

static void p_next(struct parser *p)
{
    cvt_lex_next(&p->lx);
}

static int is_punct(const struct parser *p, char c)
{
    return p->lx.t.kind == TOK_PUNCT && p->lx.t.text[0] == c;
}

static int is_ident(const struct parser *p, const char *s)
{
    return p->lx.t.kind == TOK_IDENT && strcmp(p->lx.t.text, s) == 0;
}

static int accept_punct(struct parser *p, char c)
{
    if (!is_punct(p, c))
        return 0;
    p_next(p);
    return 1;
}

static void skip_nl(struct parser *p)
{
    while (p->lx.t.kind == TOK_NL)
        p_next(p);
}

static void skip_seps(struct parser *p)
{
    while (p->lx.t.kind == TOK_NL || is_punct(p, ';'))
        p_next(p);
}

/* Consume the rest of the current line (and the newline). */
static void skip_line(struct parser *p)
{
    while (p->lx.t.kind != TOK_NL && p->lx.t.kind != TOK_EOF)
        p_next(p);
    if (p->lx.t.kind == TOK_NL)
        p_next(p);
}

/* Consume a metadata directive's operands but stop before `{`. */
static void skip_attr(struct parser *p)
{
    while (p->lx.t.kind != TOK_NL && p->lx.t.kind != TOK_EOF &&
           !is_punct(p, '{'))
        p_next(p);
    if (p->lx.t.kind == TOK_NL)
        p_next(p);
}

/* ─── Types ─── */

static enum cvt_type type_of(const char *s)
{
    if (!strcmp(s, ".b32")) return TY_B32;
    if (!strcmp(s, ".u32")) return TY_U32;
    if (!strcmp(s, ".s32")) return TY_S32;
    if (!strcmp(s, ".f32")) return TY_F32;
    if (!strcmp(s, ".pred")) return TY_PRED;
    if (!strcmp(s, ".b64")) return TY_B64;
    if (!strcmp(s, ".u64")) return TY_U64;
    if (!strcmp(s, ".s64")) return TY_S64;
    if (!strcmp(s, ".f64")) return TY_F64;
    return TY_NONE;
}

static int type_bytes(enum cvt_type t)
{
    switch (t) {
    case TY_B64: case TY_U64: case TY_S64: case TY_F64: return 8;
    case TY_B32: case TY_U32: case TY_S32: case TY_F32: return 4;
    default: return 0;
    }
}

/* Register declarations and operands carry a leading dot (".b32"); the
 * mnemonic's type segments do not ("ld.global.f32").  Accept both. */
static enum cvt_type type_seg(const char *s)
{
    if (s[0] == '.')
        return type_of(s);
    if (!strcmp(s, "b32")) return TY_B32;
    if (!strcmp(s, "u32")) return TY_U32;
    if (!strcmp(s, "s32")) return TY_S32;
    if (!strcmp(s, "f32")) return TY_F32;
    if (!strcmp(s, "b64")) return TY_B64;
    if (!strcmp(s, "u64")) return TY_U64;
    if (!strcmp(s, "s64")) return TY_S64;
    if (!strcmp(s, "f64")) return TY_F64;
    return TY_NONE;
}

/* ─── Module tables ─── */

static struct cvt_vreg *find_vreg(struct parser *p, const char *name)
{
    for (int i = 0; i < p->m->nvregs; i++)
        if (strcmp(p->m->vregs[i].name, name) == 0)
            return &p->m->vregs[i];
    return NULL;
}

static struct cvt_vreg *add_vreg(struct parser *p, const char *name,
                                 enum cvt_type type, int line)
{
    if (find_vreg(p, name)) {
        fail(p, BRAD_CVT_ERR_PARSE, line, "register '%s' declared twice", name);
        return NULL;
    }
    if (p->m->nvregs >= CVT_MAX_VREGS) {
        fail(p, BRAD_CVT_ERR_TRANSLATION, line, "too many virtual registers");
        return NULL;
    }
    struct cvt_vreg *v = &p->m->vregs[p->m->nvregs++];
    memset(v, 0, sizeof(*v));
    if (set_name(p, v->name, sizeof(v->name), name, line) < 0)
        return NULL;
    v->type = type;
    v->sreg = -1;
    v->sreg_hi = -1;
    v->hi_zero = 1;
    v->preg = -1;
    return v;
}

static struct cvt_insn *add_ir(struct parser *p, enum cvt_op op,
                               enum cvt_type type, int line)
{
    if (p->m->ninsns >= CVT_MAX_IR) {
        fail(p, BRAD_CVT_ERR_TRANSLATION, line, "IR limit reached");
        return NULL;
    }
    struct cvt_insn *in = &p->m->insns[p->m->ninsns++];
    memset(in, 0, sizeof(*in));
    in->op = op;
    in->type = type;
    in->line = line;
    in->cc = -1;
    in->pred = -1;
    return in;
}

/* ─── Operands ─── */

static int parse_immediate(struct parser *p, struct cvt_operand *o, int line)
{
    const char *s = p->lx.t.text;

    o->kind = OPR_IMM;
    o->imm_float = 0;

    if (s[0] == '0' && (s[1] == 'f' || s[1] == 'F')) {
        if (strlen(s) != 10) {
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "malformed float immediate '%s' (want 0fXXXXXXXX)", s);
        }
        char *end = NULL;
        unsigned long v = strtoul(s + 2, &end, 16);
        if (!end || *end) {
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "malformed float immediate '%s'", s);
        }
        o->imm = (uint32_t)v;
        o->imm_float = 1;
        p_next(p);
        return 0;
    }
    if (s[0] == '0' && (s[1] == 'd' || s[1] == 'D')) {
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "double-precision immediate '%s' is outside the v0 subset", s);
    }
    if (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E')) {
        char *end = NULL;
        float f = strtof(s, &end);
        if (!end || *end) {
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "malformed float immediate '%s'", s);
        }
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        o->imm = bits;
        o->imm_float = 1;
        p_next(p);
        return 0;
    }

    int base = 10;
    const char *digits = s;
    int neg = 0;
    if (*digits == '-' || *digits == '+') {
        neg = (*digits == '-');
        digits++;
    }
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits += 2;
    } else if (digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
        /* strtoll has no binary base; do it by hand. */
        uint64_t v = 0;
        const char *q = digits + 2;
        if (!*q)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "malformed immediate '%s'", s);
        for (; *q; q++) {
            if (*q != '0' && *q != '1')
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "malformed binary immediate '%s'", s);
            v = (v << 1) | (uint64_t)(*q - '0');
        }
        o->imm = neg ? (uint64_t)(-(int64_t)v) : v;
        p_next(p);
        return 0;
    }

    char *end = NULL;
    long long v = strtoll(digits, &end, base);
    if (!end || *end || end == digits) {
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "malformed immediate '%s'", s);
    }
    o->imm = neg ? (uint64_t)(-v) : (uint64_t)v;
    p_next(p);
    return 0;
}

static int parse_operand(struct parser *p, struct cvt_operand *o, int line)
{
    memset(o, 0, sizeof(*o));

    if (p->lx.t.kind == TOK_NUMBER)
        return parse_immediate(p, o, line);

    if (p->lx.t.kind == TOK_PUNCT && p->lx.t.text[0] == '[') {
        p_next(p);   /* '[' */

        if (p->lx.t.kind == TOK_IDENT && p->lx.t.text[0] == '%') {
            struct cvt_vreg *v = find_vreg(p, p->lx.t.text);
            if (!v) {
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "undeclared register '%s'", p->lx.t.text);
            }
            if (v->type == TY_PRED) {
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "predicate '%s' cannot be an address base",
                            v->name);
            }
            o->kind = OPR_ADDR;
            o->base = v;
            o->off = 0;
            p_next(p);

            if (p->lx.t.kind == TOK_NUMBER) {
                char *end = NULL;
                long v2 = strtol(p->lx.t.text, &end, 10);
                if (!end || *end) {
                    return fail(p, BRAD_CVT_ERR_PARSE, line,
                                "malformed address offset '%s'",
                                p->lx.t.text);
                }
                if (v2 < INT32_MIN || v2 > INT32_MAX) {
                    return fail(p, BRAD_CVT_ERR_TRANSLATION, line,
                                "address offset %ld is out of range", v2);
                }
                o->off = (int32_t)v2;
                p_next(p);
            }
            if (!accept_punct(p, ']')) {
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "expected ']' after address");
            }
            return 0;
        }

        if (p->lx.t.kind == TOK_IDENT) {
            o->kind = OPR_PARAM;
            if (set_name(p, o->name, sizeof(o->name), p->lx.t.text, line) < 0)
                return -1;
            p_next(p);
            if (!accept_punct(p, ']')) {
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "expected ']' after symbol");
            }
            return 0;
        }

        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "expected a register or symbol inside '[]'");
    }

    if (p->lx.t.kind == TOK_IDENT) {
        const char *s = p->lx.t.text;

        if (s[0] == '%') {
            if (!strcmp(s, "%tid.x") || !strcmp(s, "%ctaid.x") ||
                !strcmp(s, "%ntid.x") || !strcmp(s, "%nctaid.x") ||
                !strcmp(s, "%laneid")) {
                o->kind = OPR_SPECIAL;
                if (set_name(p, o->name, sizeof(o->name), s, line) < 0)
                    return -1;
                p_next(p);
                return 0;
            }
            if (!strncmp(s, "%tid.", 5) || !strncmp(s, "%ctaid.", 7) ||
                !strncmp(s, "%ntid.", 6) || !strncmp(s, "%nctaid.", 8)) {
                return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                            "'%s' is outside the v0 one-warp launch model", s);
            }
            struct cvt_vreg *v = find_vreg(p, s);
            if (!v) {
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "undeclared register '%s'", s);
            }
            o->kind = OPR_REG;
            o->reg = v;
            p_next(p);
            return 0;
        }

        /* A bare symbol is a branch target (or a global symbol, which the
         * instruction mapper rejects where it is not valid). */
        o->kind = OPR_LABEL;
        if (set_name(p, o->name, sizeof(o->name), s, line) < 0)
            return -1;
        p_next(p);
        return 0;
    }

    if (is_punct(p, '{')) {
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "vector operands ({...}) are outside the v0 subset");
    }

    return fail(p, BRAD_CVT_ERR_PARSE, line, "expected an operand");
}

/* ─── Mnemonic splitting ─── */

static int split_dots(const char *s, char seg[][32], int max)
{
    int n = 0;
    const char *q = s;
    while (n < max) {
        const char *dot = strchr(q, '.');
        size_t len = dot ? (size_t)(dot - q) : strlen(q);
        if (len == 0 || len >= 32)
            return -1;
        memcpy(seg[n], q, len);
        seg[n][len] = '\0';
        n++;
        if (!dot)
            break;
        q = dot + 1;
    }
    return n;
}

static int has_seg(char seg[][32], int n, const char *s)
{
    for (int i = 0; i < n; i++)
        if (strcmp(seg[i], s) == 0)
            return 1;
    return 0;
}

/* First type-looking segment, or TY_NONE. */
static enum cvt_type first_type(char seg[][32], int n)
{
    for (int i = 1; i < n; i++) {
        enum cvt_type t = type_seg(seg[i]);
        if (t != TY_NONE)
            return t;
    }
    return TY_NONE;
}

static int setp_cc(const char *s)
{
    if (!strcmp(s, "eq")) return BV_CC_EQ;
    if (!strcmp(s, "ne")) return BV_CC_NE;
    if (!strcmp(s, "lt") || !strcmp(s, "lo")) return BV_CC_LT;
    if (!strcmp(s, "le") || !strcmp(s, "ls")) return BV_CC_LE;
    if (!strcmp(s, "gt") || !strcmp(s, "hi")) return BV_CC_GT;
    if (!strcmp(s, "ge") || !strcmp(s, "hs")) return BV_CC_GE;
    return -1;   /* equ/neu/ltu/... are unordered float forms: unsupported */
}

/* ─── Instruction mapping ─── */

static int map_instruction(struct parser *p, const char *mn,
                           struct cvt_operand *op, int nops,
                           int line, int pred, int pred_neg)
{
    char seg[8][32];
    int nseg = split_dots(mn, seg, 8);
    if (nseg < 1) {
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "malformed mnemonic '%s'", mn);
    }

    const char *base = seg[0];
    enum cvt_type ty = first_type(seg, nseg);

    if (pred_neg && strcmp(base, "bra") != 0)
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "'@!%%p %s' is outside the v0 subset (no inverted "
                    "execution mask in the reference ISA)", base);

    if (!strcmp(base, "mov")) {
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "mov expects 2 operands");
        if (ty == TY_PRED ||
            (op[1].kind == OPR_REG && op[1].reg->type == TY_PRED))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "predicate moves are outside the v0 subset");
        struct cvt_insn *in = add_ir(p, OP_MOV, ty, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "add") || !strcmp(base, "sub") ||
        !strcmp(base, "mul") || !strcmp(base, "div")) {
        if (nops != 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects 3 operands", base);
        int is_f = cvt_is_float(ty);
        if (ty == TY_F64)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "double-precision %s is outside the v0 subset", base);
        if (cvt_is_64(ty) && (strcmp(base, "mul") == 0 ||
                              strcmp(base, "div") == 0))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit %s is outside the 32-bit reference model "
                        "(add/sub are address arithmetic)", base);
        if (!strcmp(base, "div") && !is_f && ty == TY_S32)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "signed integer div is unsigned in the reference ISA");
        enum cvt_op o;
        if (!strcmp(base, "add")) o = is_f ? OP_ADD : OP_IADD;
        else if (!strcmp(base, "sub")) o = is_f ? OP_SUB : OP_ISUB;
        else if (!strcmp(base, "mul")) o = is_f ? OP_MUL : OP_IMUL;
        else o = is_f ? OP_DIV : OP_IDIV;
        struct cvt_insn *in = add_ir(p, o,
                                     ty == TY_NONE ? TY_B32 : ty, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->b = op[2];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "fma") || !strcmp(base, "mad")) {
        if (nops != 4)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects 4 operands", base);
        if (ty == TY_F64)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "double-precision %s is outside the v0 subset", base);
        if (cvt_is_64(ty))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit %s is outside the 32-bit reference model",
                        base);
        struct cvt_insn *in = add_ir(p, OP_MAD,
                                     ty == TY_NONE ? TY_F32 : ty, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->b = op[2];
        in->c = op[3];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "min") || !strcmp(base, "max") ||
        !strcmp(base, "abs") || !strcmp(base, "neg")) {
        int is1 = !strcmp(base, "abs") || !strcmp(base, "neg");
        if (nops != (is1 ? 2 : 3))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects %d operands", base, is1 ? 2 : 3);
        if (ty != TY_F32)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "%s.%s is outside the v0 subset (f32 only)",
                        base, ty == TY_NONE ? "?" : "non-f32");
        enum cvt_op o = !strcmp(base, "min") ? OP_MIN :
                        !strcmp(base, "max") ? OP_MAX :
                        !strcmp(base, "abs") ? OP_ABS : OP_NEG;
        struct cvt_insn *in = add_ir(p, o, TY_F32, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        if (!is1) in->b = op[2];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "and") || !strcmp(base, "or") ||
        !strcmp(base, "xor") || !strcmp(base, "not")) {
        int is1 = !strcmp(base, "not");
        if (nops != (is1 ? 2 : 3))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects %d operands", base, is1 ? 2 : 3);
        if (ty == TY_PRED)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "predicate logic is outside the v0 subset");
        if (cvt_is_float(ty) || cvt_is_64(ty))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "%s operates on 32-bit integers in the reference "
                        "ISA; float and 64-bit forms are outside the v0 "
                        "subset", base);
        enum cvt_op o = !strcmp(base, "and") ? OP_AND :
                        !strcmp(base, "or") ? OP_OR :
                        !strcmp(base, "xor") ? OP_XOR : OP_NOT;
        struct cvt_insn *in = add_ir(p, o, TY_B32, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        if (!is1) in->b = op[2];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "shl") || !strcmp(base, "shr") ||
        !strcmp(base, "rem")) {
        if (nops != 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects 3 operands", base);
        if (cvt_is_64(ty))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit %s is outside the 32-bit reference model",
                        base);
        if (cvt_is_float(ty))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "%s on float data is outside the v0 subset", base);
        if (ty == TY_S32 && !strcmp(base, "shr"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "arithmetic shr.s32 is not in the reference ISA (SHR is logical)");
        if (ty == TY_S32 && !strcmp(base, "rem"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "signed rem.s32 is outside the v0 subset");
        enum cvt_op o = !strcmp(base, "shl") ? OP_SHL :
                        !strcmp(base, "shr") ? OP_SHR : OP_REM;
        struct cvt_insn *in = add_ir(p, o, TY_B32, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->b = op[2];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "setp")) {
        if (nseg < 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "setp needs a condition and a type");
        int cc = setp_cc(seg[1]);
        if (cc < 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "setp condition '.%s' is outside the v0 subset",
                        seg[1]);
        enum cvt_type t = first_type(seg, nseg);
        if (cvt_is_64(t))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit setp is outside the 32-bit reference model");
        if (t == TY_NONE)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "setp needs a type (.f32/.u32/.s32)");
        if (t == TY_PRED)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "setp on a predicate is outside the v0 subset");
        if (nops != 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "setp expects 3 operands");
        if (pred >= 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "a predicated setp is outside the v0 subset");
        if (op[0].kind != OPR_REG || op[0].reg->type != TY_PRED)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "setp destination must be a predicate register");
        struct cvt_insn *in = add_ir(p, OP_SETP, t, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->b = op[2];
        in->cc = cc;
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "selp")) {
        if (nops != 4)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "selp expects 4 operands");
        if (op[3].kind != OPR_REG || op[3].reg->type != TY_PRED)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "selp condition must be a predicate register");
        if (pred >= 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "a predicated selp is outside the v0 subset");
        if (cvt_is_64(ty))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit selp is outside the 32-bit reference model");
        if (ty == TY_PRED)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "selp on a predicate is outside the v0 subset");
        struct cvt_insn *in = add_ir(p, OP_SELP,
                                     ty == TY_NONE ? TY_B32 : ty, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->b = op[2];
        in->c = op[3];
        return 0;
    }

    if (!strcmp(base, "ld")) {
        if (nseg < 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "ld needs an address space and a type");
        const char *space = seg[1];
        enum cvt_type t = first_type(seg, nseg);
        if (t == TY_NONE)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "ld needs a type");
        if (has_seg(seg, nseg, "v2") || has_seg(seg, nseg, "v4"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "vector loads are outside the v0 subset");
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "ld expects 2 operands");

        if (!strcmp(space, "param")) {
            if (op[1].kind != OPR_PARAM)
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "ld.param expects [symbol]");
            struct cvt_insn *in = add_ir(p, OP_LOAD, t, line);
            if (!in) return -1;
            in->dst = op[0];
            in->a = op[1];
            in->width = 4;
            in->pred = pred;
            in->pred_neg = pred_neg;
            return 0;
        }

        if (!strcmp(space, "global")) {
            if (t == TY_F64)
                return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                            "ld.global.f64 is outside the subset (no double support)");
            if (t != TY_F32 && t != TY_U32 && t != TY_S32 && t != TY_B32 &&
                t != TY_U64 && t != TY_S64 && t != TY_B64)
                return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                            "ld.global of this type is outside the subset");
            if (cvt_is_64(t) && pred >= 0)
                return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                            "predicated 64-bit loads are outside the subset "
                            "(the alignment check is unconditional)");
            if (op[1].kind != OPR_ADDR)
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            "ld.global expects [register+offset]");
            struct cvt_insn *in = add_ir(p, OP_LOAD, t, line);
            if (!in) return -1;
            in->dst = op[0];
            in->a = op[1];
            in->width = cvt_is_64(t) ? 8 : 4;
            in->pred = pred;
            in->pred_neg = pred_neg;
            return 0;
        }

        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "ld.%s is outside the v0 subset", space);
    }

    if (!strcmp(base, "st")) {
        if (nseg < 3)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "st needs an address space and a type");
        const char *space = seg[1];
        enum cvt_type t = first_type(seg, nseg);
        if (strcmp(space, "global"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "st.%s is outside the v0 subset", space);
        if (t == TY_F64)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "st.global.f64 is outside the subset (no double support)");
        if (t != TY_F32 && t != TY_U32 && t != TY_S32 && t != TY_B32 &&
            t != TY_U64 && t != TY_S64 && t != TY_B64)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "st.global of this type is outside the subset");
        if (cvt_is_64(t) && pred >= 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "predicated 64-bit stores are outside the subset "
                        "(the alignment check is unconditional)");
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "st expects 2 operands");
        if (op[0].kind != OPR_ADDR)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "st.global expects [register+offset]");
        struct cvt_insn *in = add_ir(p, OP_STORE, t, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->width = cvt_is_64(t) ? 8 : 4;
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "bra")) {
        if (nops != 1 || op[0].kind != OPR_LABEL)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "bra expects a label");
        struct cvt_insn *in = add_ir(p, OP_BRA, TY_NONE, line);
        if (!in) return -1;
        in->dst = op[0];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "ret") || !strcmp(base, "exit") ||
        !strcmp(base, "trap")) {
        if (nops != 0)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s takes no operands", base);
        if (pred >= 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "a predicated %s is outside the v0 subset", base);
        enum cvt_op o = !strcmp(base, "ret") ? OP_RET :
                        !strcmp(base, "exit") ? OP_EXIT : OP_TRAP;
        struct cvt_insn *in = add_ir(p, o, TY_NONE, line);
        if (!in) return -1;
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "bar") || !strcmp(base, "barrier")) {
        if (!has_seg(seg, nseg, "sync"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "%s.%s is outside the v0 subset", base,
                        nseg > 1 ? seg[1] : "?");
        if (pred >= 0)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "a predicated barrier is outside the v0 subset");
        struct cvt_insn *in = add_ir(p, OP_BAR, TY_NONE, line);
        if (!in) return -1;
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "cvta")) {
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "cvta expects 2 operands");
        if (!has_seg(seg, nseg, "global"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "cvta to a non-global space is outside the v0 subset");
        struct cvt_insn *in = add_ir(p, OP_MOV, TY_U64, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "cvt")) {
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line, "cvt expects 2 operands");
        enum cvt_type dt = TY_NONE, st = TY_NONE;
        for (int i = 1; i < nseg; i++) {
            enum cvt_type t = type_seg(seg[i]);
            if (t == TY_NONE) continue;
            if (dt == TY_NONE) dt = t;
            else if (st == TY_NONE) st = t;
        }
        if (dt == TY_NONE || st == TY_NONE)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "cvt needs source and destination types");
        if (cvt_is_float(dt) != cvt_is_float(st))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "int<->float conversion is outside the v0 subset "
                        "(the reference ISA has no CVT)");
        if (type_bytes(dt) != type_bytes(st) && (type_bytes(dt) != 4 || type_bytes(st) != 4))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "64-bit cvt is outside the v0 subset");
        struct cvt_insn *in = add_ir(p, OP_MOV, dt, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "sqrt") || !strcmp(base, "rsqrt") ||
        !strcmp(base, "rcp") || !strcmp(base, "ex2") ||
        !strcmp(base, "lg2") || !strcmp(base, "sin") ||
        !strcmp(base, "cos")) {
        if (nops != 2)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "%s expects 2 operands", base);
        enum cvt_op o = !strcmp(base, "sqrt") ? OP_SQRT :
                        !strcmp(base, "rsqrt") ? OP_RSQRT :
                        !strcmp(base, "rcp") ? OP_RCP :
                        !strcmp(base, "ex2") ? OP_EXP2 :
                        !strcmp(base, "lg2") ? OP_LOG2 :
                        !strcmp(base, "sin") ? OP_SIN : OP_COS;
        struct cvt_insn *in = add_ir(p, o, TY_F32, line);
        if (!in) return -1;
        in->dst = op[0];
        in->a = op[1];
        in->pred = pred;
        in->pred_neg = pred_neg;
        return 0;
    }

    if (!strcmp(base, "membar") || !strcmp(base, "fence") ||
        !strcmp(base, "atom") || !strcmp(base, "red") ||
        !strcmp(base, "shfl") || !strcmp(base, "vote") ||
        !strcmp(base, "ldmatrix") || !strcmp(base, "cp") ||
        !strcmp(base, "mma")) {
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "'%s' is outside the v0 subset", mn);
    }

    return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                "unsupported instruction '%s'", mn);
}

/* ─── Statements ─── */

static int parse_reg_decl(struct parser *p)
{
    int line = p->lx.t.line;
    p_next(p);   /* .reg */

    if (is_ident(p, ".align")) {
        p_next(p);
        if (p->lx.t.kind != TOK_NUMBER)
            return fail(p, BRAD_CVT_ERR_PARSE, line, ".align needs a number");
        p_next(p);
    }

    if (p->lx.t.kind != TOK_IDENT || p->lx.t.text[0] != '.')
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    ".reg needs a type (.b32, .f32, .pred, ...)");
    enum cvt_type ty = type_of(p->lx.t.text);
    if (ty == TY_NONE)
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "register type '%s' is outside the v0 subset",
                    p->lx.t.text);
    if (ty == TY_F64)
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "double-precision registers are outside the v0 subset");
    p_next(p);

    if (p->lx.t.kind != TOK_IDENT || p->lx.t.text[0] != '%')
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    ".reg needs a %name");
    char base[CVT_NAME];
    if (set_name(p, base, sizeof(base), p->lx.t.text, line) < 0)
        return -1;
    p_next(p);

    int count = 1;
    int explicit_count = 0;
    if (accept_punct(p, '<')) {
        explicit_count = 1;
        if (p->lx.t.kind != TOK_NUMBER)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected a register count");
        count = atoi(p->lx.t.text);
        p_next(p);
        if (!accept_punct(p, '>'))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected '>' after register count");
    }
    if (is_punct(p, '['))
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "register arrays are outside the v0 subset");
    if (count <= 0 || count > CVT_MAX_VREGS - p->m->nvregs)
        return fail(p, BRAD_CVT_ERR_TRANSLATION, line,
                    "bad register count %d", count);

    for (int i = 0; i < count; i++) {
        char name[CVT_NAME + 16];
        if (explicit_count)
            snprintf(name, sizeof(name), "%s%d", base, i);
        else
            snprintf(name, sizeof(name), "%s", base);
        if (!add_vreg(p, name, ty, line))
            return -1;
    }

    while (p->lx.t.kind != TOK_NL && p->lx.t.kind != TOK_EOF &&
           !is_punct(p, ';'))
        p_next(p);
    skip_seps(p);
    return 0;
}

static int parse_shared_decl(struct parser *p)
{
    int line = p->lx.t.line;
    p_next(p);   /* .shared */

    if (is_ident(p, ".align")) {
        p_next(p);
        if (p->lx.t.kind != TOK_NUMBER)
            return fail(p, BRAD_CVT_ERR_PARSE, line, ".align needs a number");
        p_next(p);
    }
    if (is_ident(p, ".extern"))
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "extern shared memory is outside the v0 subset");
    if (p->lx.t.kind != TOK_IDENT || p->lx.t.text[0] != '.')
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    ".shared needs a type");
    enum cvt_type ty = type_of(p->lx.t.text);
    int tb = type_bytes(ty);
    if (tb == 0)
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "shared type '%s' is outside the v0 subset",
                    p->lx.t.text);
    p_next(p);

    if (p->lx.t.kind != TOK_IDENT)
        return fail(p, BRAD_CVT_ERR_PARSE, line, ".shared needs a name");
    p_next(p);

    unsigned n = 1;
    if (accept_punct(p, '[')) {
        if (p->lx.t.kind != TOK_NUMBER)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "only constant shared-memory sizes are supported");
        n = (unsigned)strtoul(p->lx.t.text, NULL, 10);
        p_next(p);
        if (!accept_punct(p, ']'))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected ']' after shared-memory size");
    }
    if (n > 0 && tb > 0 && n > (UINT32_MAX / (unsigned)tb))
        return fail(p, BRAD_CVT_ERR_TRANSLATION, line,
                    "shared-memory size overflows");
    p->m->shared_bytes += n * (unsigned)tb;

    while (p->lx.t.kind != TOK_NL && p->lx.t.kind != TOK_EOF &&
           !is_punct(p, ';'))
        p_next(p);
    skip_seps(p);
    return 0;
}

static int parse_body_item(struct parser *p)
{
    if (p->lx.t.kind == TOK_EOF)
        return fail(p, BRAD_CVT_ERR_PARSE, p->lx.t.line,
                    "unexpected end of file inside .entry");

    if (p->lx.t.kind == TOK_IDENT && p->lx.t.text[0] == '.') {
        const char *d = p->lx.t.text;

        if (!strcmp(d, ".reg"))
            return parse_reg_decl(p);
        if (!strcmp(d, ".shared"))
            return parse_shared_decl(p);
        if (!strcmp(d, ".local") || !strcmp(d, ".const") ||
            !strcmp(d, ".global") || !strcmp(d, ".extern"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, p->lx.t.line,
                        "'%s' declarations are outside the v0 subset", d);
        if (!strcmp(d, ".maxntid") || !strcmp(d, ".reqntid") ||
            !strcmp(d, ".minnctapersm") || !strcmp(d, ".maxnreg") ||
            !strcmp(d, ".pragma") || !strcmp(d, ".loc") ||
            !strcmp(d, ".file") || !strcmp(d, ".section")) {
            skip_attr(p);
            return 0;
        }
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, p->lx.t.line,
                    "directive '%s' is outside the v0 subset", d);
    }

    /* Mission label: IDENT ':' */
    if (p->lx.t.kind == TOK_IDENT) {
        struct cvt_lexer saved = p->lx;
        int line = p->lx.t.line;
        char name[CVT_NAME];
        if (strlen(p->lx.t.text) >= sizeof(name))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "label name is too long");
        memcpy(name, p->lx.t.text, strlen(p->lx.t.text) + 1);
        p_next(p);
        int is_label = is_punct(p, ':');
        p->lx = saved;
        if (is_label) {
            p_next(p);   /* name */
            p_next(p);   /* ':' */
            struct cvt_insn *in = add_ir(p, OP_LABEL, TY_NONE, line);
            if (!in) return -1;
            snprintf(in->dst.name, sizeof(in->dst.name), "%s", name);
            in->dst.kind = OPR_LABEL;
            skip_seps(p);
            return 0;
        }
    }

    /* Instruction. */
    int line = p->lx.t.line;
    int pred = -1, pred_neg = 0;

    if (accept_punct(p, '@')) {
        if (accept_punct(p, '!'))
            pred_neg = 1;
        if (p->lx.t.kind != TOK_IDENT || p->lx.t.text[0] != '%')
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected a predicate after '@'");
        struct cvt_vreg *v = find_vreg(p, p->lx.t.text);
        if (!v || v->type != TY_PRED)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "'%s' is not a declared predicate", p->lx.t.text);
        pred = (int)(v - p->m->vregs);
        p_next(p);
    }

    if (p->lx.t.kind != TOK_IDENT)
        return fail(p, BRAD_CVT_ERR_PARSE, line, "expected an instruction");
    char mn[128];
    snprintf(mn, sizeof(mn), "%s", p->lx.t.text);
    p_next(p);

    struct cvt_operand ops[4];
    memset(ops, 0, sizeof(ops));
    int nops = 0;

    while (p->lx.t.kind != TOK_EOF && !is_punct(p, ';') &&
           !is_punct(p, '}') && p->lx.t.kind != TOK_NL) {
        if (nops >= 4)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "'%s' has too many operands", mn);
        if (parse_operand(p, &ops[nops], line) < 0)
            return -1;
        nops++;
        if (accept_punct(p, ','))
            continue;
        break;
    }

    if (is_punct(p, ';'))
        p_next(p);

    return map_instruction(p, mn, ops, nops, line, pred, pred_neg);
}

static int parse_entry(struct parser *p)
{
    int line = p->lx.t.line;
    p_next(p);   /* .entry */

    if (p->m->name[0])
        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                    "multiple .entry kernels are outside the v0 subset "
                    "(translate one kernel per image)");
    if (p->lx.t.kind != TOK_IDENT)
        return fail(p, BRAD_CVT_ERR_PARSE, line, ".entry needs a name");
    if (strlen(p->lx.t.text) >= sizeof(p->m->name))
        return fail(p, BRAD_CVT_ERR_TRANSLATION, line,
                    "kernel name is too long");
    snprintf(p->m->name, sizeof(p->m->name), "%s", p->lx.t.text);
    p_next(p);

    if (!accept_punct(p, '('))
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "expected '(' after the kernel name");

    for (;;) {
        skip_nl(p);
        if (is_punct(p, ')'))
            break;
        if (!is_ident(p, ".param"))
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected .param or ')'");

        if (p->m->nparams >= CVT_MAX_PARAMS)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "more than %d kernel parameters", CVT_MAX_PARAMS);
        p_next(p);   /* .param */

        if (is_ident(p, ".align")) {
            p_next(p);
            if (p->lx.t.kind != TOK_NUMBER)
                return fail(p, BRAD_CVT_ERR_PARSE, line,
                            ".align needs a number");
            p_next(p);
        }
        if (is_ident(p, ".ptr"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "typed .ptr parameters are outside the v0 subset");

        if (p->lx.t.kind != TOK_IDENT || p->lx.t.text[0] != '.')
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected a parameter type");
        enum cvt_type ty = type_of(p->lx.t.text);
        if (ty == TY_NONE || ty == TY_PRED || ty == TY_F64)
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "parameter type '%s' is outside the v0 subset",
                        p->lx.t.text);
        p_next(p);

        if (p->lx.t.kind != TOK_IDENT)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "expected a parameter name");
        struct cvt_param *pm = &p->m->params[p->m->nparams++];
        if (set_name(p, pm->name, sizeof(pm->name), p->lx.t.text, line) < 0)
            return -1;
        pm->type = ty;
        p_next(p);

        if (is_punct(p, '['))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, line,
                        "parameter arrays are outside the v0 subset");
        if (accept_punct(p, ','))
            continue;
        if (is_punct(p, ')'))
            break;
    }

    if (!accept_punct(p, ')'))
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "expected ')' after the parameter list");

    skip_nl(p);
    while (p->lx.t.kind == TOK_IDENT && p->lx.t.text[0] == '.' &&
           (is_ident(p, ".maxntid") || is_ident(p, ".reqntid") ||
            is_ident(p, ".minnctapersm") || is_ident(p, ".maxnreg") ||
            is_ident(p, ".pragma"))) {
        skip_attr(p);
        skip_nl(p);
    }

    if (!accept_punct(p, '{'))
        return fail(p, BRAD_CVT_ERR_PARSE, line,
                    "expected '{' to open the kernel body");

    p->seen_entry = 1;

    for (;;) {
        skip_seps(p);
        if (is_punct(p, '}')) {
            p_next(p);
            break;
        }
        if (p->lx.t.kind == TOK_EOF)
            return fail(p, BRAD_CVT_ERR_PARSE, line,
                        "missing '}' at end of kernel");
        if (parse_body_item(p) < 0)
            return -1;
    }

    skip_seps(p);
    return 0;
}

static int parse_top(struct parser *p)
{
    for (;;) {
        skip_seps(p);
        if (p->lx.t.kind == TOK_EOF)
            break;

        if (p->lx.t.kind != TOK_IDENT)
            return fail(p, BRAD_CVT_ERR_PARSE, p->lx.t.line,
                        "expected a directive or .entry");

        const char *d = p->lx.t.text;
        if (d[0] != '.')
            return fail(p, BRAD_CVT_ERR_PARSE, p->lx.t.line,
                        "instruction outside .entry");

        if (!strcmp(d, ".version") || !strcmp(d, ".target") ||
            !strcmp(d, ".address_size") || !strcmp(d, ".file") ||
            !strcmp(d, ".loc") || !strcmp(d, ".pragma")) {
            skip_line(p);
            continue;
        }
        if (!strcmp(d, ".visible")) {
            p_next(p);
            if (!is_ident(p, ".entry"))
                return fail(p, BRAD_CVT_ERR_UNSUPPORTED, p->lx.t.line,
                            "only .visible .entry is supported");
            if (parse_entry(p) < 0)
                return -1;
            continue;
        }
        if (!strcmp(d, ".entry")) {
            if (parse_entry(p) < 0)
                return -1;
            continue;
        }
        if (!strcmp(d, ".func"))
            return fail(p, BRAD_CVT_ERR_UNSUPPORTED, p->lx.t.line,
                        ".func device functions are outside the v0 subset");
        if (!strcmp(d, ".reg") || !strcmp(d, ".shared"))
            return fail(p, BRAD_CVT_ERR_PARSE, p->lx.t.line,
                        "'%s' outside .entry", d);

        return fail(p, BRAD_CVT_ERR_UNSUPPORTED, p->lx.t.line,
                    "directive '%s' is outside the v0 subset", d);
    }
    return 0;
}

int cvt_parse(const char *src, size_t len, struct cvt_module *m,
              struct cvt_diag *diag)
{
    memset(m, 0, sizeof(*m));
    memset(diag, 0, sizeof(*diag));

    if (!src) {
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "null PTX source");
        return BRAD_CVT_ERR_PARSE;
    }

    m->insns = calloc(CVT_MAX_IR, sizeof(*m->insns));
    if (!m->insns) {
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "out of memory");
        return BRAD_CVT_ERR_MEMORY;
    }

    /* The public API takes a length, not a guarantee of NUL termination. */
    char *copy = malloc(len + 1);
    if (!copy) {
        free(m->insns);
        m->insns = NULL;
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "out of memory");
        return BRAD_CVT_ERR_MEMORY;
    }
    if (len)
        memcpy(copy, src, len);
    copy[len] = '\0';

    struct parser p;
    memset(&p, 0, sizeof(p));
    p.m = m;
    p.diag = diag;
    p.status = BRAD_CVT_ERR_PARSE;
    cvt_lex_init(&p.lx, copy);
    cvt_lex_next(&p.lx);

    int status = 0;
    if (parse_top(&p) < 0) {
        status = p.status;
    } else if (!p.seen_entry) {
        diag->line = 0;
        snprintf(diag->msg, sizeof(diag->msg), "no .entry kernel found");
        status = BRAD_CVT_ERR_PARSE;
    }

    free(copy);
    if (status != 0) {
        free(m->insns);
        m->insns = NULL;
    }
    return status;
}
