#ifndef BRAD_VECTOR_H
#define BRAD_VECTOR_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ─── BradVector ISA — Reference Definitions ───
 *
 * The native instruction set for all BradGfx and BradFx shader cores.
 * [Gen1] — shipping 2026.  See: Brad Silicon/GPU/GPU_ISA.md
 *
 * Two object forms: .bvbc portable bytecode (compiled once) and .brsh
 * native ISA (install-time AOT per tier).  This header declares the
 * shared instruction encoding; the .bvbc container lives in bvbc.h.
 *
 * NOTE: instruction semantics below are the reference (host-side)
 * model implemented by BVRT (src/bradvector/bvrt.c).  They are design
 * intent for hardware, not a claim of shipped silicon behavior.
 */

#define BV_ARCH_NAME            "BradVector"
#define BV_VERSION_MAJOR        1
#define BV_VERSION_MINOR        0
#define BV_ISA_VERSION          0x0100

/* ─── Data Model ─── */

#define BV_WARP_SIZE            32      /* threads per warp (CUDA-compatible) */
#define BV_INSN_SIZE            8       /* 64-bit fixed-size instructions */
#define BV_SCALAR_REGS          256     /* S0–S255, 32-bit each */
#define BV_PRED_REGS            16      /* P0–P15, 32-bit each */
#define BV_VEC_REGS             64      /* V0–V63, 512-bit each */
#define BV_VEC_LANES            16      /* 16 × 32-bit lanes per vector reg */
#define BV_VEC_BITS             512
#define BV_SHARED_MEM_SIZE      (64 * 1024)

/* Special scalar registers */
#define BV_S_ZERO               0       /* reads 0, writes discarded */
#define BV_S_THREAD_ID          1       /* read-only lane index */
#define BV_S_BLOCK_ID           2       /* read-only */
#define BV_S_CLUSTER_ID         3       /* read-only */
#define BV_S_ARG_BASE           16      /* S16–S31 kernel arguments */
#define BV_S_NUM_ARGS           16

/* ─── Instruction Encoding (64-bit) ───
 *
 *  63–56  55–48  47–40  39–32  31–24  23–16  15–8   7–0
 *   OP     PRED   DST    SRC1   SRC2   IMM    FLAGS  MOD
 *
 * FLAGS is instruction-specific.  CMP stores BV_CC_* (the comparison);
 * SEL stores the select predicate number + 1 (0 selects src2), which is
 * separate from PRED, the execution predicate.  Other opcodes leave it 0.
 */

#define BV_OP_SHIFT             56
#define BV_PRED_SHIFT           48
#define BV_DST_SHIFT            40
#define BV_SRC1_SHIFT           32
#define BV_SRC2_SHIFT           24
#define BV_IMM_SHIFT            16
#define BV_FLAGS_SHIFT          8
#define BV_MOD_SHIFT            0

#define BV_INSN_OP(i)           (uint8_t)(((uint64_t)(i) >> BV_OP_SHIFT) & 0xFF)
#define BV_INSN_PRED(i)         (uint8_t)(((uint64_t)(i) >> BV_PRED_SHIFT) & 0xFF)
#define BV_INSN_DST(i)          (uint8_t)(((uint64_t)(i) >> BV_DST_SHIFT) & 0xFF)
#define BV_INSN_SRC1(i)         (uint8_t)(((uint64_t)(i) >> BV_SRC1_SHIFT) & 0xFF)
#define BV_INSN_SRC2(i)         (uint8_t)(((uint64_t)(i) >> BV_SRC2_SHIFT) & 0xFF)
#define BV_INSN_IMM(i)          (int8_t)(((uint64_t)(i) >> BV_IMM_SHIFT) & 0xFF)
#define BV_INSN_FLAGS(i)        (uint8_t)(((uint64_t)(i) >> BV_FLAGS_SHIFT) & 0xFF)
#define BV_INSN_MOD(i)          (uint8_t)(((uint64_t)(i) >> BV_MOD_SHIFT) & 0xFF)

