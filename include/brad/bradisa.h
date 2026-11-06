#ifndef BRAD_ISA_H
#define BRAD_ISA_H

#include <stdint.h>
#include <stddef.h>

/* ─── Architecture Identification ─── */

#define BRAD_ISA_NAME            "BradISA"
#define BRAD_ISA_VERSION_MAJOR   1
#define BRAD_ISA_VERSION_MINOR   0
#define BRAD_ISA_V1              0x0100
#define BRAD_ISA_V2              0x0200

#define BRAD_ISA_STRING          "BradISA V1"

/* ─── Data Model ─── */

#define BRAD_WORD_BITS           32
#define BRAD_WORD_BYTES          4
#define BRAD_HWORD_BITS          16
#define BRAD_HWORD_BYTES         2
#define BRAD_ADDR_BITS           32
#define BRAD_PAGE_SIZE           4096

/* ═══════════════════════════════════════════════════════════════════════
 * BradISA V2 -- the canonical encoding
 *
 * Six-bit opcode, five-bit register fields, seven formats, every
 * instruction exactly 32 bits.  This block is the C side of the contract
 * published in src/spec/bradisa_spec.tex and implemented in
 * src/rtl/verilog/bradisa_defines.v; tools/check_isa_drift.py compares the
 * three of them and fails the build if any disagree.
 *
 * The V1 block below is retained only because the cycle emulator in
 * src/emu still decodes V1 binaries.  New code -- the assembler, the RTL,
 * the specification -- is V2 and must not use the V1 macros.
 * ═══════════════════════════════════════════════════════════════════════ */

#define BRAD_V2_INSN_SIZE        4
#define BRAD_V2_OPCODE_BITS      6
#define BRAD_V2_REG_BITS         5
#define BRAD_V2_NUM_REGS         32
#define BRAD_V2_OPCODE_SHIFT     26
#define BRAD_V2_RD_SHIFT         21
#define BRAD_V2_RS1_SHIFT        16
#define BRAD_V2_RS2_SHIFT        11
#define BRAD_V2_OPCODE_MASK      0x3Fu
#define BRAD_V2_REG_MASK         0x1Fu
#define BRAD_V2_IMM16_MASK       0xFFFFu
#define BRAD_V2_IMM21_MASK       0x1FFFFFu

/* ─── V2 opcodes (64 slots: 50 assigned, 14 reserved) ─── */

enum brad_isa_v2_opcode {
    /* ALU, register-register (0x00-0x0F): 15 used, 1 reserved */
    BRAD_V2_OP_NOP      = 0x00,
    BRAD_V2_OP_ADD      = 0x01,
    BRAD_V2_OP_SUB      = 0x02,
    BRAD_V2_OP_MUL      = 0x03,
    BRAD_V2_OP_MULHU    = 0x04,
    BRAD_V2_OP_DIV      = 0x05,
    BRAD_V2_OP_DIVU     = 0x06,
    BRAD_V2_OP_REM      = 0x07,
    BRAD_V2_OP_REMU     = 0x08,
    BRAD_V2_OP_AND      = 0x09,
    BRAD_V2_OP_OR       = 0x0A,
    BRAD_V2_OP_XOR      = 0x0B,
    BRAD_V2_OP_SHL      = 0x0C,
    BRAD_V2_OP_SHR      = 0x0D,
    BRAD_V2_OP_SAR      = 0x0E,
    /* 0x0F reserved */

    /* ALU, register-immediate (0x10-0x1F): 9 used, 7 reserved */
    BRAD_V2_OP_ADDI     = 0x10,
    BRAD_V2_OP_ANDI     = 0x11,
    BRAD_V2_OP_ORI      = 0x12,
    BRAD_V2_OP_XORI     = 0x13,
    BRAD_V2_OP_SLLI     = 0x14,
    BRAD_V2_OP_SRLI     = 0x15,
    BRAD_V2_OP_SRAI     = 0x16,
    BRAD_V2_OP_SLTI     = 0x17,
    BRAD_V2_OP_SLTIU    = 0x18,
    /* 0x19-0x1F reserved */

    /* Load / store (0x20-0x27): 6 used, 2 reserved */
    BRAD_V2_OP_LDW      = 0x20,
    BRAD_V2_OP_LHU      = 0x21,
    BRAD_V2_OP_LHBU     = 0x22,
    BRAD_V2_OP_STW      = 0x23,
    BRAD_V2_OP_SH       = 0x24,
    BRAD_V2_OP_SB       = 0x25,
    /* 0x26-0x27 reserved */

    /* Upper immediate and control transfer (0x28-0x2F): 8 used */
    BRAD_V2_OP_LUI      = 0x28,
    BRAD_V2_OP_AUIPC    = 0x29,
    BRAD_V2_OP_JMP      = 0x2A,
    BRAD_V2_OP_JAL      = 0x2B,
    BRAD_V2_OP_CALL     = 0x2C,
    BRAD_V2_OP_JALR     = 0x2D,
    BRAD_V2_OP_RET      = 0x2E,
    BRAD_V2_OP_FENCE    = 0x2F,

