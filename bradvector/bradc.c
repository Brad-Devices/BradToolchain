#include "bradc.h"
#include "brad/bradvector.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ─── Tokenizer ─── */

enum tok_kind {
    T_EOF, T_ID, T_NUM, T_COLON, T_COMMA, T_NL, T_AT, T_DOT,
};

struct tok {
    enum tok_kind kind;
    char  text[64];
    int   ival;
};

struct lexer {
    const char *src;
    const char *p;
    int line;
    struct tok t;
};

static void lex_take(struct lexer *lx)
{
    while (*lx->p == ' ' || *lx->p == '\t')
        lx->p++;

    char c = *lx->p;
    memset(&lx->t, 0, sizeof(lx->t));

    if (c == '\0') { lx->t.kind = T_EOF; return; }
    if (c == '\n') { lx->p++; lx->line++; lx->t.kind = T_NL; return; }
    if (c == ';' || c == '#') {
        while (*lx->p && *lx->p != '\n') lx->p++;
        lx->t.kind = T_NL;
        return;
    }
    if (c == ',') { lx->p++; lx->t.kind = T_COMMA; return; }
    if (c == ':') { lx->p++; lx->t.kind = T_COLON; return; }
    if (c == '@') { lx->p++; lx->t.kind = T_AT; return; }
    if (c == '.') { lx->p++; lx->t.kind = T_DOT; return; }

    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)lx->p[1]))) {
        char *end;
        long v = strtol(lx->p, &end, 0);
        lx->p = end;
        lx->t.kind = T_NUM;
        lx->t.ival = (int)v;
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t i = 0;
        while (i < sizeof(lx->t.text) - 1 &&
               (isalnum((unsigned char)*lx->p) || *lx->p == '_' ||
                *lx->p == '+' || *lx->p == '-')) {
            lx->t.text[i++] = *lx->p++;
        }
        lx->t.text[i] = '\0';
        lx->t.kind = T_ID;
        return;
    }

    lx->t.kind = T_ID;
    lx->t.text[0] = c;
    lx->t.text[1] = '\0';
    lx->p++;
}

static void lx_init(struct lexer *lx, const char *src)
{
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->p = src;
    lx->line = 1;
}

/* Advance past the remainder of the current line (to just after NL). */
static void skip_line(struct lexer *lx)
{
    while (lx->t.kind != T_NL && lx->t.kind != T_EOF)
        lex_take(lx);
    if (lx->t.kind == T_NL)
        lex_take(lx);
}

/* ─── Symbol table ─── */

struct bradc_asm {
    struct bvbc_image *img;
    struct bradc_symbol syms[BRADC_MAX_SYMBOLS];
    int nsyms;
    int in_kernel;       /* 1 while inside .kernel/.end */
    uint32_t kpc;        /* instruction count inside current kernel */
};

static int b_find(const struct bradc_asm *a, const char *name)
{
    for (int i = 0; i < a->nsyms; i++)
        if (strcmp(a->syms[i].name, name) == 0)
            return i;
    return -1;
}

static int b_add(struct bradc_asm *a, const char *name, uint32_t addr)
{
    if (a->nsyms >= BRADC_MAX_SYMBOLS)
        return -1;
    strncpy(a->syms[a->nsyms].name, name, sizeof(a->syms[0].name) - 1);
    a->syms[a->nsyms].name[sizeof(a->syms[0].name) - 1] = '\0';
    a->syms[a->nsyms].addr = addr;
    return a->nsyms++;
}

static void b_err(struct bradc_error *err, int line, const char *fmt, ...)
{
    if (!err)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
    err->line = line;
}

/* ─── Operands ─── */

enum ocls { OC_NONE = 0, OC_S = 1, OC_V = 2, OC_P = 3 };

struct operand {
    int is_reg;
    int cls;
    int num;
};