#define BVEN(op, pred, dst, src1, src2, imm, flags, mod) \
    ((((uint64_t)(op)    & 0xFF) << BV_OP_SHIFT)   | \
     (((uint64_t)(pred)  & 0xFF) << BV_PRED_SHIFT) | \
     (((uint64_t)(dst)   & 0xFF) << BV_DST_SHIFT)  | \
     (((uint64_t)(src1)  & 0xFF) << BV_SRC1_SHIFT) | \
     (((uint64_t)(src2)  & 0xFF) << BV_SRC2_SHIFT) | \
     (((uint64_t)((uint8_t)(imm)) & 0xFF) << BV_IMM_SHIFT) | \
     (((uint64_t)(flags) & 0xFF) << BV_FLAGS_SHIFT) | \
     (((uint64_t)(mod)   & 0xFF) << BV_MOD_SHIFT))

struct bv_insn {
    uint64_t raw;
    uint8_t  op;
    uint8_t  pred;
    uint8_t  dst;
    uint8_t  src1;
    uint8_t  src2;
    int8_t   imm;
    uint8_t  flags;
    uint8_t  mod;
};

static inline struct bv_insn bv_decode(uint64_t raw)
{
    struct bv_insn d;
    memset(&d, 0, sizeof(d));
    d.raw  = raw;
    d.op   = BV_INSN_OP(raw);
    d.pred = BV_INSN_PRED(raw);
    d.dst  = BV_INSN_DST(raw);
    d.src1 = BV_INSN_SRC1(raw);
    d.src2 = BV_INSN_SRC2(raw);
    d.imm  = BV_INSN_IMM(raw);
    d.flags = BV_INSN_FLAGS(raw);
    d.mod  = BV_INSN_MOD(raw);
    return d;
}

/* ─── Opcode Classes ─── */

enum bv_opcode {
    /* ALU (float) */
    BV_OP_ADD      = 0x00,
    BV_OP_ADDi     = 0x01,
    BV_OP_SUB      = 0x02,
    BV_OP_MUL      = 0x03,
    BV_OP_MAD      = 0x04,
    BV_OP_DIV      = 0x05,
    BV_OP_SQRT     = 0x06,
    BV_OP_RSQRT    = 0x07,
    BV_OP_MIN      = 0x08,
    BV_OP_MAX      = 0x09,
    BV_OP_ABS      = 0x0A,
    BV_OP_NEG      = 0x0B,
    BV_OP_CMP      = 0x0C,
    BV_OP_SEL      = 0x0D,

    /* Integer */
    BV_OP_IADD     = 0x10,
    BV_OP_ISUB     = 0x11,
    BV_OP_IMUL     = 0x12,
    BV_OP_IDIV     = 0x13,
    BV_OP_IMAD     = 0x14,
    BV_OP_POPC     = 0x15,
    BV_OP_CLZ      = 0x16,
    BV_OP_BFIND    = 0x17,
    BV_OP_SHL      = 0x18,
    BV_OP_SHR      = 0x19,
    BV_OP_AND      = 0x1A,
    BV_OP_OR       = 0x1B,
    BV_OP_XOR      = 0x1C,
    BV_OP_NOT      = 0x1D,

    /* Vector / Matrix */
    BV_OP_VADD     = 0x20,
    BV_OP_VMUL     = 0x21,
    BV_OP_VDOT     = 0x22,
    BV_OP_VCROSS   = 0x23,
    BV_OP_VMAD     = 0x24,
    BV_OP_MMUL     = 0x25,
    BV_OP_MMUL_B   = 0x26,
    BV_OP_VSHUFFLE = 0x27,
    BV_OP_VBROADCAST = 0x28,
    BV_OP_VREDUCE  = 0x29,

    /* Memory */
    BV_OP_LOAD     = 0x30,
    BV_OP_LOAD2    = 0x31,
    BV_OP_LOADV    = 0x32,
    BV_OP_LOAD_DS  = 0x33,
    BV_OP_STORE    = 0x34,
    BV_OP_STORE2   = 0x35,
    BV_OP_STOREV   = 0x36,
    BV_OP_PREFETCH = 0x37,
    BV_OP_ATOMIC_ADD = 0x38,
    BV_OP_ATOMIC_CAS = 0x39,
    BV_OP_ATOMIC_EXCH = 0x3A,
    BV_OP_TEX_SAMPLE  = 0x3B,
    BV_OP_TEX_SAMPLE_3D = 0x3C,
    BV_OP_TEX_GATHER   = 0x3D,

