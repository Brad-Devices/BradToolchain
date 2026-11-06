/*
 * test_brad_as -- BradISA V2 assembler.
 *
 * Two kinds of check:
 *
 *   1. Exact words.  Each format gets at least one literal 32-bit value
 *      computed by hand from bradisa_spec.tex §3, so a field move or a
 *      wrong shift fails here rather than in a program six months later.
 *
 *   2. Coverage.  Every mnemonic in the V2 instruction set is assembled at
 *      least once and its opcode field is checked against the BRAD_V2_OP_*
 *      constant it is supposed to encode.
 *
 * The assembler is the toolchain's side of the ISA contract. If it drifts
 * from the RTL, tools/check_isa_drift.py catches that; this file catches
 * the choices inside the assembler itself.
 */

#include "brad/bradasm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct brad_as_output out;

static uint32_t opcode_of(uint32_t word)
{
    return (word >> BRAD_V2_OPCODE_SHIFT) & BRAD_V2_OPCODE_MASK;
}

static uint32_t rd_of(uint32_t word)
{
    return (word >> BRAD_V2_RD_SHIFT) & BRAD_V2_REG_MASK;
}

static uint32_t rs1_of(uint32_t word)
{
    return (word >> BRAD_V2_RS1_SHIFT) & BRAD_V2_REG_MASK;
}

static uint32_t rs2_of(uint32_t word)
{
    return (word >> BRAD_V2_RS2_SHIFT) & BRAD_V2_REG_MASK;
}

static uint32_t assemble_one(const char *src)
{
    memset(&out, 0, sizeof out);
    if (brad_as_assemble(src, &out) != 0) {
        fprintf(stderr, "failed to assemble: %s\n  %s\n", src, out.error.msg);
        assert(0);
    }
    assert(out.ok);
    assert(out.count == 1);
    return out.code[0];
}

static void expect_ok(const char *src)
{
    memset(&out, 0, sizeof out);
    assert(brad_as_assemble(src, &out) == 0);
    assert(out.ok);
}

static void expect_error(const char *src, const char *needle)
{
    memset(&out, 0, sizeof out);
    if (brad_as_assemble(src, &out) == 0 || out.ok) {
        fprintf(stderr, "expected an error from: %s\n", src);
        assert(0);
    }
    if (needle && strstr(out.error.msg, needle) == NULL) {
        fprintf(stderr, "error for %s was '%s', expected to contain '%s'\n",
                src, out.error.msg, needle);
        assert(0);
    }
}

/* ─── Exact encodings, one per format ────────────────────────────────── */

static void test_exact_words(void)
{
    assert(assemble_one("NOP") == 0x00000000u);
    assert(assemble_one("ADD r3, r1, r2") == 0x04611000u);
    assert(assemble_one("ADDI r1, r0, -1") == 0x4020FFFFu);
    assert(assemble_one("ANDI r2, r3, 0xFFFF") == 0x4443FFFFu);
    assert(assemble_one("LDW r3, [r1 + 4]") == 0x80610004u);
    assert(assemble_one("STW r2, [r1 + 4]") == 0x8C410004u);
    assert(assemble_one("LUI r1, 0x12345") == 0xA0212345u);
    assert(assemble_one("AUIPC r2, 1") == 0xA4400001u);
    assert(assemble_one("BEQ r1, r2, 8") == 0xC0220001u);
    assert(assemble_one("BZ r5, 8") == 0xD8A00001u);
    assert(assemble_one("JMP 8") == 0xA8000001u);
    assert(assemble_one("JAL r15, 8") == 0xADE00001u);
    assert(assemble_one("CALL 8") == 0xB1E00001u);
    assert(assemble_one("JALR r15, r1, 0") == 0xB5E10000u);
    assert(assemble_one("RET") == 0xB8000000u);
    assert(assemble_one("FENCE") == 0xBC000000u);
    assert(assemble_one("LR.W r3, [r1]") == 0xE0610000u);
    assert(assemble_one("SC.W r3, r2, [r1]") == 0xE4611000u);
    assert(assemble_one("AMOSWAP.W r3, r2, [r1]") == 0xE8611000u);
    assert(assemble_one("AMOADD.W r3, r2, [r1]") == 0xEC611000u);
}

/* ─── Every mnemonic, and its opcode field ───────────────────────────── */

struct mnemonic_case {
    const char *src;
    enum brad_isa_v2_opcode op;
};