static int parse_reg(struct lexer *lx, int cls, struct operand *o, int *line)
{
    if (lx->t.kind != T_ID)
        return -1;
    char r = lx->t.text[0];
    int want = (r == 'S' || r == 's') ? OC_S : (r == 'V' || r == 'v') ? OC_V
              : (r == 'P' || r == 'p') ? OC_P : OC_NONE;
    if (want != cls)
        return -1;
    char *end;
    long v = strtol(lx->t.text + 1, &end, 10);
    int max = (cls == OC_S) ? BV_SCALAR_REGS : (cls == OC_V) ? BV_VEC_REGS
              : BV_PRED_REGS;
    if (*end != '\0' || v < 0 || v >= max)
        return -1;
    o->is_reg = 1;
    o->cls = cls;
    o->num = (int)v;
    *line = lx->line;
    return 0;
}

static int parse_operand(struct lexer *lx, struct operand *o, int *line)
{
    if (lx->t.kind == T_NUM) {
        o->is_reg = 0;
        o->cls = OC_NONE;
        o->num = lx->t.ival;
        *line = lx->line;
        return 0;
    }
    if (lx->t.kind == T_ID) {
        struct lexer saved = *lx;
        if (parse_reg(lx, OC_S, o, line) == 0) return 0;
        *lx = saved;
        if (parse_reg(lx, OC_V, o, line) == 0) return 0;
        *lx = saved;
        if (parse_reg(lx, OC_P, o, line) == 0) return 0;
    }
    return -1;
}

static void next_src(struct lexer *lx)
{
    lex_take(lx);
    if (lx->t.kind == T_COMMA) lex_take(lx);
}

/* ─── Instruction assembly ─── */

static int b_emit(struct bradc_asm *a, uint64_t insn,
                  struct bradc_error *err, int line)
{
    if (!a->in_kernel) {
        b_err(err, line, "instruction outside .kernel/.end");
        return -1;
    }
    if (bvbc_emit(a->img, insn) < 0) {
        b_err(err, line, "instruction stream full (%d)", BVBC_MAX_INSN);
        return -1;
    }
    a->kpc++;
    return 0;
}

static int b_branch(struct bradc_asm *a, struct lexer *lx, uint8_t op,
                    uint8_t pred, struct bradc_error *err)
{
    int8_t off = 0;
    if (lx->t.kind == T_NUM) {
        off = (int8_t)lx->t.ival;
    } else if (lx->t.kind == T_ID) {
        int si = b_find(a, lx->t.text);
        if (si < 0) {
            b_err(err, lx->line, "undefined label '%s'", lx->t.text);
            return -1;
        }
        long d = (long)a->syms[si].addr - ((long)a->kpc + 1);
        if (d < -128 || d > 127) {
            b_err(err, lx->line, "branch out of range (label %s)", lx->t.text);
            return -1;
        }
        off = (int8_t)d;
    } else {
        b_err(err, lx->line, "expected label or offset");
        return -1;
    }

    return b_emit(a, BVEN(op, pred, 0, 0, 0, off, 0, 0), err, lx->line);
}

/* ─── Line classification ───
   Returns:
     LT_LABEL — token is `name:`
     LT_DIR   — `.kernel name` or `.end`
     LT_INSN  — anything else that looks like an instruction line
   Never advances the lexer past the first token. */

enum line_type { LT_LABEL, LT_DIR, LT_INSN };

enum line_type peek_line(struct lexer *lx)
{
    if (lx->t.kind == T_ID) {
        struct lexer saved = *lx;
        lex_take(lx);
        int is_label = (lx->t.kind == T_COLON);
        lex_take(lx);
        int is_kw = (lx->t.kind == T_ID &&
                     (strcmp(lx->t.text, "kernel") == 0 ||
                      strcmp(lx->t.text, "end") == 0));
        *lx = saved;
        if (is_label) return LT_LABEL;
        if (lx->t.kind == T_DOT && is_kw) return LT_DIR;
        return LT_INSN;
    }
    if (lx->t.kind == T_DOT) {
        struct lexer saved = *lx;
        lex_take(lx);
        int is_kw = (lx->t.kind == T_ID &&
                     (strcmp(lx->t.text, "kernel") == 0 ||
                      strcmp(lx->t.text, "end") == 0));
        *lx = saved;
        return is_kw ? LT_DIR : LT_INSN;
    }
    return LT_INSN;
}