    /* Branches (0x30-0x37): 8 used */
    BRAD_V2_OP_BEQ      = 0x30,
    BRAD_V2_OP_BNE      = 0x31,
    BRAD_V2_OP_BLT      = 0x32,
    BRAD_V2_OP_BGE      = 0x33,
    BRAD_V2_OP_BLTU     = 0x34,
    BRAD_V2_OP_BGEU     = 0x35,
    BRAD_V2_OP_BZ       = 0x36,
    BRAD_V2_OP_BNZ      = 0x37,

    /* Atomics (0x38-0x3B): 4 used; 0x3C-0x3F are extension escape */
    BRAD_V2_OP_LR_W     = 0x38,
    BRAD_V2_OP_SC_W     = 0x39,
    BRAD_V2_OP_AMOSWAP_W= 0x3A,
    BRAD_V2_OP_AMOADD_W = 0x3B,
};

/* ─── V2 formats ─── */

enum brad_isa_v2_format {
    BRAD_V2_FMT_RRR = 0,
    BRAD_V2_FMT_RI,
    BRAD_V2_FMT_SI,
    BRAD_V2_FMT_BR,
    BRAD_V2_FMT_IJ,
    BRAD_V2_FMT_JMP,
    BRAD_V2_FMT_U,
};

/* ─── V2 register numbers and ABI names ─── */
/* The ABI is the one published in bradisa_spec.tex §3.2: eight
 * arguments, six temporaries, eleven callee-saved, a link register, a
 * stack pointer, a global pointer and a thread pointer.  r30 and r31
 * are reserved.  The link register is r15. */

#define BRAD_V2_R0   0
#define BRAD_V2_A0   1
#define BRAD_V2_A1   2
#define BRAD_V2_A2   3
#define BRAD_V2_A3   4
#define BRAD_V2_A4   5
#define BRAD_V2_A5   6
#define BRAD_V2_A6   7
#define BRAD_V2_A7   8
#define BRAD_V2_T0   9
#define BRAD_V2_T1   10
#define BRAD_V2_T2   11
#define BRAD_V2_T3   12
#define BRAD_V2_T4   13
#define BRAD_V2_T5   14
#define BRAD_V2_LR   15
#define BRAD_V2_RA   15   /* alias: return address */
#define BRAD_V2_S0   16
#define BRAD_V2_S1   17
#define BRAD_V2_S2   18
#define BRAD_V2_S3   19
#define BRAD_V2_S4   20
#define BRAD_V2_S5   21
#define BRAD_V2_S6   22
#define BRAD_V2_S7   23
#define BRAD_V2_S8   24
#define BRAD_V2_S9   25
#define BRAD_V2_S10  26
#define BRAD_V2_SP   27
#define BRAD_V2_GP   28
#define BRAD_V2_TP   29

/* ─── V2 constructors ─── */
/* The macros take the field value as it appears in the instruction word.
 * Byte displacements are scaled by the caller (the assembler does this),
 * because the RTL decode stage multiplies BR/JMP offsets by four. */

#define BRAD_V2_RRR(op, rd, rs1, rs2) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rd)  & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     (((uint32_t)(rs1) & BRAD_V2_REG_MASK) << BRAD_V2_RS1_SHIFT) | \
     (((uint32_t)(rs2) & BRAD_V2_REG_MASK) << BRAD_V2_RS2_SHIFT))

#define BRAD_V2_RI(op, rd, rs1, imm) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rd)  & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     (((uint32_t)(rs1) & BRAD_V2_REG_MASK) << BRAD_V2_RS1_SHIFT) | \
     ((uint32_t)(imm) & BRAD_V2_IMM16_MASK))

#define BRAD_V2_SI(op, rs2, rs1, imm) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rs2) & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     (((uint32_t)(rs1) & BRAD_V2_REG_MASK) << BRAD_V2_RS1_SHIFT) | \
     ((uint32_t)(imm) & BRAD_V2_IMM16_MASK))

#define BRAD_V2_BR(op, rs1, rs2, off) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rs1) & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     (((uint32_t)(rs2) & BRAD_V2_REG_MASK) << BRAD_V2_RS1_SHIFT) | \
     ((uint32_t)(off) & BRAD_V2_IMM16_MASK))

#define BRAD_V2_IJ(op, rd, rs1, imm) \
    BRAD_V2_RI((op), (rd), (rs1), (imm))

#define BRAD_V2_JMP(op, rd, off) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rd)  & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     ((uint32_t)(off) & BRAD_V2_IMM21_MASK))

