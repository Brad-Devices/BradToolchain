#ifndef BRAD_CVT_PTX_H
#define BRAD_CVT_PTX_H

#include <stdint.h>
#include <stddef.h>

#include "brad/bradcvt.h"

/* ─── Brad-CVT PTX front end (internal) ───
 *
 * One .entry kernel in, one serialized .bvbc image out.  The supported
 * subset is documented in Brad Software/OS/BRADCVT_PTX_SUBSET.md; anything
 * outside it fails with BRAD_CVT_ERR_UNSUPPORTED and a line number rather
 * than producing an image that quietly does something else.
 */

#define CVT_MAX_VREGS     512
#define CVT_MAX_PARAMS     16
#define CVT_MAX_LABELS    256
#define CVT_MAX_IR      16384
#define CVT_MAX_PATCH     512
#define CVT_NAME          64

struct cvt_diag {
    int  line;
    char msg[256];
};

/* ─── Lexer ─── */

enum cvt_tok_kind {
    TOK_EOF = 0,
    TOK_NL,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_PUNCT,
    TOK_STRING,
};

struct cvt_token {
    enum cvt_tok_kind kind;
    char              text[128];
    int               line;
};

struct cvt_lexer {
    const char       *p;
    int               line;
    struct cvt_token  t;
};

void cvt_lex_init(struct cvt_lexer *lx, const char *src);
void cvt_lex_next(struct cvt_lexer *lx);

enum cvt_type {
    TY_NONE = 0,
    TY_B32, TY_U32, TY_S32, TY_F32, TY_PRED,
    TY_B64, TY_U64, TY_S64, TY_F64,
};

static inline int cvt_is_float(enum cvt_type t)
{
    return t == TY_F32 || t == TY_F64;
}

static inline int cvt_is_64(enum cvt_type t)
{
    return t == TY_B64 || t == TY_U64 || t == TY_S64 || t == TY_F64;
}

struct cvt_vreg {
    char          name[CVT_NAME];
    enum cvt_type type;
    int           sreg;   /* -1 until allocated; scalar registers */
    int           sreg_hi;/* 64-bit high word; -1 until allocated */
    int           hi_zero;/* high word is known to be zero (zero-extended) */
    int           preg;   /* -1 until allocated; predicate registers */
};

struct cvt_param {
    char          name[CVT_NAME];
    enum cvt_type type;
};

enum cvt_operand_kind {
    OPR_NONE = 0,
    OPR_REG,
    OPR_IMM,
    OPR_PARAM,
    OPR_SPECIAL,
    OPR_ADDR,
    OPR_LABEL,
};

struct cvt_operand {
    enum cvt_operand_kind kind;
    struct cvt_vreg       *reg;
    uint64_t               imm;
    unsigned               imm_float : 1;
    char                   name[CVT_NAME];
    struct cvt_vreg       *base;
    int32_t                off;
};

enum cvt_op {
    OP_LABEL = 0,
    OP_MOV,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_MAD, OP_MIN, OP_MAX, OP_ABS, OP_NEG,
    OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_REM,
    OP_IADD, OP_ISUB, OP_IMUL, OP_IDIV,
    OP_SETP, OP_SELP,
    OP_LOAD, OP_STORE,
    OP_BRA, OP_RET, OP_EXIT, OP_BAR, OP_TRAP,
    OP_SQRT, OP_RSQRT, OP_RCP,
    OP_EXP2, OP_LOG2, OP_SIN, OP_COS,
};

struct cvt_insn {
    enum cvt_op        op;
    enum cvt_type      type;
    int                line;
    struct cvt_operand dst, a, b, c;
    int                cc;        /* BV_CC_* for OP_SETP */
    int                pred;      /* vreg predicate index, -1 none */
    int                pred_neg;
    int                width;     /* load/store width in bytes */
};

struct cvt_module {
    char               name[CVT_NAME];
    struct cvt_param   params[CVT_MAX_PARAMS];
    int                nparams;
    unsigned           shared_bytes;

    struct cvt_vreg    vregs[CVT_MAX_VREGS];
    int                nvregs;

    struct cvt_insn   *insns;
    int                ninsns;
};

/* Parse a PTX module into the IR.  0 on success. */
int cvt_parse(const char *src, size_t len, struct cvt_module *m,
              struct cvt_diag *diag);

/* Lower the IR to a serialized .bvbc image (malloc'd).  0 on success. */
int cvt_codegen(struct cvt_module *m, void **out, size_t *out_len,
                struct brad_cvt_kernel_info *info, struct cvt_diag *diag);

/* Parse + lower.  Returns a BRAD_CVT_* status. */
int cvt_ptx_translate(const char *src, size_t len,
                      void **out, size_t *out_len,
                      struct brad_cvt_kernel_info *info,
                      struct cvt_diag *diag);

#endif /* BRAD_CVT_PTX_H */