    /* Control Flow */
    BV_OP_BR       = 0x40,
    BV_OP_BR_COND  = 0x41,
    BV_OP_CALL     = 0x42,
    BV_OP_RET      = 0x43,
    BV_OP_BAR      = 0x44,
    BV_OP_BAR_CLUSTER = 0x45,
    BV_OP_EXIT     = 0x46,
    BV_OP_TRAP     = 0x47,

    /* Ray Tracing (BradRT) */
    BV_OP_RT_TRACE    = 0x50,
    BV_OP_RT_INTERSECT = 0x51,
    BV_OP_RT_BVH_WALK = 0x52,
    BV_OP_RT_ANY_HIT  = 0x53,
    BV_OP_RT_CLOSEST_HIT = 0x54,
    BV_OP_RT_MISS     = 0x55,
    BV_OP_RT_NEARF_EVAL = 0x56,
    BV_OP_RT_NEARF_TRAIN = 0x57,

    /* Neuro-Stream (NSU) */
    BV_OP_NS_CONV2D     = 0x60,
    BV_OP_NS_CONV2D_5   = 0x61,
    BV_OP_NS_RELU       = 0x62,
    BV_OP_NS_GELU       = 0x63,
    BV_OP_NS_SILU       = 0x64,
    BV_OP_NS_MAXPOOL    = 0x65,
    BV_OP_NS_UPSAMPLE   = 0x66,
    BV_OP_NS_ATTENTION  = 0x67,
    BV_OP_NS_EXPERT_SELECT = 0x68,
    BV_OP_NS_MOE_FUSE   = 0x69,

    /* Special Function */
    BV_OP_RCP      = 0xF0,
    BV_OP_SIN      = 0xF1,
    BV_OP_COS      = 0xF2,
    BV_OP_EXP2     = 0xF3,
    BV_OP_LOG2     = 0xF4,
    BV_OP_RAND     = 0xF5,
    BV_OP_LANE_ID  = 0xF6,
    BV_OP_WARP_SZ  = 0xF7,
    BV_OP_CLOCK    = 0xF8,
};

/* CMP/VREDUCE modifiers carried in FLAGS */
enum bv_cmp_cond {
    BV_CC_EQ = 0x0,
    BV_CC_NE = 0x1,
    BV_CC_LT = 0x2,
    BV_CC_GT = 0x3,
    BV_CC_LE = 0x4,
    BV_CC_GE = 0x5,
};

enum bv_reduce_op {
    BV_RED_SUM = 0x00,
    BV_RED_MAX = 0x01,
    BV_RED_MIN = 0x02,
};

/* ─── Opcode Name Lookup ─── */