#define BRAD_V2_U(op, rd, imm) \
    (((uint32_t)(op)  << BRAD_V2_OPCODE_SHIFT) | \
     (((uint32_t)(rd)  & BRAD_V2_REG_MASK) << BRAD_V2_RD_SHIFT) | \
     ((uint32_t)(imm) & BRAD_V2_IMM21_MASK))

/* Fixed encodings.  RET is opcode 0x2E in the six-bit field, so the
 * instruction word is 0xB8000000 -- not 0x2E000000, which would decode
 * as opcode 0x0B under the V2 field layout. */
#define BRAD_V2_RET_ENCODING     0xB8000000u
#define BRAD_V2_NOP_ENCODING     0x00000000u

/* ─── V1 (legacy emulator) ─── */

/* ─── Register File ─── */

#define BRAD_NUM_REGS            16
#define BRAD_NUM_REGS_LITE       8

#define BRAD_REG_R0              0
#define BRAD_REG_R1              1
#define BRAD_REG_R2              2
#define BRAD_REG_R3              3
#define BRAD_REG_R4              4
#define BRAD_REG_R5              5
#define BRAD_REG_R6              6
#define BRAD_REG_R7              7
#define BRAD_REG_R8              8
#define BRAD_REG_R9              9
#define BRAD_REG_R10             10
#define BRAD_REG_R11             11
#define BRAD_REG_R12             12
#define BRAD_REG_SP              13
#define BRAD_REG_LR              14
#define BRAD_REG_PC              15

#define BRAD_ABI_CALLEE_SAVE_MIN BRAD_REG_R8
#define BRAD_ABI_CALLEE_SAVE_MAX BRAD_REG_R12
#define BRAD_ABI_ARG0            BRAD_REG_R1
#define BRAD_ABI_ARG1            BRAD_REG_R2
#define BRAD_ABI_ARG2            BRAD_REG_R3
#define BRAD_ABI_ARG3            BRAD_REG_R4
#define BRAD_ABI_RET0            BRAD_REG_R1

/* ─── V2 (Kinetic V2) — additive over V1 ───
 *
 * V1 (above) is what runs on the FPGA today: 16 GPRs, SP at 13, LR at 14,
 * PC at 15, 4 arguments. V2 widens the register file and moves the ABI; the
 * kernel is written once against roles and adapts via bradk_arch_v1/v2
 * (BRADKERNEL_ABI.md §0). The V1 block above stays true — the FPGA runs it.
 */

#define BRAD_ISA_V2_REGS             32
#define BRAD_V2_ABI_ARGS             8    /* r1-r8; names are in the enum below */
#define BRAD_V2_ABI_CALLEE_SAVE_MIN  16
#define BRAD_V2_ABI_CALLEE_SAVE_MAX  26
#define BRAD_V2_ABI_TEMP_MIN         9
#define BRAD_V2_ABI_TEMP_MAX         14

/* V2 opcodes live in the enum above: FENCE is 0x2F, LR.W..AMOADD.W fill
 * 0x38-0x3B, and 0x3C-0x3F are the extension escape.  Do not restate them
 * as macros here — the drift gate reads the enum and the Verilog package.
 *
 * ECALL and RFE have no assigned opcode in V2.  Trap entry is still
 * software-injected (BRADKERNEL_COHERENCE.md I-09 / D1).  A syscall's
 * exception-vector address is 0x14; that is a table entry, not an opcode.
 * The opcode at 0x14 is SLLI. */

/* ─── V1 Instruction Formats (32-bit fixed) ─── */

#define BRAD_INSN_SIZE           4
#define BRAD_OPCODE_BITS         4
#define BRAD_REG_BITS            4
#define BRAD_IMM_BITS            16
#define BRAD_JM_IMM_BITS         28

#define BRAD_OPCODE_SHIFT        28
#define BRAD_RD_SHIFT            24
#define BRAD_RS1_SHIFT           20
#define BRAD_RS2_SHIFT           16
#define BRAD_IMM_MASK            0xFFFF

#define BRAD_INSN_OPCODE(i)      (((i) >> 28) & 0xF)
#define BRAD_INSN_RD(i)          (((i) >> 24) & 0xF)
#define BRAD_INSN_RS1(i)         (((i) >> 20) & 0xF)
#define BRAD_INSN_RS2(i)         (((i) >> 16) & 0xF)
#define BRAD_INSN_IMM(i)         ((int16_t)((i) & 0xFFFF))

#define BRAD_RRR(op, rd, rs1, rs2)  (((op) << 28) | ((rd) << 24) | ((rs1) << 20) | ((rs2) << 16))
#define BRAD_RI(op, rd, rs1, imm)   (((op) << 28) | ((rd) << 24) | ((rs1) << 20) | ((imm) & 0xFFFF))
#define BRAD_BR(op, rs1, imm)       (((op) << 28) | ((rs1) << 20) | (((imm) * 4) & 0xFFFF))
#define BRAD_JMP(op, imm)           (((op) << 28) | (((imm) * 4) & 0xFFFF))
#define BRAD_RET_RAW                0xF0000000U

