/*
 * BradISA V2 assembler.
 *
 * Two passes over the source: the first measures (every V2 instruction is
 * four bytes, so this is mostly labels and data directives), the second
 * encodes and emits.
 *
 * The encoding is the one in bradisa.h and bradisa_defines.v.  This file
 * does not carry a second copy of the opcode numbers: every table row names
 * a BRAD_V2_OP_* constant, and tools/check_isa_drift.py fails the build if
 * the C table and the Verilog package disagree.
 *
 * Compressed 16-bit mode was a V1 feature.  BradISA V2 is fixed at 32 bits,
 * so this assembler retires it explicitly: .compress and C.* produce a
 * diagnostic saying so rather than silently emitting something else.
 */

#include "brad/bradasm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Tokenizer ──────────────────────────────────────────────────────── */

enum tok_kind {
    T_EOF, T_ID, T_NUM, T_COLON, T_COMMA, T_LBRACK, T_RBRACK,
    T_PLUS, T_MINUS, T_NEWLINE,
    T_DIR_ORG, T_DIR_WORD, T_DIR_BYTE, T_DIR_HALF, T_DIR_ENTRY,
    T_DIR_COMPRESS, T_DIR_NOCOMPRESS, T_DIR_UNKNOWN
};

struct token {
    enum tok_kind kind;
    char          text[64];
    long          ival;
    int           err;
};

struct lexer {
    const char *p;
    int         line;
    struct token tok;
};