static const struct mnemonic_case mnemonics[] = {
    {"NOP",                 BRAD_V2_OP_NOP},
    {"ADD  r1, r2, r3",     BRAD_V2_OP_ADD},
    {"SUB  r1, r2, r3",     BRAD_V2_OP_SUB},
    {"MUL  r1, r2, r3",     BRAD_V2_OP_MUL},
    {"MULHU r1, r2, r3",    BRAD_V2_OP_MULHU},
    {"DIV  r1, r2, r3",     BRAD_V2_OP_DIV},
    {"DIVU r1, r2, r3",     BRAD_V2_OP_DIVU},
    {"REM  r1, r2, r3",     BRAD_V2_OP_REM},
    {"REMU r1, r2, r3",     BRAD_V2_OP_REMU},
    {"AND  r1, r2, r3",     BRAD_V2_OP_AND},
    {"OR   r1, r2, r3",     BRAD_V2_OP_OR},
    {"XOR  r1, r2, r3",     BRAD_V2_OP_XOR},
    {"SHL  r1, r2, r3",     BRAD_V2_OP_SHL},
    {"SHR  r1, r2, r3",     BRAD_V2_OP_SHR},
    {"SAR  r1, r2, r3",     BRAD_V2_OP_SAR},

    {"ADDI r1, r2, 1",      BRAD_V2_OP_ADDI},
    {"ANDI r1, r2, 1",      BRAD_V2_OP_ANDI},
    {"ORI  r1, r2, 1",      BRAD_V2_OP_ORI},
    {"XORI r1, r2, 1",      BRAD_V2_OP_XORI},
    {"SLLI r1, r2, 1",      BRAD_V2_OP_SLLI},
    {"SRLI r1, r2, 1",      BRAD_V2_OP_SRLI},
    {"SRAI r1, r2, 1",      BRAD_V2_OP_SRAI},
    {"SLTI r1, r2, 1",      BRAD_V2_OP_SLTI},
    {"SLTIU r1, r2, 1",     BRAD_V2_OP_SLTIU},

    {"LDW  r1, [r2 + 0]",   BRAD_V2_OP_LDW},
    {"LHU  r1, [r2 + 0]",   BRAD_V2_OP_LHU},
    {"LHBU r1, [r2 + 0]",   BRAD_V2_OP_LHBU},
    {"STW  r1, [r2 + 0]",   BRAD_V2_OP_STW},
    {"SH   r1, [r2 + 0]",   BRAD_V2_OP_SH},
    {"SB   r1, [r2 + 0]",   BRAD_V2_OP_SB},

    {"LUI   r1, 1",         BRAD_V2_OP_LUI},
    {"AUIPC r1, 1",         BRAD_V2_OP_AUIPC},

    {"JMP  8",              BRAD_V2_OP_JMP},
    {"JAL  r1, 8",          BRAD_V2_OP_JAL},
    {"CALL 8",              BRAD_V2_OP_CALL},
    {"JALR r1, r2, 0",      BRAD_V2_OP_JALR},
    {"RET",                 BRAD_V2_OP_RET},
    {"FENCE",               BRAD_V2_OP_FENCE},

    {"BEQ  r1, r2, 8",      BRAD_V2_OP_BEQ},
    {"BNE  r1, r2, 8",      BRAD_V2_OP_BNE},
    {"BLT  r1, r2, 8",      BRAD_V2_OP_BLT},
    {"BGE  r1, r2, 8",      BRAD_V2_OP_BGE},
    {"BLTU r1, r2, 8",      BRAD_V2_OP_BLTU},
    {"BGEU r1, r2, 8",      BRAD_V2_OP_BGEU},
    {"BZ   r1, 8",          BRAD_V2_OP_BZ},
    {"BNZ  r1, 8",          BRAD_V2_OP_BNZ},

    {"LR.W      r1, [r2]",      BRAD_V2_OP_LR_W},
    {"SC.W      r1, r3, [r2]",  BRAD_V2_OP_SC_W},
    {"AMOSWAP.W r1, r3, [r2]",  BRAD_V2_OP_AMOSWAP_W},
    {"AMOADD.W  r1, r3, [r2]",  BRAD_V2_OP_AMOADD_W},
};

static void test_every_mnemonic(void)
{
    unsigned n = sizeof mnemonics / sizeof mnemonics[0];
    assert(n == 50);

    for (unsigned i = 0; i < n; i++) {
        uint32_t word = assemble_one(mnemonics[i].src);
        if (opcode_of(word) != (uint32_t)mnemonics[i].op) {
            fprintf(stderr, "mnemonic %s encoded opcode 0x%X, want 0x%X\n",
                    mnemonics[i].src, opcode_of(word),
                    (unsigned)mnemonics[i].op);
            assert(0);
        }
    }
}