/* ─── Opcode table ─── */

struct opentry {
    const char *name;
    uint8_t op;
};

static const struct opentry optab[] = {
    {"ADD", BV_OP_ADD}, {"ADDi", BV_OP_ADDi}, {"MOV", BV_OP_ADDi},
    {"SUB", BV_OP_SUB}, {"MUL", BV_OP_MUL}, {"MAD", BV_OP_MAD},
    {"DIV", BV_OP_DIV},
    {"SQRT", BV_OP_SQRT}, {"RSQRT", BV_OP_RSQRT}, {"RCP", BV_OP_RCP},
    {"SIN", BV_OP_SIN}, {"COS", BV_OP_COS},
    {"EXP2", BV_OP_EXP2}, {"LOG2", BV_OP_LOG2},
    {"MIN", BV_OP_MIN}, {"MAX", BV_OP_MAX}, {"ABS", BV_OP_ABS},
    {"NEG", BV_OP_NEG}, {"SEL", BV_OP_SEL}, {"CMP", BV_OP_CMP},
    {"IADD", BV_OP_IADD}, {"ISUB", BV_OP_ISUB}, {"IMUL", BV_OP_IMUL},
    {"IDIV", BV_OP_IDIV}, {"IMAD", BV_OP_IMAD}, {"POPC", BV_OP_POPC},
    {"CLZ", BV_OP_CLZ}, {"SHL", BV_OP_SHL}, {"SHR", BV_OP_SHR},
    {"AND", BV_OP_AND}, {"OR", BV_OP_OR}, {"XOR", BV_OP_XOR},
    {"NOT", BV_OP_NOT},
    {"VADD", BV_OP_VADD}, {"VMUL", BV_OP_VMUL}, {"VDOT", BV_OP_VDOT},
    {"VMAD", BV_OP_VMAD}, {"VBROADCAST", BV_OP_VBROADCAST},
    {"VREDUCE", BV_OP_VREDUCE},
    {"LOAD", BV_OP_LOAD}, {"LOADV", BV_OP_LOADV},
    {"STORE", BV_OP_STORE}, {"STOREV", BV_OP_STOREV},
    {"BR", BV_OP_BR}, {"BR_COND", BV_OP_BR_COND}, {"CALL", BV_OP_CALL},
    {"RET", BV_OP_RET}, {"BAR", BV_OP_BAR}, {"EXIT", BV_OP_EXIT},
    {"TRAP", BV_OP_TRAP},
    {"LANE_ID", BV_OP_LANE_ID}, {"WARP_SZ", BV_OP_WARP_SZ},
    {"CLOCK", BV_OP_CLOCK},
};

static uint8_t op_of(const char *name)
{
    for (size_t i = 0; i < sizeof(optab) / sizeof(optab[0]); i++)
        if (strcmp(name, optab[i].name) == 0)
            return optab[i].op;
    return 0xFF;
}

/* Parse `OP@P#` predication.  On entry lx->t is the mnemonic (first
   token of the line).  On return lx->t is the first operand token and
   *pred holds the predicate register (0 = unpredicated). */

static int parse_pred(struct lexer *lx, uint8_t *pred,
                      struct bradc_error *err)
{
    *pred = 0;
    lex_take(lx);              /* consume mnemonic; lx->t = next token */
    if (lx->t.kind != T_AT)
        return 0;              /* no predication; lexer is at first operand */

    lex_take(lx);
    if (lx->t.kind != T_ID || lx->t.text[0] != 'P') {
        b_err(err, lx->line, "expected predicate after @");
        return -1;
    }
    char *end;
    long p = strtol(lx->t.text + 1, &end, 10);
    if (*end != '\0' || p < 0 || p >= BV_PRED_REGS) {
        b_err(err, lx->line, "bad predicate 'P%ld'", p);
        return -1;
    }
    *pred = (uint8_t)p + 1;      /* 1..15 stored 0 = none in ISA PRED bit */
    next_src(lx);
    return 0;
}