static void lex_init(struct lexer *lx, const char *src)
{
    lx->p = src;
    lx->line = 1;
    memset(&lx->tok, 0, sizeof lx->tok);
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_number(const char *s, long *out)
{
    char clean[64];
    size_t j = 0;
    for (size_t i = 0; s[i] && j < sizeof clean - 1; i++)
        if (s[i] != '_')
            clean[j++] = s[i];
    clean[j] = '\0';

    int base = 10;
    const char *p = clean;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    if (*p == '\0')
        return -1;

    char *end;
    long v = strtol(p, &end, base);
    if (end == p || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

static void lex_next(struct lexer *lx)
{
    while (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r')
        lx->p++;

    memset(&lx->tok, 0, sizeof lx->tok);
    char c = *lx->p;

    if (c == '\0') { lx->tok.kind = T_EOF; return; }
    if (c == '\n') { lx->p++; lx->line++; lx->tok.kind = T_NEWLINE; return; }
    if (c == ';' || c == '#') {
        while (*lx->p && *lx->p != '\n')
            lx->p++;
        lx->tok.kind = T_NEWLINE;
        return;
    }
    if (c == ':') { lx->p++; lx->tok.kind = T_COLON; return; }
    if (c == ',') { lx->p++; lx->tok.kind = T_COMMA; return; }
    if (c == '[') { lx->p++; lx->tok.kind = T_LBRACK; return; }
    if (c == ']') { lx->p++; lx->tok.kind = T_RBRACK; return; }
    if (c == '+') { lx->p++; lx->tok.kind = T_PLUS; return; }
    if (c == '-') { lx->p++; lx->tok.kind = T_MINUS; return; }
    if (c == '$') {
        lx->p++;
        lx->tok.kind = T_ID;
        strcpy(lx->tok.text, "$");
        return;
    }

    if (c == '.') {
        const char *start = lx->p + 1;
        const char *q = start;
        while (isalpha((unsigned char)*q) || *q == '_')
            q++;
        size_t n = (size_t)(q - start);
        if (n == 0 || n >= sizeof lx->tok.text) {
            lx->tok.kind = T_DIR_UNKNOWN;
            snprintf(lx->tok.text, sizeof lx->tok.text, "invalid directive");
            lx->p++;
            return;
        }
        lx->tok.text[0] = '.';
        memcpy(lx->tok.text + 1, start, n);
        lx->tok.text[n + 1] = '\0';
        lx->p = q;

        const char *word = lx->tok.text + 1;
        if (name_eq(word, "org"))            lx->tok.kind = T_DIR_ORG;
        else if (name_eq(word, "word"))      lx->tok.kind = T_DIR_WORD;
        else if (name_eq(word, "byte"))      lx->tok.kind = T_DIR_BYTE;
        else if (name_eq(word, "half"))      lx->tok.kind = T_DIR_HALF;
        else if (name_eq(word, "entry"))     lx->tok.kind = T_DIR_ENTRY;
        else if (name_eq(word, "compress"))  lx->tok.kind = T_DIR_COMPRESS;
        else if (name_eq(word, "nocompress"))lx->tok.kind = T_DIR_NOCOMPRESS;
        else                                 lx->tok.kind = T_DIR_UNKNOWN;
        return;
    }

    if (isdigit((unsigned char)c)) {
        const char *start = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '_')
            lx->p++;
        size_t n = (size_t)(lx->p - start);
        if (n >= sizeof lx->tok.text) {
            lx->tok.kind = T_NUM;
            lx->tok.err = 1;
            return;
        }
        memcpy(lx->tok.text, start, n);
        lx->tok.text[n] = '\0';
        lx->tok.kind = T_NUM;
        if (parse_number(lx->tok.text, &lx->tok.ival) < 0)
            lx->tok.err = 1;
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '_' || *lx->p == '.')
            lx->p++;
        size_t n = (size_t)(lx->p - start);
        if (n >= sizeof lx->tok.text)
            n = sizeof lx->tok.text - 1;
        memcpy(lx->tok.text, start, n);
        lx->tok.text[n] = '\0';
        lx->tok.kind = T_ID;
        return;
    }

    snprintf(lx->tok.text, sizeof lx->tok.text, "unexpected character '%c'", c);
    lx->tok.kind = T_DIR_UNKNOWN;
    lx->p++;
}

/* ─── Expressions ────────────────────────────────────────────────────── */

struct eval_ctx {
    const struct brad_as_symbol *symbols;
    unsigned                     nsymbols;
    uint32_t                     pc;
    int                          error;
};

static int eval_term(struct lexer *lx, struct eval_ctx *ec, long *out)
{
    int sign = 1;
    if (lx->tok.kind == T_MINUS) { sign = -1; lex_next(lx); }
    else if (lx->tok.kind == T_PLUS) { lex_next(lx); }

    long v = 0;
    if (lx->tok.kind == T_NUM) {
        if (lx->tok.err) { ec->error = 1; return -1; }
        v = lx->tok.ival;
        lex_next(lx);
    } else if (lx->tok.kind == T_ID) {
        if (strcmp(lx->tok.text, "$") == 0) {
            v = (long)ec->pc;
        } else {
            int found = 0;
            for (unsigned i = 0; i < ec->nsymbols; i++) {
                if (strcmp(ec->symbols[i].name, lx->tok.text) == 0) {
                    v = (long)ec->symbols[i].addr;
                    found = 1;
                    break;
                }
            }
            if (!found) { ec->error = 1; return -1; }
        }
        lex_next(lx);
    } else {
        ec->error = 1;
        return -1;
    }

    *out = sign * v;
    return 0;
}

static int eval_expr(struct lexer *lx, struct eval_ctx *ec, long *out)
{
    long acc;
    if (eval_term(lx, ec, &acc) < 0)
        return -1;
    while (lx->tok.kind == T_PLUS || lx->tok.kind == T_MINUS) {
        int sign = (lx->tok.kind == T_PLUS) ? 1 : -1;
        lex_next(lx);
        long rhs;
        if (eval_term(lx, ec, &rhs) < 0)
            return -1;
        acc += sign * rhs;
    }
    *out = acc;
    return 0;
}

/* ─── Registers ──────────────────────────────────────────────────────── */

static int parse_reg(struct lexer *lx, unsigned *reg)
{
    if (lx->tok.kind != T_ID)
        return -1;

    const char *name = lx->tok.text;

    if ((name[0] == 'r' || name[0] == 'R') && name[1]) {
        char *end;
        long n = strtol(name + 1, &end, 10);
        if (*end == '\0') {
            if (n >= 0 && n < BRAD_V2_NUM_REGS) {
                *reg = (unsigned)n;
                lex_next(lx);
                return 0;
            }
            return -1;
        }
        /* Not rN (for example "ra"): fall through to the ABI names. */
    }

    static const struct { const char *name; unsigned num; } abi[] = {
        {"zero", BRAD_V2_R0},
        {"a0", BRAD_V2_A0}, {"a1", BRAD_V2_A1}, {"a2", BRAD_V2_A2},
        {"a3", BRAD_V2_A3}, {"a4", BRAD_V2_A4}, {"a5", BRAD_V2_A5},
        {"a6", BRAD_V2_A6}, {"a7", BRAD_V2_A7},
        {"t0", BRAD_V2_T0}, {"t1", BRAD_V2_T1}, {"t2", BRAD_V2_T2},
        {"t3", BRAD_V2_T3}, {"t4", BRAD_V2_T4}, {"t5", BRAD_V2_T5},
        {"lr", BRAD_V2_LR}, {"ra", BRAD_V2_RA},
        {"s0", BRAD_V2_S0}, {"s1", BRAD_V2_S1}, {"s2", BRAD_V2_S2},
        {"s3", BRAD_V2_S3}, {"s4", BRAD_V2_S4}, {"s5", BRAD_V2_S5},
        {"s6", BRAD_V2_S6}, {"s7", BRAD_V2_S7}, {"s8", BRAD_V2_S8},
        {"s9", BRAD_V2_S9}, {"s10", BRAD_V2_S10},
        {"sp", BRAD_V2_SP}, {"gp", BRAD_V2_GP}, {"tp", BRAD_V2_TP},
    };

    for (unsigned i = 0; i < sizeof abi / sizeof abi[0]; i++) {
        if (name_eq(name, abi[i].name)) {
            *reg = abi[i].num;
            lex_next(lx);
            return 0;
        }
    }
    return -1;
}

/* ─── Instruction table ──────────────────────────────────────────────── */

enum asm_fmt {
    A_NONE,    /* NOP, RET, FENCE */
    A_RRR,     /* op rd, rs1, rs2 */
    A_RI,      /* op rd, rs1, imm16 */
    A_LOAD,    /* op rd, [rs1 + imm16] */
    A_SI,      /* op rs2, [rs1 + imm16] */
    A_BR,      /* op rs1, rs2, target */
    A_BR_Z,    /* op rs1, target */
    A_IJ,      /* JALR rd, rs1, imm16 */
    A_JMP,     /* JMP target */
    A_JAL,     /* JAL rd, target */
    A_CALL,    /* CALL target */
    A_U,       /* op rd, imm21 */
    A_AMO_LR,  /* LR.W rd, [rs1] */
    A_AMO_ST,  /* op rd, rs2, [rs1] */
};

struct insn_def {
    const char          *mnemonic;
    enum brad_isa_v2_opcode opcode;
    enum asm_fmt         fmt;
};

static const struct insn_def insn_table[] = {
    {"NOP",      BRAD_V2_OP_NOP,      A_NONE},
    {"ADD",      BRAD_V2_OP_ADD,      A_RRR},
    {"SUB",      BRAD_V2_OP_SUB,      A_RRR},
    {"MUL",      BRAD_V2_OP_MUL,      A_RRR},
    {"MULHU",    BRAD_V2_OP_MULHU,    A_RRR},
    {"DIV",      BRAD_V2_OP_DIV,      A_RRR},
    {"DIVU",     BRAD_V2_OP_DIVU,     A_RRR},
    {"REM",      BRAD_V2_OP_REM,      A_RRR},
    {"REMU",     BRAD_V2_OP_REMU,     A_RRR},
    {"AND",      BRAD_V2_OP_AND,      A_RRR},
    {"OR",       BRAD_V2_OP_OR,       A_RRR},
    {"XOR",      BRAD_V2_OP_XOR,      A_RRR},
    {"SHL",      BRAD_V2_OP_SHL,      A_RRR},
    {"SHR",      BRAD_V2_OP_SHR,      A_RRR},
    {"SAR",      BRAD_V2_OP_SAR,      A_RRR},

    {"ADDI",     BRAD_V2_OP_ADDI,     A_RI},
    {"ANDI",     BRAD_V2_OP_ANDI,     A_RI},
    {"ORI",      BRAD_V2_OP_ORI,      A_RI},
    {"XORI",     BRAD_V2_OP_XORI,     A_RI},
    {"SLLI",     BRAD_V2_OP_SLLI,     A_RI},
    {"SRLI",     BRAD_V2_OP_SRLI,     A_RI},
    {"SRAI",     BRAD_V2_OP_SRAI,     A_RI},
    {"SLTI",     BRAD_V2_OP_SLTI,     A_RI},
    {"SLTIU",    BRAD_V2_OP_SLTIU,    A_RI},

    {"LDW",      BRAD_V2_OP_LDW,      A_LOAD},
    {"LHU",      BRAD_V2_OP_LHU,      A_LOAD},
    {"LHBU",     BRAD_V2_OP_LHBU,     A_LOAD},
    {"STW",      BRAD_V2_OP_STW,      A_SI},
    {"SH",       BRAD_V2_OP_SH,       A_SI},
    {"SB",       BRAD_V2_OP_SB,       A_SI},

    {"LUI",      BRAD_V2_OP_LUI,      A_U},
    {"AUIPC",    BRAD_V2_OP_AUIPC,    A_U},

    {"JMP",      BRAD_V2_OP_JMP,      A_JMP},
    {"JAL",      BRAD_V2_OP_JAL,      A_JAL},
    {"CALL",     BRAD_V2_OP_CALL,     A_CALL},
    {"JALR",     BRAD_V2_OP_JALR,     A_IJ},
    {"RET",      BRAD_V2_OP_RET,      A_NONE},
    {"FENCE",    BRAD_V2_OP_FENCE,    A_NONE},

    {"BEQ",      BRAD_V2_OP_BEQ,      A_BR},
    {"BNE",      BRAD_V2_OP_BNE,      A_BR},
    {"BLT",      BRAD_V2_OP_BLT,      A_BR},
    {"BGE",      BRAD_V2_OP_BGE,      A_BR},
    {"BLTU",     BRAD_V2_OP_BLTU,     A_BR},
    {"BGEU",     BRAD_V2_OP_BGEU,     A_BR},
    {"BZ",       BRAD_V2_OP_BZ,       A_BR_Z},
    {"BNZ",      BRAD_V2_OP_BNZ,      A_BR_Z},

    {"LR.W",     BRAD_V2_OP_LR_W,     A_AMO_LR},
    {"SC.W",     BRAD_V2_OP_SC_W,     A_AMO_ST},
    {"AMOSWAP.W",BRAD_V2_OP_AMOSWAP_W,A_AMO_ST},
    {"AMOADD.W", BRAD_V2_OP_AMOADD_W, A_AMO_ST},
};

static const struct insn_def *lookup_insn(const char *name)
{
    for (unsigned i = 0; i < sizeof insn_table / sizeof insn_table[0]; i++)
        if (name_eq(name, insn_table[i].mnemonic))
            return &insn_table[i];
    return NULL;
}

/* ─── Diagnostics ────────────────────────────────────────────────────── */

static int fail(struct brad_as_output *out, int line, const char *fmt, ...)
{
    out->ok = 0;
    out->error.line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->error.msg, sizeof out->error.msg, fmt, ap);
    va_end(ap);
    return -1;
}

/* ─── Data emission ──────────────────────────────────────────────────── */

static int emit_bytes(struct brad_as_output *out, unsigned pc, long value, int nbytes)
{
    for (int i = 0; i < nbytes; i++) {
        unsigned addr = pc + (unsigned)i;
        unsigned idx = addr / 4;
        if (idx >= BRAD_ASM_MAX_OUTPUT)
            return -1;
        unsigned shift = (addr % 4) * 8;
        uint32_t byte = ((uint32_t)value >> (8 * i)) & 0xFFu;
        out->code[idx] = (out->code[idx] & ~(0xFFu << shift)) | (byte << shift);
    }
    return 0;
}

/* ─── Symbol helpers ─────────────────────────────────────────────────── */

static int find_symbol(const struct brad_as_output *out, const char *name)
{
    for (unsigned i = 0; i < out->nsymbols; i++)
        if (strcmp(out->symbols[i].name, name) == 0)
            return (int)i;
    return -1;
}

static int add_symbol(struct brad_as_output *out, const char *name, uint32_t addr)
{
    if (out->nsymbols >= BRAD_ASM_MAX_SYMBOLS)
        return -1;
    struct brad_as_symbol *s = &out->symbols[out->nsymbols++];
    strncpy(s->name, name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = '\0';
    s->addr = addr;
    return 0;
}

/* ─── Address operand ────────────────────────────────────────────────── */

static int parse_addr(struct lexer *lx, struct eval_ctx *ec, unsigned pc,
                      unsigned *rs1, long *imm)
{
    *imm = 0;

    if (lx->tok.kind == T_LBRACK) {
        lex_next(lx);
        if (parse_reg(lx, rs1) < 0)
            return -1;

        if (lx->tok.kind == T_PLUS || lx->tok.kind == T_COMMA)
            lex_next(lx);

        if (lx->tok.kind != T_RBRACK) {
            ec->pc = pc;
            ec->error = 0;
            if (eval_expr(lx, ec, imm) < 0)
                return -1;
        }
        if (lx->tok.kind != T_RBRACK)
            return -1;
        lex_next(lx);
        return 0;
    }

    if (parse_reg(lx, rs1) < 0)
        return -1;
    if (lx->tok.kind == T_COMMA)
        lex_next(lx);

    ec->pc = pc;
    ec->error = 0;
    if (eval_expr(lx, ec, imm) < 0)
        return -1;
    return 0;
}

/* ─── Instruction encoding ───────────────────────────────────────────── */

static int imm16_for(const struct insn_def *d, long v)
{
    switch (d->opcode) {
    case BRAD_V2_OP_ANDI:
    case BRAD_V2_OP_ORI:
    case BRAD_V2_OP_XORI:
    case BRAD_V2_OP_SLTIU:
        /* Zero-extended by the spec, so the whole 16-bit pattern is legal. */
        return (v >= -32768 && v <= 65535);
    case BRAD_V2_OP_SLLI:
    case BRAD_V2_OP_SRLI:
    case BRAD_V2_OP_SRAI:
        return (v >= 0 && v <= 31);
    default:
        return (v >= -32768 && v <= 32767);
    }
}

static int branch_field(struct brad_as_output *out, int line, const char *what,
                        long target, unsigned pc, long *field)
{
    long disp = target - (long)(pc + 4);
    if (disp % 4 != 0)
        return fail(out, line, "%s target 0x%lX is not 4-byte aligned",
                    what, (unsigned long)target);
    long f = disp / 4;
    if (f < -32768 || f > 32767)
        return fail(out, line, "%s target out of range (16-bit word offset)", what);
    *field = f;
    return 0;
}

static int jump_field(struct brad_as_output *out, int line, const char *what,
                      long target, unsigned pc, long *field)
{
    long disp = target - (long)(pc + 4);
    if (disp % 4 != 0)
        return fail(out, line, "%s target 0x%lX is not 4-byte aligned",
                    what, (unsigned long)target);
    long f = disp / 4;
    if (f < -(1L << 20) || f > (1L << 20) - 1)
        return fail(out, line, "%s target out of range (21-bit word offset)", what);
    *field = f;
    return 0;
}

static int process_insn(struct lexer *lx, const struct insn_def *d,
                        unsigned *pc, struct brad_as_output *out, int emit,
                        struct eval_ctx *ec)
{
    uint32_t word = 0;
    unsigned rd = 0, rs1 = 0, rs2 = 0;
    long imm = 0;

    switch (d->fmt) {
    case A_NONE:
        if (lx->tok.kind != T_NEWLINE && lx->tok.kind != T_EOF)
            return fail(out, lx->line, "%s takes no operands", d->mnemonic);
        if (d->opcode == BRAD_V2_OP_NOP)
            word = BRAD_V2_NOP_ENCODING;
        else if (d->opcode == BRAD_V2_OP_RET)
            word = BRAD_V2_RET_ENCODING;
        else
            word = BRAD_V2_U(d->opcode, 0, 0);   /* FENCE */
        break;

    case A_RRR:
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs1) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs2) < 0) goto syntax;
        word = BRAD_V2_RRR(d->opcode, rd, rs1, rs2);
        break;

    case A_RI:
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs1) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        if (eval_expr(lx, ec, &imm) < 0) goto undefined;
        if (!imm16_for(d, imm))
            return fail(out, lx->line, "immediate %ld out of range for %s",
                        imm, d->mnemonic);
        word = BRAD_V2_RI(d->opcode, rd, rs1, imm);
        break;

    case A_LOAD:
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_addr(lx, ec, *pc, &rs1, &imm) < 0) goto undefined;
        if (!imm16_for(d, imm))
            return fail(out, lx->line, "offset %ld out of range for %s",
                        imm, d->mnemonic);
        word = BRAD_V2_RI(d->opcode, rd, rs1, imm);
        break;

    case A_SI:
        if (parse_reg(lx, &rs2) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_addr(lx, ec, *pc, &rs1, &imm) < 0) goto undefined;
        if (!imm16_for(d, imm))
            return fail(out, lx->line, "offset %ld out of range for %s",
                        imm, d->mnemonic);
        word = BRAD_V2_SI(d->opcode, rs2, rs1, imm);
        break;

    case A_BR: {
        if (parse_reg(lx, &rs1) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs2) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        long target;
        if (eval_expr(lx, ec, &target) < 0) goto undefined;
        long field;
        if (branch_field(out, lx->line, d->mnemonic, target, *pc, &field) < 0)
            return -1;
        word = BRAD_V2_BR(d->opcode, rs1, rs2, field);
        break;
    }

    case A_BR_Z: {
        if (parse_reg(lx, &rs1) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        long target;
        if (eval_expr(lx, ec, &target) < 0) goto undefined;
        long field;
        if (branch_field(out, lx->line, d->mnemonic, target, *pc, &field) < 0)
            return -1;
        word = BRAD_V2_BR(d->opcode, rs1, 0, field);
        break;
    }

    case A_IJ: {
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs1) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        if (eval_expr(lx, ec, &imm) < 0) goto undefined;
        if (imm < -32768 || imm > 32767)
            return fail(out, lx->line, "JALR offset %ld out of range", imm);
        word = BRAD_V2_IJ(d->opcode, rd, rs1, imm);
        break;
    }

    case A_JMP: {
        ec->pc = *pc; ec->error = 0;
        long target;
        if (eval_expr(lx, ec, &target) < 0) goto undefined;
        long field;
        if (jump_field(out, lx->line, d->mnemonic, target, *pc, &field) < 0)
            return -1;
        word = BRAD_V2_JMP(d->opcode, 0, field);
        break;
    }

    case A_JAL: {
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        long target;
        if (eval_expr(lx, ec, &target) < 0) goto undefined;
        long field;
        if (jump_field(out, lx->line, d->mnemonic, target, *pc, &field) < 0)
            return -1;
        word = BRAD_V2_JMP(d->opcode, rd, field);
        break;
    }

    case A_CALL: {
        ec->pc = *pc; ec->error = 0;
        long target;
        if (eval_expr(lx, ec, &target) < 0) goto undefined;
        long field;
        if (jump_field(out, lx->line, d->mnemonic, target, *pc, &field) < 0)
            return -1;
        word = BRAD_V2_JMP(d->opcode, BRAD_V2_LR, field);
        break;
    }

    case A_U: {
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        ec->pc = *pc; ec->error = 0;
        if (eval_expr(lx, ec, &imm) < 0) goto undefined;
        if (imm < -(1L << 20) || imm > (1L << 20) - 1)
            return fail(out, lx->line, "21-bit immediate %ld out of range for %s",
                        imm, d->mnemonic);
        word = BRAD_V2_U(d->opcode, rd, imm);
        break;
    }

    case A_AMO_LR:
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_addr(lx, ec, *pc, &rs1, &imm) < 0) goto undefined;
        if (imm != 0)
            return fail(out, lx->line, "%s takes no offset (address is rs1)", d->mnemonic);
        word = BRAD_V2_RRR(d->opcode, rd, rs1, 0);
        break;

    case A_AMO_ST:
        if (parse_reg(lx, &rd) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_reg(lx, &rs2) < 0) goto syntax;
        if (lx->tok.kind == T_COMMA) lex_next(lx);
        if (parse_addr(lx, ec, *pc, &rs1, &imm) < 0) goto undefined;
        if (imm != 0)
            return fail(out, lx->line, "%s takes no offset (address is rs1)", d->mnemonic);
        word = BRAD_V2_RRR(d->opcode, rd, rs1, rs2);
        break;
    }

    if (emit) {
        if (*pc % 4 != 0)
            return fail(out, lx->line, "instruction at unaligned address 0x%X", *pc);
        unsigned idx = *pc / 4;
        if (idx >= BRAD_ASM_MAX_OUTPUT)
            return fail(out, lx->line, "output limit of %d words exceeded",
                        BRAD_ASM_MAX_OUTPUT);
        out->code[idx] = word;
    }
    *pc += BRAD_V2_INSN_SIZE;
    return 0;