/* ─── V1 Opcodes ─── */

enum brad_isa_opcode {
    BRAD_OP_ADD  = 0x0,
    BRAD_OP_SUB  = 0x1,
    BRAD_OP_MUL  = 0x2,
    BRAD_OP_AND  = 0x3,
    BRAD_OP_OR   = 0x4,
    BRAD_OP_XOR  = 0x5,
    BRAD_OP_SHL  = 0x6,
    BRAD_OP_SHR  = 0x7,
    BRAD_OP_ADDI = 0x8,
    BRAD_OP_LDW  = 0x9,
    BRAD_OP_STW  = 0xA,
    BRAD_OP_BZ   = 0xB,
    BRAD_OP_BNZ  = 0xC,
    BRAD_OP_JMP  = 0xD,
    BRAD_OP_CALL = 0xE,
    BRAD_OP_RET  = 0xF,
};

/* ─── V2 Compressed Instruction Formats (16-bit) ─── */

/* Compressed mode is activated by STATUS.C flag (bit 4).
   When C=1, PC advances by 2 (halfword) and each halfword
   encodes one 16-bit instruction. */

#define BRAD_CINSN_SIZE          2
#define BRAD_COP_BITS            4
#define BRAD_CIMM4_BITS          4
#define BRAD_CIMM8_BITS          8

#define BRAD_COP_SHIFT           12
#define BRAD_CRD_SHIFT           8
#define BRAD_CRS1_SHIFT          4
#define BRAD_CRS2_SHIFT          0

#define BRAD_CINSN_OP(i)         (((i) >> 12) & 0xF)
#define BRAD_CINSN_RD(i)         (((i) >> 8)  & 0xF)
#define BRAD_CINSN_RS1(i)        (((i) >> 4)  & 0xF)
#define BRAD_CINSN_RS2(i)        (((i) >> 0)  & 0xF)

/* Compressed instruction constructors (value is 16-bit) */
#define BRAD_CRRR(op, rd, rs1, rs2) (((op) << 12) | ((rd) << 8) | ((rs1) << 4) | (rs2))
#define BRAD_CRI(op, rd, rs1, imm4) (((op) << 12) | ((rd) << 8) | ((rs1) << 4) | ((imm4) & 0xF))
#define BRAD_CBR(op, rs1, off8)     (((op) << 12) | ((rs1) << 8) | ((off8) & 0xFF))
#define BRAD_CMV(rd, imm8)          ((0xCu << 12) | ((rd) << 8) | ((imm8) & 0xFF))

enum brad_isa_cop {
    BRAD_COP_ADD  = 0x0,
    BRAD_COP_SUB  = 0x1,
    BRAD_COP_AND  = 0x2,
    BRAD_COP_OR   = 0x3,
    BRAD_COP_XOR  = 0x4,
    BRAD_COP_SHL  = 0x5,
    BRAD_COP_SHR  = 0x6,
    BRAD_COP_ADDI = 0x7,
    BRAD_COP_LDW  = 0x8,
    BRAD_COP_STW  = 0x9,
    BRAD_COP_BZ   = 0xA,
    BRAD_COP_BNZ  = 0xB,
    BRAD_COP_MV   = 0xC,
    BRAD_COP_NOP  = 0xD,
    BRAD_COP_JMP  = 0xE,
    BRAD_COP_CALL = 0xF,
};

#define BRAD_CADDI(rd, rs1, imm4) BRAD_CRI(0x7, rd, rs1, imm4)
#define BRAC_CLDW(rd, rs1, imm4)  BRAD_CRI(0x8, rd, rs1, imm4)
#define BRAC_CSTW(rd, rs1, imm4)  BRAD_CRI(0x9, rd, rs1, imm4)
#define BRAD_CBZ(rs1, off8)       BRAD_CBR(0xA, rs1, off8)
#define BRAD_CBNZ(rs1, off8)      BRAD_CBR(0xB, rs1, off8)
#define BRAD_CNOP                 ((uint16_t)0xD000)
#define BRAD_CJMP(off8)           BRAD_CBR(0xE, 0, off8)
#define BRAD_CCALL(off8)          BRAD_CBR(0xF, 0, off8)

/* BradISA V2 Vector Extension (VSET): 32 x 256-bit registers, opcode 0xF */
#define BRAD_VEC_OP             0xF
#define BRAD_VEC_RET            0xF0000000u  /* V1 RET (backward compat) */
#define BRAD_VEC_NREG           32
#define BRAD_VEC_VLEN           256
#define BRAD_VEC_SEW_8          0
#define BRAD_VEC_SEW_16         1
#define BRAD_VEC_SEW_32         2
#define BRAD_VEC_SEW_64         3