/* ─── Fields, ABI names, labels ──────────────────────────────────────── */

static void test_fields_and_abi(void)
{
    /* ABI names resolve to the published register map. */
    uint32_t w = assemble_one("ADD a0, t0, sp");
    assert(rd_of(w) == BRAD_V2_A0);       /* r1  */
    assert(rs1_of(w) == BRAD_V2_T0);      /* r9  */
    assert(rs2_of(w) == BRAD_V2_SP);      /* r27 */

    w = assemble_one("ADD lr, ra, zero");
    assert(rd_of(w) == 15);
    assert(rs1_of(w) == 15);
    assert(rs2_of(w) == 0);

    /* The non-bracket address form is the same encoding. */
    assert(assemble_one("LDW r3, r1, 4") == assemble_one("LDW r3, [r1 + 4]"));
    assert(assemble_one("STW r2, r1, 4") == assemble_one("STW r2, [r1 + 4]"));

    /* Branch offsets are relative to pc + 4, then divided by four. */
    w = assemble_one("BEQ r1, r2, 12");
    assert((w & 0xFFFFu) == 2);

    /* A label resolves to the same address a literal would name. */
    expect_ok(".org 0\nstart: BZ r1, done\nADD r2, r0, r0\ndone: RET\n");
    assert(out.count == 3);
    assert(out.code[0] == 0xD8200001u);   /* BZ r1, +4 -> word offset 1 */
    assert(out.code[1] == 0x04400000u);   /* ADD r2, r0, r0 */
    assert(out.code[2] == BRAD_V2_RET_ENCODING);

    /* .entry records the symbol address, not the line number. */
    expect_ok(".org 0x100\n_start: RET\n.entry _start\n");
    assert(out.entry_point == 0x100);
    assert(out.nsymbols == 1);
    assert(strcmp(out.symbols[0].name, "_start") == 0);

    /* Comments and case-insensitivity. */
    assert(assemble_one("add r1, r2, r3   # lower case, hash comment") ==
           assemble_one("ADD r1, r2, r3 ; semicolon comment"));
}

/* ─── Data directives ────────────────────────────────────────────────── */

static void test_data(void)
{
    expect_ok(".word 0xDEADBEEF\n");
    assert(out.count == 1);
    assert(out.code[0] == 0xDEADBEEFu);

    expect_ok(".byte 0x12\n");
    assert(out.count == 1);
    assert(out.code[0] == 0x00000012u);

    expect_ok(".byte 1\n.byte 2\n.byte 3\n.byte 4\n");
    assert(out.count == 1);
    assert(out.code[0] == 0x04030201u);

    expect_ok(".half 0x1234\n.half 0x5678\n");
    assert(out.count == 1);
    assert(out.code[0] == 0x56781234u);

    expect_ok(".org 8\nRET\n");
    assert(out.count == 3);
    assert(out.code[2] == BRAD_V2_RET_ENCODING);
    assert(out.code[0] == 0 && out.code[1] == 0);
}

/* ─── Errors that must not become silent encodings ───────────────────── */

static void test_errors(void)
{
    expect_error("FOO r1, r2, r3", "unknown instruction");
    expect_error("ADD r32, r0, r0", "syntax error");
    expect_error("ADD r1, r1", "syntax error");

    expect_error(".compress", "retired");
    expect_error(".nocompress", "retired");
    expect_error("C.ADD r1, r2, r3", "V1-only");

    expect_error("BZ r1, 3", "not 4-byte aligned");
    expect_error("JMP 2", "not 4-byte aligned");
    expect_error("ADDI r1, r0, 70000", "out of range");
    expect_error("SLLI r1, r0, 32", "out of range");
    expect_error("LDW r1, [r2 + 0x10000]", "out of range");
    expect_error("LR.W r1, [r2 + 4]", "no offset");
    expect_error("JALR r1, r2, 70000", "out of range");

    expect_error("a: NOP\na: NOP\n", "duplicate label");
    expect_error("BZ r1, nowhere\n", "undefined symbol");
    expect_error(".word nowhere\n", "bad data expression");
}

int main(void)
{
    test_exact_words();
    test_every_mnemonic();
    test_fields_and_abi();
    test_data();
    test_errors();

    printf("brad_as: PASS -- exact words, 50 mnemonics, labels, data, errors\n");
    return 0;
}