syntax:
    return fail(out, lx->line, "syntax error in %s operands", d->mnemonic);

undefined:
    if (ec->error)
        return fail(out, lx->line, "undefined symbol in %s operand", d->mnemonic);
    return fail(out, lx->line, "bad operand for %s", d->mnemonic);
}

/* ─── Pass 1: labels and sizes ───────────────────────────────────────── */

static int pass_measure(const char *source, struct brad_as_output *out)
{
    struct lexer lx;
    lex_init(&lx, source);

    unsigned pc = 0;
    struct eval_ctx ec = { out->symbols, 0, 0, 0 };

    for (;;) {
        lex_next(&lx);
        if (lx.tok.kind == T_EOF)
            break;
        if (lx.tok.kind == T_NEWLINE)
            continue;

        if (lx.tok.kind == T_ID) {
            char name[64];
            strncpy(name, lx.tok.text, sizeof name - 1);
            name[sizeof name - 1] = '\0';
            lex_next(&lx);

            if (lx.tok.kind == T_COLON) {
                if (find_symbol(out, name) >= 0)
                    return fail(out, lx.line, "duplicate label '%s'", name);
                if (add_symbol(out, name, pc) < 0)
                    return fail(out, lx.line, "too many labels");
                ec.nsymbols = out->nsymbols;
                continue;
            }

            if (!lookup_insn(name)) {
                if (name[0] == 'C' && name[1] == '.')
                    return fail(out, lx.line,
                                "compressed instruction %s is V1-only; "
                                "BradISA V2 is fixed 32-bit", name);
                return fail(out, lx.line, "unknown instruction '%s'", name);
            }
            if (pc % 4 != 0)
                return fail(out, lx.line, "instruction at unaligned address 0x%X", pc);
            pc += BRAD_V2_INSN_SIZE;

            while (lx.tok.kind != T_NEWLINE && lx.tok.kind != T_EOF)
                lex_next(&lx);
            continue;
        }

        if (lx.tok.kind == T_DIR_ORG) {
            lex_next(&lx);
            ec.pc = pc; ec.error = 0;
            long v;
            if (eval_expr(&lx, &ec, &v) < 0 || v < 0)
                return fail(out, lx.line, "bad .org expression");
            pc = (unsigned)v;
        } else if (lx.tok.kind == T_DIR_WORD) {
            pc += 4;
        } else if (lx.tok.kind == T_DIR_HALF) {
            pc += 2;
        } else if (lx.tok.kind == T_DIR_BYTE) {
            pc += 1;
        } else if (lx.tok.kind == T_DIR_ENTRY) {
            /* size zero */
        } else if (lx.tok.kind == T_DIR_COMPRESS || lx.tok.kind == T_DIR_NOCOMPRESS) {
            return fail(out, lx.line,
                        "compressed mode was retired in BradISA V2; "
                        "every instruction is 32 bits");
        } else if (lx.tok.kind == T_DIR_UNKNOWN) {
            return fail(out, lx.line, "%s", lx.tok.text);
        } else {
            return fail(out, lx.line, "unexpected token before instruction");
        }

        while (lx.tok.kind != T_NEWLINE && lx.tok.kind != T_EOF)
            lex_next(&lx);
    }
    return 0;
}