#define BRAD_VEC_CLS_VALU       0x0   /* integer elementwise */
#define BRAD_VEC_CLS_VFP        0x1   /* FP elementwise */
#define BRAD_VEC_CLS_VMEM       0x2   /* vector memory */
#define BRAD_VEC_CLS_VRED       0x3   /* reductions */
#define BRAD_VEC_CLS_VCVT       0x4   /* convert/widen/narrow */
#define BRAD_VEC_CLS_VSPL       0x5   /* scalar-vector */
#define BRAD_VEC_CLS_VSC        0x6   /* scalar extract */
#define BRAD_VEC_CLS_RSVD       0x7

#define BRAD_VEC_FUNCT_ADD      0x01
#define BRAD_VEC_FUNCT_SUB      0x02
#define BRAD_VEC_FUNCT_MUL      0x03
#define BRAD_VEC_FUNCT_FADD     0x01
#define BRAD_VEC_FUNCT_FSUB     0x02
#define BRAD_VEC_FUNCT_FMUL     0x03
#define BRAD_VEC_FUNCT_FMADD    0x04
#define BRAD_VEC_FUNCT_SUM      0x01
#define BRAD_VEC_FUNCT_FSUM     0x02
#define BRAD_VEC_FUNCT_DOT      0x07
#define BRAD_VEC_FUNCT_FDOT     0x08
#define BRAD_VEC_FUNCT_LW       0x0   /* mop 000 */
#define BRAD_VEC_FUNCT_SW       0x1   /* mop 001 */

#define BRAD_VEC_MSR_VSTATUS    23
#define BRAD_VEC_VSTATUS_VE     (1u << 0)
#define BRAD_VEC_VSTATUS_VILL   (1u << 5)
#define BRAD_VEC_VSTATUS_VFP    (1u << 6)

#define BRAD_VENC(class, vd, vs1, vs2, sew, funct) \
    (BRAD_VEC_OP << 28) | ((class & 0x7) << 25) | ((vd & 0x1f) << 20) | \
    ((vs1 & 0x1f) << 15) | ((vs2 & 0x1f) << 10) | ((sew & 0x3) << 8) | \
    (funct & 0xff)

/* Map compressed 16-bit instruction to equivalent V1 32-bit opcode */
static inline enum brad_isa_opcode brad_isa_cop_to_op(enum brad_isa_cop cop)
{
    switch (cop) {
        case BRAD_COP_ADD:  return BRAD_OP_ADD;
        case BRAD_COP_SUB:  return BRAD_OP_SUB;
        case BRAD_COP_AND:  return BRAD_OP_AND;
        case BRAD_COP_OR:   return BRAD_OP_OR;
        case BRAD_COP_XOR:  return BRAD_OP_XOR;
        case BRAD_COP_SHL:  return BRAD_OP_SHL;
        case BRAD_COP_SHR:  return BRAD_OP_SHR;
        case BRAD_COP_ADDI: return BRAD_OP_ADDI;
        case BRAD_COP_LDW:  return BRAD_OP_LDW;
        case BRAD_COP_STW:  return BRAD_OP_STW;
        case BRAD_COP_BZ:   return BRAD_OP_BZ;
        case BRAD_COP_BNZ:  return BRAD_OP_BNZ;
        case BRAD_COP_MV:   return BRAD_OP_ADDI;
        case BRAD_COP_NOP:  return BRAD_OP_ADD;   /* ADD r0, r0, r0 */
        case BRAD_COP_JMP:  return BRAD_OP_JMP;
        case BRAD_COP_CALL: return BRAD_OP_CALL;
    }
    return BRAD_OP_ADD;
}

#define BRAD_IS_ALU(op)          ((op) <= 0x7)
#define BRAD_IS_IMM(op)          ((op) == BRAD_OP_ADDI)
#define BRAD_IS_LOAD(op)         ((op) == BRAD_OP_LDW)
#define BRAD_IS_STORE(op)        ((op) == BRAD_OP_STW)
#define BRAD_IS_BRANCH(op)       ((op) >= BRAD_OP_BZ && (op) <= BRAD_OP_RET)
#define BRAD_IS_COND_BR(op)      ((op) == BRAD_OP_BZ || (op) == BRAD_OP_BNZ)
#define BRAD_IS_UNCOND_BR(op)    ((op) >= BRAD_OP_JMP && (op) <= BRAD_OP_RET)

/* ─── Opcode name strings ─── */

#define BRAD_OP_NAME_ADD  "ADD"
#define BRAD_OP_NAME_SUB  "SUB"
#define BRAD_OP_NAME_MUL  "MUL"
#define BRAD_OP_NAME_AND  "AND"
#define BRAD_OP_NAME_OR   "OR"
#define BRAD_OP_NAME_XOR  "XOR"
#define BRAD_OP_NAME_SHL  "SHL"
#define BRAD_OP_NAME_SHR  "SHR"
#define BRAD_OP_NAME_ADDI "ADDI"
#define BRAD_OP_NAME_LDW  "LDW"
#define BRAD_OP_NAME_STW  "STW"
#define BRAD_OP_NAME_BZ   "BZ"
#define BRAD_OP_NAME_BNZ  "BNZ"
#define BRAD_OP_NAME_JMP  "JMP"
#define BRAD_OP_NAME_CALL "CALL"
#define BRAD_OP_NAME_RET  "RET"