int bradc_assemble(const char *source, struct bvbc_image *img,
                   struct bradc_error *err)
{
    if (!source || !img)
        return -1;
    if (err) { err->line = 0; err->msg[0] = 0; }

    struct bradc_asm a;
    memset(&a, 0, sizeof(a));
    a.img = img;

    /* ── Pass 1: labels ── */
    {
        struct lexer lx;
        lx_init(&lx, source);
        lex_take(&lx);
        for (;;) {
            if (lx.t.kind == T_EOF) break;
            if (lx.t.kind == T_NL) { lex_take(&lx); continue; }

            switch (peek_line(&lx)) {
            case LT_DIR: {
                struct lexer saved = lx;
                lex_take(&lx);           /* consume '.' */
                int is_end = strcmp(lx.t.text, "end") == 0;
                skip_line(&lx);
                a.in_kernel = is_end ? 0 : 1;
                a.kpc = 0;
                (void)saved;
                break;
            }
            case LT_LABEL: {
                struct tok name = lx.t;
                struct lexer saved = lx;
                lex_take(&lx);           /* consume name */
                lex_take(&lx);           /* consume ':' */
                (void)saved;
                if (a.in_kernel && b_find(&a, name.text) < 0)
                    b_add(&a, name.text, a.kpc);
                skip_line(&lx);
                break;
            }
            default:
                if (a.in_kernel)
                    a.kpc++;
                skip_line(&lx);
                break;
            }
        }
    }

    /* ── Pass 2: emit ── */
    a.in_kernel = 0;
    a.kpc = 0;
    img->nkernels = 0;
    img->ninsn = 0;
    img->pool_len = 0;

    struct lexer lx;
    lx_init(&lx, source);
    lex_take(&lx);