/* ─── Pass 2: encode and emit ────────────────────────────────────────── */

static int pass_emit(const char *source, struct brad_as_output *out)
{
    struct lexer lx;
    lex_init(&lx, source);

    unsigned pc = 0, high = 0;
    struct eval_ctx ec = { out->symbols, out->nsymbols, 0, 0 };

    for (;;) {
        lex_next(&lx);
        if (lx.tok.kind == T_EOF)
            break;
        if (lx.tok.kind == T_NEWLINE)
            continue;

        if (lx.tok.kind == T_ID) {
            char name[64];
            strncpy(name, lx.tok.text, sizeof name - 1);
            name[sizeof name - 1] = '\0';
            lex_next(&lx);

            if (lx.tok.kind == T_COLON)
                continue;

            const struct insn_def *d = lookup_insn(name);
            if (!d) {
                if (name[0] == 'C' && name[1] == '.')
                    return fail(out, lx.line,
                                "compressed instruction %s is V1-only; "
                                "BradISA V2 is fixed 32-bit", name);
                return fail(out, lx.line, "unknown instruction '%s'", name);
            }

            if (process_insn(&lx, d, &pc, out, 1, &ec) < 0)
                return -1;
            if (pc > high)
                high = pc;

            while (lx.tok.kind != T_NEWLINE && lx.tok.kind != T_EOF)
                lex_next(&lx);
            continue;
        }

        if (lx.tok.kind == T_DIR_ORG) {
            lex_next(&lx);
            ec.pc = pc; ec.error = 0;
            long v;
            if (eval_expr(&lx, &ec, &v) < 0 || v < 0)
                return fail(out, lx.line, "bad .org expression");
            pc = (unsigned)v;
        } else if (lx.tok.kind == T_DIR_ENTRY) {
            lex_next(&lx);
            ec.pc = pc; ec.error = 0;
            long v;
            if (eval_expr(&lx, &ec, &v) < 0)
                return fail(out, lx.line, "bad .entry expression");
            out->entry_point = (unsigned)v;
        } else if (lx.tok.kind == T_DIR_WORD || lx.tok.kind == T_DIR_HALF ||
                   lx.tok.kind == T_DIR_BYTE) {
            int nbytes = (lx.tok.kind == T_DIR_WORD) ? 4 :
                         (lx.tok.kind == T_DIR_HALF) ? 2 : 1;
            lex_next(&lx);
            ec.pc = pc; ec.error = 0;
            long v;
            if (eval_expr(&lx, &ec, &v) < 0)
                return fail(out, lx.line, "bad data expression");
            if (emit_bytes(out, pc, v, nbytes) < 0)
                return fail(out, lx.line, "output limit of %d words exceeded",
                            BRAD_ASM_MAX_OUTPUT);
            pc += (unsigned)nbytes;
            if (pc > high)
                high = pc;
        } else if (lx.tok.kind == T_DIR_COMPRESS || lx.tok.kind == T_DIR_NOCOMPRESS) {
            return fail(out, lx.line,
                        "compressed mode was retired in BradISA V2; "
                        "every instruction is 32 bits");
        } else if (lx.tok.kind == T_DIR_UNKNOWN) {
            return fail(out, lx.line, "%s", lx.tok.text);
        } else {
            return fail(out, lx.line, "unexpected token before instruction");
        }

        while (lx.tok.kind != T_NEWLINE && lx.tok.kind != T_EOF)
            lex_next(&lx);
    }

    out->count = (high + 3) / 4;
    return 0;
}

/* ─── Public entry point ─────────────────────────────────────────────── */

int brad_as_assemble(const char *source, struct brad_as_output *out)
{
    memset(out, 0, sizeof *out);
    out->ok = 1;

    if (!source)
        return fail(out, 0, "no source");

    if (pass_measure(source, out) < 0)
        return -1;
    if (pass_emit(source, out) < 0)
        return -1;

    out->ok = 1;
    return 0;
}