/* ─── IPC Estimates ─── */

/* Phoenix: 10-stage, octa-issue out-of-order, 128-entry ROB */
#define BRAD_IPC_PHOENIX_TYPICAL  220
#define BRAD_IPC_PHOENIX_PEAK     500

/* Falcon: 5-stage, single-issue in-order */
#define BRAD_IPC_FALCON_TYPICAL   90
#define BRAD_IPC_FALCON_PEAK      100

/* Kestrel: 8-stage, dual-issue in-order */
#define BRAD_IPC_KESTREL_TYPICAL  120
#define BRAD_IPC_KESTREL_PEAK     180

/* Falcon-Lite: 3-stage, single-issue in-order, 8 registers */
#define BRAD_IPC_FALCON_LITE_TYPICAL  55
#define BRAD_IPC_FALCON_LITE_PEAK     70

/* Composite DMIPS/MHz approximation (Dhrystone 2.1, GCC -O2) */
#define BRAD_DMIPS_PER_MHZ_PHOENIX    550
#define BRAD_DMIPS_PER_MHZ_KESTREL    300
#define BRAD_DMIPS_PER_MHZ_FALCON     220
#define BRAD_DMIPS_PER_MHZ_FALCON_LITE 120

/* Efficiency targets (DMIPS/W at typical freq) */
#define BRAD_EFF_PHOENIX_W      2200  /* 4.0 GHz, 10W/core */
#define BRAD_EFF_PHOENIX_C      4714  /* 3.0 GHz, 3.5W/core */
#define BRAD_EFF_KESTREL        3000  /* 3.5 GHz, 3.5W/core */
#define BRAD_EFF_FALCON         10560 /* 2.4 GHz, 0.5W/core */
#define BRAD_EFF_FALCON_LITE    8800  /* 1.2 GHz, 0.16W/core */

/* ─── Pipeline Models ─── */

enum brad_isa_pipeline_model {
    BRAD_PIPE_PHOENIX = 0,
    BRAD_PIPE_FALCON  = 1,
    BRAD_PIPE_KESTREL = 2,
    BRAD_PIPE_FALCON_LITE = 3,
};

#define BRAD_PHOENIX_DEPTH  10
#define BRAD_FALCON_DEPTH   5
#define BRAD_KESTREL_DEPTH  8
#define BRAD_FALCON_LITE_DEPTH 3

#define BRAD_PHOENIX_EXEC_STAGE  6
#define BRAD_FALCON_EXEC_STAGE   3
#define BRAD_KESTREL_EXEC_STAGE  4
#define BRAD_FALCON_LITE_EXEC_STAGE 1

/* ─── Phoenix Optimization Constants ─── */

/* Branch predictor types */
#define BRAD_BP_NONE         0
#define BRAD_BP_STATIC       1  /* BTFNT: Backward Taken, Forward Not Taken */
#define BRAD_BP_2BIT         2  /* 2-bit saturating counter per entry */
#define BRAD_BP_TOURNAMENT   3  /* Hybrid predictor (gshare + bimodal) */

/* Phoenix branch predictor configuration */
#define BRAD_PHOENIX_BP_DEFAULT     BRAD_BP_TOURNAMENT
#define BRAD_PHOENIX_BTB_SIZE       256
#define BRAD_PHOENIX_BTB_ASSOC      4
#define BRAD_PHOENIX_BP_GHIST_BITS  12

/* Load-forwarding: bypass writeback for load-to-use */
#define BRAD_PHOENIX_LF_ENABLE      1
#define BRAD_PHOENIX_LF_MAX_DIST    2

/* Clock-gating granularity (bitmask of gated units) */
#define BRAD_CG_FETCH       1
#define BRAD_CG_DECODE      2
#define BRAD_CG_RENAME      4
#define BRAD_CG_DISPATCH    8
#define BRAD_CG_ISSUE       16
#define BRAD_CG_EXEC        32
#define BRAD_CG_MEM         64
#define BRAD_CG_WB          128
#define BRAD_CG_ALL         255

/* ─── Decoded Instruction (V1) ─── */

struct brad_isa_insn {
    uint32_t            raw;
    enum brad_isa_opcode opcode;
    unsigned            rd;
    unsigned            rs1;
    unsigned            rs2;
    int16_t             immediate;
};