    for (;;) {
        if (lx.t.kind == T_EOF) break;
        if (lx.t.kind == T_NL) { lex_take(&lx); continue; }

        switch (peek_line(&lx)) {
        case LT_DIR: {
            lex_take(&lx);               /* consume '.' */
            if (strcmp(lx.t.text, "end") == 0) {
                if (!a.in_kernel) {
                    b_err(err, lx.line, ".end without .kernel");
                    return -1;
                }
                a.in_kernel = 0;
            } else {
                if (a.in_kernel) {
                    b_err(err, lx.line, "nested .kernel");
                    return -1;
                }
                next_src(&lx);           /* consume directive name */
                if (lx.t.kind != T_ID) {
                    b_err(err, lx.line, ".kernel expects a name");
                    return -1;
                }
                char dir_name[64];
                strncpy(dir_name, lx.t.text, sizeof(dir_name) - 1);
                dir_name[sizeof(dir_name) - 1] = 0;
                if (bvbc_add_kernel(img, dir_name, img->ninsn, 0) < 0) {
                    b_err(err, lx.line, "kernel table full");
                    return -1;
                }
                a.in_kernel = 1;
            }
            a.kpc = 0;
            skip_line(&lx);
            break;
        }
        case LT_LABEL:
            skip_line(&lx);
            break;
        case LT_INSN: {
            char mn[64];
            strncpy(mn, lx.t.text, sizeof(mn) - 1);
            mn[sizeof(mn) - 1] = 0;

            uint8_t pred = 0;
            if (parse_pred(&lx, &pred, err))
                return -1;

            uint8_t op = op_of(mn);

            /* zero-operand control flow */
            if (op == BV_OP_RET || op == BV_OP_EXIT || op == BV_OP_BAR ||
                op == BV_OP_TRAP) {
                if (b_emit(&a, BVEN(op, pred, 0, 0, 0, 0, 0, 0), err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* branches */
            if (op == BV_OP_BR || op == BV_OP_BR_COND || op == BV_OP_CALL) {
                if (b_branch(&a, &lx, op, pred, err))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* special one-operand */
            if (op == BV_OP_LANE_ID || op == BV_OP_WARP_SZ ||
                op == BV_OP_CLOCK) {
                struct operand d;
                int dline = lx.line;
                if (parse_operand(&lx, &d, &dline) < 0 || d.cls != OC_S) {
                    b_err(err, lx.line, "%s expects S#", mn);
                    return -1;
                }
                if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, 0, 0, 0, 0, 0),
                           err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            if (op == 0xFF) {
                b_err(err, lx.line, "unknown opcode '%s'", mn);
                return -1;
            }

            int isvec = (op >= BV_OP_VADD && op <= BV_OP_VREDUCE);
            int ismem = (op >= BV_OP_LOAD && op <= BV_OP_STOREV);
            int is1op = (op == BV_OP_ABS || op == BV_OP_NEG ||
                         op == BV_OP_NOT || op == BV_OP_POPC ||
                         op == BV_OP_CLZ || op == BV_OP_SQRT ||
                         op == BV_OP_RSQRT || op == BV_OP_RCP ||
                         op == BV_OP_SIN || op == BV_OP_COS ||
                         op == BV_OP_EXP2 || op == BV_OP_LOG2);

            /* destinations: LOAD/ATOMIC dst in d; LOADV dst is V; STORE
               first operand is the S base (value comes second); CMP dst is P */
            int dcls = (op == BV_OP_CMP) ? OC_P :
                       (op == BV_OP_LOADV) ? OC_V : OC_S;

            struct operand d, a1, a2;
            memset(&d, 0, sizeof(d));
            memset(&a1, 0, sizeof(a1));
            memset(&a2, 0, sizeof(a2));
            int dline = lx.line;
            if (parse_operand(&lx, &d, &dline) < 0) {
                b_err(err, lx.line, "bad destination for '%s'", mn);
                return -1;
            }
            if (d.cls != dcls) {
                b_err(err, lx.line, "bad destination class for '%s'", mn);
                return -1;
            }

            next_src(&lx);
            if (parse_operand(&lx, &a1, &dline) < 0) {
                b_err(err, lx.line, "bad source for '%s'", mn);
                return -1;
            }

            /* CMP P#, S#, S#|imm */
            if (op == BV_OP_CMP) {
                if (d.cls != OC_P) {
                    b_err(err, lx.line, "CMP destination must be P#");
                    return -1;
                }
                next_src(&lx);
                struct operand b2;
                int b2line = lx.line;
                memset(&b2, 0, sizeof(b2));
                if (lx.t.kind != T_NUM &&
                    parse_operand(&lx, &b2, &b2line) < 0) {
                    b_err(err, lx.line, "CMP source must be S#");
                    return -1;
                }
                if (b2.is_reg && b2.cls != OC_S) {
                    b_err(err, lx.line, "CMP source must be S#");
                    return -1;
                }
                if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, (uint8_t)a1.num,
                                    (uint8_t)b2.num, 0, 0, 0), err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* memory:
               LOAD  Vd,      Sbase, off   -> dst=d, src1=base
               LOADV Vd,      Sbase, off
               STORE Sbase,   Sval,  off   -> dst=val, src1=base
               STOREV Sbase,  Vval,  off
               ATOMIC_ADD Vd, Sbase, off   -> dst=d, src1=base
               dst field = value (store) or destination (load);
               src1 field = base address. */
            if (ismem) {
                int is_store = (op == BV_OP_STORE || op == BV_OP_STOREV);
                /* base is stored in the SRC1 field; for stores that is the
                   first operand, for loads the second. */
                int store_val_v = (op == BV_OP_STOREV);
                int want_base = OC_S;
                int want_val  = store_val_v ? OC_V : OC_S;
                if ((is_store ? d.cls : a1.cls) != want_base) {
                    b_err(err, lx.line, "memory base must be S#");
                    return -1;
                }
                if ((is_store ? a1.cls : d.cls) != want_val) {
                    b_err(err, lx.line, "bad memory operand class");
                    return -1;
                }
                next_src(&lx);
                if (lx.t.kind != T_NUM) {
                    b_err(err, lx.line, "memory access needs offset");
                    return -1;
                }
                uint8_t dst = is_store ? (uint8_t)a1.num : (uint8_t)d.num;
                uint8_t base = is_store ? (uint8_t)d.num : (uint8_t)a1.num;
                (void)want_val;
                if (b_emit(&a, BVEN(op, pred, dst, base, 0,
                                    (int8_t)lx.t.ival, 0, 0),
                           err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* MOV Sd, Ssrc  ->  OR Sd, Ssrc, S0
             *
             * MOV must be a bit copy.  ADDi with a zero immediate is a
             * float add: it canonicalises -0.0 to +0.0 and may quiet a
             * NaN.  OR with the hardwired zero register is exact for
             * every 32-bit pattern. */
            if (strcmp(mn, "MOV") == 0) {
                next_src(&lx);
                if (b_emit(&a, BVEN(BV_OP_OR, pred, (uint8_t)d.num,
                                    (uint8_t)a1.num, BV_S_ZERO, 0, 0, 0),
                           err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* immediate form: ADDi S#, S#, imm */
            if (op == BV_OP_ADDi) {
                next_src(&lx);
                if (lx.t.kind != T_NUM) {
                    b_err(err, lx.line, "%s expects immediate", mn);
                    return -1;
                }
                if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, (uint8_t)a1.num,
                                    0, (int8_t)lx.t.ival, 0, 0),
                           err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* single-source ops */
            if (is1op) {
                if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, (uint8_t)a1.num,
                                    0, 0, 0, 0), err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* SEL Sd, Sa, Sb, P#  ->  FLAGS names the select predicate.
             * The condition is a separate operand from the execution
             * predicate: dst = P# ? Sa : Sb.  Without it SEL has no way
             * to name its condition, which is why the old form could
             * never select src1. */
            if (op == BV_OP_SEL) {
                if (a1.cls != OC_S) {
                    b_err(err, lx.line, "SEL first source must be S#");
                    return -1;
                }
                next_src(&lx);
                if (parse_operand(&lx, &a2, &dline) < 0 || a2.cls != OC_S) {
                    b_err(err, lx.line, "SEL second source must be S#");
                    return -1;
                }
                next_src(&lx);
                struct operand p3;
                int p3line = lx.line;
                memset(&p3, 0, sizeof(p3));
                if (parse_operand(&lx, &p3, &p3line) < 0 || p3.cls != OC_P) {
                    b_err(err, lx.line, "SEL needs a condition predicate P#");
                    return -1;
                }
                if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, (uint8_t)a1.num,
                                    (uint8_t)a2.num, 0,
                                    (uint8_t)(p3.num + 1), 0),
                           err, lx.line))
                    return -1;
                skip_line(&lx);
                break;
            }

            /* vector second source (register forms only; immediates are
               exclusive to ADDi) */
            next_src(&lx);
            if (lx.t.kind == T_NUM) {
                b_err(err, lx.line,
                      "'%s' does not take an immediate; use ADDi", mn);
                return -1;
            }
            if (parse_operand(&lx, &a2, &dline) < 0 ||
                a2.cls != (isvec && op != BV_OP_VBROADCAST ? OC_V : OC_S)) {
                b_err(err, lx.line, "bad source class for '%s'", mn);
                return -1;
            }
            if (b_emit(&a, BVEN(op, pred, (uint8_t)d.num, (uint8_t)a1.num,
                                (uint8_t)a2.num, 0, 0, 0), err, lx.line))
                return -1;
            skip_line(&lx);
            break;
        }
        }
    }

    if (a.in_kernel) {
        b_err(err, 0, "missing .end");
        return -1;
    }
    img->version = BVBC_VERSION;
    return 0;
}

int bradc_symbols(const struct bvbc_image *img,
                  struct bradc_symbol *sym, size_t cap, size_t *n)
{
    (void)img;
    if (n) *n = 0;
    if (!sym || cap == 0)
        return 0;
    /* The in-memory image does not carry the symbol table after
       assembly; reserved for bradgdb. */
    return 0;
}