static inline const char *bv_opcode_name(unsigned op)
{
    static const char *names[256] = {
        [BV_OP_ADD] = "ADD", [BV_OP_ADDi] = "ADDi", [BV_OP_SUB] = "SUB",
        [BV_OP_MUL] = "MUL", [BV_OP_MAD] = "MAD",   [BV_OP_DIV] = "DIV",
        [BV_OP_SQRT] = "SQRT", [BV_OP_RSQRT] = "RSQRT",
        [BV_OP_MIN] = "MIN", [BV_OP_MAX] = "MAX",   [BV_OP_ABS] = "ABS",
        [BV_OP_NEG] = "NEG", [BV_OP_CMP] = "CMP",   [BV_OP_SEL] = "SEL",
        [BV_OP_IADD] = "IADD", [BV_OP_ISUB] = "ISUB", [BV_OP_IMUL] = "IMUL",
        [BV_OP_IDIV] = "IDIV", [BV_OP_IMAD] = "IMAD",
        [BV_OP_POPC] = "POPC", [BV_OP_CLZ] = "CLZ", [BV_OP_BFIND] = "BFIND",
        [BV_OP_SHL] = "SHL", [BV_OP_SHR] = "SHR",   [BV_OP_AND] = "AND",
        [BV_OP_OR] = "OR",   [BV_OP_XOR] = "XOR",   [BV_OP_NOT] = "NOT",
        [BV_OP_VADD] = "VADD", [BV_OP_VMUL] = "VMUL", [BV_OP_VDOT] = "VDOT",
        [BV_OP_VCROSS] = "VCROSS", [BV_OP_VMAD] = "VMAD",
        [BV_OP_MMUL] = "MMUL", [BV_OP_MMUL_B] = "MMUL_B",
        [BV_OP_VSHUFFLE] = "VSHUFFLE", [BV_OP_VBROADCAST] = "VBROADCAST",
        [BV_OP_VREDUCE] = "VREDUCE",
        [BV_OP_LOAD] = "LOAD", [BV_OP_LOAD2] = "LOAD2",
        [BV_OP_LOADV] = "LOADV", [BV_OP_LOAD_DS] = "LOAD_DS",
        [BV_OP_STORE] = "STORE", [BV_OP_STORE2] = "STORE2",
        [BV_OP_STOREV] = "STOREV", [BV_OP_PREFETCH] = "PREFETCH",
        [BV_OP_ATOMIC_ADD] = "ATOMIC_ADD", [BV_OP_ATOMIC_CAS] = "ATOMIC_CAS",
        [BV_OP_ATOMIC_EXCH] = "ATOMIC_EXCH",
        [BV_OP_TEX_SAMPLE] = "TEX_SAMPLE",
        [BV_OP_TEX_SAMPLE_3D] = "TEX_SAMPLE_3D", [BV_OP_TEX_GATHER] = "TEX_GATHER",
        [BV_OP_BR] = "BR", [BV_OP_BR_COND] = "BR_COND",
        [BV_OP_CALL] = "CALL", [BV_OP_RET] = "RET",
        [BV_OP_BAR] = "BAR", [BV_OP_BAR_CLUSTER] = "BAR_CLUSTER",
        [BV_OP_EXIT] = "EXIT", [BV_OP_TRAP] = "TRAP",
        [BV_OP_RT_TRACE] = "RT_TRACE", [BV_OP_RT_INTERSECT] = "RT_INTERSECT",
        [BV_OP_RT_BVH_WALK] = "RT_BVH_WALK", [BV_OP_RT_ANY_HIT] = "RT_ANY_HIT",
        [BV_OP_RT_CLOSEST_HIT] = "RT_CLOSEST_HIT", [BV_OP_RT_MISS] = "RT_MISS",
        [BV_OP_RT_NEARF_EVAL] = "RT_NEARF_EVAL",
        [BV_OP_RT_NEARF_TRAIN] = "RT_NEARF_TRAIN",
        [BV_OP_NS_CONV2D] = "NS_CONV2D", [BV_OP_NS_CONV2D_5] = "NS_CONV2D_5",
        [BV_OP_NS_RELU] = "NS_RELU", [BV_OP_NS_GELU] = "NS_GELU",
        [BV_OP_NS_SILU] = "NS_SILU", [BV_OP_NS_MAXPOOL] = "NS_MAXPOOL",
        [BV_OP_NS_UPSAMPLE] = "NS_UPSAMPLE", [BV_OP_NS_ATTENTION] = "NS_ATTENTION",
        [BV_OP_NS_EXPERT_SELECT] = "NS_EXPERT_SELECT",
        [BV_OP_NS_MOE_FUSE] = "NS_MOE_FUSE",
        [BV_OP_RCP] = "RCP", [BV_OP_SIN] = "SIN", [BV_OP_COS] = "COS",
        [BV_OP_EXP2] = "EXP2", [BV_OP_LOG2] = "LOG2",
        [BV_OP_RAND] = "RAND", [BV_OP_LANE_ID] = "LANE_ID",
        [BV_OP_WARP_SZ] = "WARP_SZ", [BV_OP_CLOCK] = "CLOCK",
    };
    return names[op & 0xFF] ? names[op & 0xFF] : "???";
}

/* ─── Result / Trap status ─── */

enum bv_status {
    BV_OK       = 0,
    BV_TRAP     = 1,   /* TRAP or unimplemented opcode executed */
    BV_TIMEOUT  = 2,   /* instruction budget exhausted */
    BV_HALT     = 3,   /* all lanes exited (normal completion) */
    BV_OOB      = 4,   /* out-of-bounds memory access */
};

#endif /* BRAD_VECTOR_H */