static inline struct brad_isa_insn brad_isa_decode(uint32_t raw)
{
    struct brad_isa_insn d;
    d.raw     = raw;
    d.opcode  = (enum brad_isa_opcode)BRAD_INSN_OPCODE(raw);
    d.rd      = BRAD_INSN_RD(raw);
    d.rs1     = BRAD_INSN_RS1(raw);
    d.rs2     = BRAD_INSN_RS2(raw);
    d.immediate = BRAD_INSN_IMM(raw);
    return d;
}

/* ─── Decoded Instruction (V2 compressed) ─── */

struct brad_isa_cinsn {
    uint16_t            raw;
    enum brad_isa_cop   cop;
    unsigned            rd;
    unsigned            rs1;
    unsigned            rs2;
    int16_t             immediate;  /* expanded from encoding */
};

static inline struct brad_isa_cinsn brad_isa_cdecode(uint16_t raw)
{
    struct brad_isa_cinsn d;
    d.raw     = raw;
    d.cop     = (enum brad_isa_cop)BRAD_CINSN_OP(raw);
    d.rd      = BRAD_CINSN_RD(raw);
    d.rs1     = BRAD_CINSN_RS1(raw);
    d.rs2     = BRAD_CINSN_RS2(raw);
    d.immediate = 0;
    return d;
}

/* ─── Instruction Name Lookup ─── */

static inline const char *brad_isa_opcode_name(enum brad_isa_opcode op)
{
    static const char *names[] = {
        [BRAD_OP_ADD]  = BRAD_OP_NAME_ADD,
        [BRAD_OP_SUB]  = BRAD_OP_NAME_SUB,
        [BRAD_OP_MUL]  = BRAD_OP_NAME_MUL,
        [BRAD_OP_AND]  = BRAD_OP_NAME_AND,
        [BRAD_OP_OR]   = BRAD_OP_NAME_OR,
        [BRAD_OP_XOR]  = BRAD_OP_NAME_XOR,
        [BRAD_OP_SHL]  = BRAD_OP_NAME_SHL,
        [BRAD_OP_SHR]  = BRAD_OP_NAME_SHR,
        [BRAD_OP_ADDI] = BRAD_OP_NAME_ADDI,
        [BRAD_OP_LDW]  = BRAD_OP_NAME_LDW,
        [BRAD_OP_STW]  = BRAD_OP_NAME_STW,
        [BRAD_OP_BZ]   = BRAD_OP_NAME_BZ,
        [BRAD_OP_BNZ]  = BRAD_OP_NAME_BNZ,
        [BRAD_OP_JMP]  = BRAD_OP_NAME_JMP,
        [BRAD_OP_CALL] = BRAD_OP_NAME_CALL,
        [BRAD_OP_RET]  = BRAD_OP_NAME_RET,
    };
    return (op <= 0xF) ? names[op] : "???";
}

static inline const char *brad_isa_cop_name(enum brad_isa_cop cop)
{
    static const char *names[] = {
        [BRAD_COP_ADD]  = "C.ADD",
        [BRAD_COP_SUB]  = "C.SUB",
        [BRAD_COP_AND]  = "C.AND",
        [BRAD_COP_OR]   = "C.OR",
        [BRAD_COP_XOR]  = "C.XOR",
        [BRAD_COP_SHL]  = "C.SHL",
        [BRAD_COP_SHR]  = "C.SHR",
        [BRAD_COP_ADDI] = "C.ADDI",
        [BRAD_COP_LDW]  = "C.LDW",
        [BRAD_COP_STW]  = "C.STW",
        [BRAD_COP_BZ]   = "C.BZ",
        [BRAD_COP_BNZ]  = "C.BNZ",
        [BRAD_COP_MV]   = "C.MV",
        [BRAD_COP_NOP]  = "C.NOP",
        [BRAD_COP_JMP]  = "C.JMP",
        [BRAD_COP_CALL] = "C.CALL",
    };
    return (cop <= 0xF) ? names[cop] : "???";
}

/* ─── Exception / Interrupt Vectors ─── */

#define BRAD_EXC_RESET          0x00000000U
#define BRAD_EXC_UNDEF_INSN     0x00000004U
#define BRAD_EXC_PAGE_FAULT     0x00000008U
#define BRAD_EXC_UNALIGNED      0x0000000CU
#define BRAD_EXC_PRIVILEGE      0x00000010U
#define BRAD_EXC_SYSCALL        0x00000014U
#define BRAD_EXC_TIMER          0x00000018U
#define BRAD_EXC_IRQ0           0x00000020U
#define BRAD_EXC_IRQ1           0x00000024U
#define BRAD_EXC_IRQ2           0x00000028U
#define BRAD_EXC_IRQ3           0x0000002CU

/* Exception types for context saving */
#define BRAD_EXC_NONE           0
#define BRAD_EXC_UNDEF          1
#define BRAD_EXC_PAGE_FAULT_T   2
#define BRAD_EXC_UNALIGNED_T    3
#define BRAD_EXC_PRIVILEGE_T    4
#define BRAD_EXC_SYSCALL_T      5
#define BRAD_EXC_TIMER_T        6
#define BRAD_EXC_IRQ_T          7
#define BRAD_EXC_DBG            8

/* ─── MSR / Control Register Indices ─── */

/* V1 MSRs */
#define BRAD_MSR_STATUS         0
#define BRAD_MSR_CAUSE          1
#define BRAD_MSR_EPC            2
#define BRAD_MSR_EAR            3
#define BRAD_MSR_PAGE_BASE      4
#define BRAD_MSR_TICK           5
#define BRAD_MSR_CORE_ID        6
#define BRAD_MSR_CLUSTER_ID     7

/* V2 Power Management MSRs */
#define BRAD_MSR_PM_CTRL        8   /* Power management control */
#define BRAD_MSR_PSTATE         9   /* Current P-state (voltage/freq pair) */
#define BRAD_MSR_PSTATES        10  /* Available P-states bitmap */
#define BRAD_MSR_VOLTAGE        11  /* Core voltage in mV */
#define BRAD_MSR_FREQ           12  /* Core frequency in MHz */
#define BRAD_MSR_ENERGY         13  /* Energy counter (uJ consumed) */
#define BRAD_MSR_PWR_CAP        14  /* Power cap in mW */
#define BRAD_MSR_TEMP           15  /* Die temperature in Celsius */
#define BRAD_MSR_PERF_CNT0      16  /* Performance counter 0 */
#define BRAD_MSR_PERF_CNT1      17  /* Performance counter 1 */
#define BRAD_MSR_PERF_CNT_CTRL  18  /* Performance counter control */
#define BRAD_MSR_L2_PART        19  /* L2 cache partitioning */
#define BRAD_MSR_L3_PART        20  /* L3 cache partitioning */
#define BRAD_MSR_CG_MASK        21  /* Clock-gating unit mask */
#define BRAD_MSR_DBG_CTRL       22  /* Debug control */

#define BRAD_MSR_MAX            31

/* ─── Status Register Flags (MSR_STATUS) ─── */

/* V1 flags */
#define BRAD_STATUS_IE          (1U << 0)
#define BRAD_STATUS_SVC         (1U << 1)
#define BRAD_STATUS_HALTED      (1U << 2)
#define BRAD_STATUS_WFE         (1U << 3)

/* V2 flags */
#define BRAD_STATUS_C           (1U << 4)
#define BRAD_STATUS_EE          (1U << 5)
#define BRAD_STATUS_DBG         (1U << 6)

#define BRAD_STATUS_EXC_PEND    (1U << 8)
#define BRAD_STATUS_PSTATE_SHIFT 16
#define BRAD_STATUS_PSTATE_MASK  0x1F

/* ─── Power States (BRAD_MSR_PM_CTRL) ─── */

#define BRAD_PM_CSTATE_MASK     0xF
#define BRAD_PM_C0              0   /* Active */
#define BRAD_PM_C1              1   /* WFI (clock-gated) */
#define BRAD_PM_C1E             2   /* WFI + voltage reduced */
#define BRAD_PM_C2              3   /* SLEEP (power-gated, cache retained) */
#define BRAD_PM_C2E             4   /* SLEEP (power-gated, cache flushed) */
#define BRAD_PM_C3              5   /* DEEP_SLEEP (full power-down, wake from IRQ) */

#define BRAD_PM_WAKE_IRQ        (1U << 4)
#define BRAD_PM_WAKE_TIMER      (1U << 5)
#define BRAD_PM_AUTO_DEMOTION   (1U << 8)
#define BRAD_PM_AUTO_PROMOTION  (1U << 9)
#define BRAD_PM_VOLTAGE_DROOP   (1U << 12)

/* ─── Cause Register Exception Codes ─── */

#define BRAD_CAUSE_NONE         0
#define BRAD_CAUSE_UNDEF_INSN   1
#define BRAD_CAUSE_PAGE_FAULT   2
#define BRAD_CAUSE_UNALIGNED    3
#define BRAD_CAUSE_PRIVILEGE    4
#define BRAD_CAUSE_SYSCALL      5
#define BRAD_CAUSE_TIMER        6
#define BRAD_CAUSE_IRQ          7

/* ─── Performance Counter Event Selectors ─── */

#define BRAD_PERF_CYCLES        0
#define BRAD_PERF_INSN_RETIRED  1
#define BRAD_PERF_BRANCHES      2
#define BRAD_PERF_BRANCH_MISS   3
#define BRAD_PERF_LD_STALL      4
#define BRAD_PERF_L1_MISS       5
#define BRAD_PERF_L2_MISS       6
#define BRAD_PERF_L3_MISS       7
#define BRAD_PERF_STALL_CYCLES  8
#define BRAD_PERF_IPC           9

#endif /* BRAD_ISA_H */
