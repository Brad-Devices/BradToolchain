#include "brad/bvrt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ─── BVRT reference interpreter ───
 * Single-warp SIMT model with a shared warp PC.  Predication via the
 * PRED field gating the active mask; scalar/vector/predicate register
 * files per the BradVector ISA.  Flat host memory models the SPMP.
 *
 * Divergence is modeled coarsely: the warp has ONE PC, so control
 * flow is warp-uniform (predication handles the divergent cases).
 * Memory addresses are per-lane (each lane computes its own base),
 * which supports the classic scatter/gather kernel patterns.
 */

struct bvrt_device {
    struct bvbc_image  img;
    struct bvrt_warp   warp;
    uint8_t           *mem;
    size_t             mem_size;
    uint64_t           insns;
    enum bv_status     status;
    const uint64_t    *code;    /* resolved instruction stream */
    struct bvrt_warp  *wp;
};

static uint32_t f_to_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    u_to_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static inline int lane_active(const struct bvrt_warp *w, int lane)
{
    return (w->active >> lane) & 1u;
}

static inline int pred_ok(const struct bvrt_device *dev, int lane,
                          uint8_t pred)
{
    if (pred == 0)
        return 1;
    return dev->wp->p[lane][pred - 1] != 0;
}

static enum bv_status bvrt_trap(bvrt_device *dev, struct bvrt_warp *w,
                                const char *why)
{
    (void)why;
    dev->status = BV_TRAP;
    (void)w;
    return BV_TRAP;
}

/* Predicated write to a scalar register (S0 is zero-register). */
static void wr_s(struct bvrt_device *dev, int lane, uint8_t reg, uint32_t v)
{
    if (reg != BV_S_ZERO)
        dev->wp->s[lane][reg] = v;
}

static enum bv_status execute_insn(bvrt_device *dev, struct bvrt_warp *w,
                                   struct bv_insn in)
{
    uint32_t step = 1;
    (void)step;
    int exec_branched = 0;

    switch (in.op) {
    /* ── ALU (float) ── */
    case BV_OP_ADD:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(u_to_f(w->s[l][in.src1]) +
                                            u_to_f(w->s[l][in.src2])));
        break;
    case BV_OP_ADDi:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(u_to_f(w->s[l][in.src1]) + in.imm));
        break;
    case BV_OP_SUB:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(u_to_f(w->s[l][in.src1]) -
                                            u_to_f(w->s[l][in.src2])));
        break;
    case BV_OP_MUL:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(u_to_f(w->s[l][in.src1]) *
                                            u_to_f(w->s[l][in.src2])));
        break;
    case BV_OP_MAD:   /* dst = a*b + dst */
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst,
                     f_to_u(u_to_f(w->s[l][in.src1]) *
                            u_to_f(w->s[l][in.src2]) +
                            u_to_f(w->s[l][in.dst])));
        break;
    case BV_OP_DIV:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                float a = u_to_f(w->s[l][in.src1]);
                float b = u_to_f(w->s[l][in.src2]);
                wr_s(dev, l, in.dst,
                     b == 0.0f ? UINT32_MAX : f_to_u(a / b));
            }
        break;
    case BV_OP_SQRT:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(sqrtf(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_RSQRT:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                float x = u_to_f(w->s[l][in.src1]);
                wr_s(dev, l, in.dst, f_to_u(1.0f / sqrtf(x)));
            }
        break;
    case BV_OP_RCP:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(1.0f / u_to_f(w->s[l][in.src1])));
        break;
    case BV_OP_SIN:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(sinf(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_COS:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(cosf(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_EXP2:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(exp2f(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_LOG2:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(log2f(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_MIN:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                float a = u_to_f(w->s[l][in.src1]);
                float b = u_to_f(w->s[l][in.src2]);
                wr_s(dev, l, in.dst, f_to_u(a < b ? a : b));
            }
        break;
    case BV_OP_MAX:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                float a = u_to_f(w->s[l][in.src1]);
                float b = u_to_f(w->s[l][in.src2]);
                wr_s(dev, l, in.dst, f_to_u(a > b ? a : b));
            }
        break;
    case BV_OP_ABS:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(fabsf(u_to_f(w->s[l][in.src1]))));
        break;
    case BV_OP_NEG:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, f_to_u(-u_to_f(w->s[l][in.src1])));
        break;
    case BV_OP_SEL:   /* dst = FLAGS-predicate ? src1 : src2 */
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                /* FLAGS names the select condition; PRED still gates the
                 * write.  Using PRED for both made SEL a no-op selector. */
                int c = (in.flags != 0) &&
                        (w->p[l][in.flags - 1] != 0);
                wr_s(dev, l, in.dst, c ? w->s[l][in.src1] : w->s[l][in.src2]);
            }
        break;
    case BV_OP_CMP:   /* dst predicate = (src1 op src2), op in FLAGS */
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                uint32_t a = w->s[l][in.src1];
                uint32_t b = w->s[l][in.src2];
                int r;
                switch (in.flags) {
                    case BV_CC_EQ: r = (a == b); break;
                    case BV_CC_NE: r = (a != b); break;
                    case BV_CC_LT: r = (a <  b); break;
                    case BV_CC_GT: r = (a >  b); break;
                    case BV_CC_LE: r = (a <= b); break;
                    default:       r = (a >= b); break;
                }
                w->p[l][in.dst] = r ? 1u : 0u;
            }
        break;

    /* ── Integer ── */
    case BV_OP_IADD:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] + w->s[l][in.src2]);
        break;
    case BV_OP_ISUB:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] - w->s[l][in.src2]);
        break;
    case BV_OP_IMUL:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] * w->s[l][in.src2]);
        break;
    case BV_OP_IDIV:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                uint32_t b = w->s[l][in.src2];
                wr_s(dev, l, in.dst,
                     b == 0 ? UINT32_MAX : w->s[l][in.src1] / b);
            }
        break;
    case BV_OP_IMAD:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst,
                     w->s[l][in.src1] * w->s[l][in.src2] + w->s[l][in.dst]);
        break;
    case BV_OP_POPC:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                uint32_t v = w->s[l][in.src1];
                int c = 0;
                for (int bit = 0; bit < 32; bit++) c += (v >> bit) & 1;
                wr_s(dev, l, in.dst, (uint32_t)c);
            }
        break;
    case BV_OP_CLZ:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                uint32_t v = w->s[l][in.src1];
                int c = 0;
                while (c < 32 && !(v & (1u << 31))) { v <<= 1; c++; }
                wr_s(dev, l, in.dst, (uint32_t)c);
            }
        break;
    case BV_OP_BFIND:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                uint32_t v = w->s[l][in.src1];
                int idx = -1;
                for (int bit = 0; bit < 32; bit++)
                    if (v & (1u << bit)) idx = bit;
                wr_s(dev, l, in.dst, (uint32_t)idx);
            }
        break;
    case BV_OP_SHL:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst,
                     w->s[l][in.src1] << (w->s[l][in.src2] & 31));
        break;
    case BV_OP_SHR:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst,
                     w->s[l][in.src1] >> (w->s[l][in.src2] & 31));
        break;
    case BV_OP_AND:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] & w->s[l][in.src2]);
        break;
    case BV_OP_OR:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] | w->s[l][in.src2]);
        break;
    case BV_OP_XOR:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->s[l][in.src1] ^ w->s[l][in.src2]);
        break;
    case BV_OP_NOT:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, ~w->s[l][in.src1]);
        break;

    /* ── Vector ──
   Vector ops use the dedicated per-lane vector register file
   v[lane][reg][lane16]; VBROADCAST fans a scalar out. */

#define VR(dev, l, reg, i)   ((dev)->wp->v[(l)][(reg)][(i)])
#define VRD(dev, l, reg, i)  ((dev)->wp->v[(l)][(reg)][(i)])
    case BV_OP_VADD:
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            for (int li = 0; li < BV_VEC_LANES; li++)
                VRD(dev, l, in.dst, li) =
                    f_to_u(u_to_f(VR(dev, l, in.src1, li)) +
                           u_to_f(VR(dev, l, in.src2, li)));
        }
        break;
    case BV_OP_VMUL:
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            for (int li = 0; li < BV_VEC_LANES; li++)
                VRD(dev, l, in.dst, li) =
                    f_to_u(u_to_f(VR(dev, l, in.src1, li)) *
                           u_to_f(VR(dev, l, in.src2, li)));
        }
        break;
    case BV_OP_VMAD:
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            for (int li = 0; li < BV_VEC_LANES; li++)
                VRD(dev, l, in.dst, li) =
                    f_to_u(u_to_f(VR(dev, l, in.src1, li)) *
                           u_to_f(VR(dev, l, in.src2, li)) +
                           u_to_f(VR(dev, l, in.dst, li)));
        }
        break;
    case BV_OP_VDOT:   /* dst[0] = a·b */
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            float acc = 0.0f;
            for (int li = 0; li < BV_VEC_LANES; li++)
                acc += u_to_f(VR(dev, l, in.src1, li)) *
                       u_to_f(VR(dev, l, in.src2, li));
            for (int li = 0; li < BV_VEC_LANES; li++)
                VRD(dev, l, in.dst, li) = li == 0 ? f_to_u(acc) : 0;
        }
        break;
    case BV_OP_VBROADCAST:
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            for (int li = 0; li < BV_VEC_LANES; li++)
                VRD(dev, l, in.dst, li) = w->s[l][in.src1];
        }
        break;
    case BV_OP_VREDUCE:   /* dst[0] = sum/max/min over lanes; FLAGS selects */
        for (int l = 0; l < BV_WARP_SIZE; l++) {
            if (!(lane_active(w, l) && pred_ok(dev, l, in.pred))) continue;
            switch (in.flags) {
                case BV_RED_MAX: {
                    uint32_t m = 0;
                    for (int li = 1; li < BV_VEC_LANES; li++)
                        if (VR(dev, l, in.dst, li) > m) m = VR(dev, l, in.dst, li);
                    VRD(dev, l, in.dst, 0) = m;
                    break;
                }
                case BV_RED_MIN: {
                    uint32_t m = UINT32_MAX;
                    for (int li = 0; li < BV_VEC_LANES; li++)
                        if (VR(dev, l, in.dst, li) < m) m = VR(dev, l, in.dst, li);
                    VRD(dev, l, in.dst, 0) = m;
                    break;
                }
                default: {  /* SUM */
                    uint32_t acc = 0;
                    for (int li = 0; li < BV_VEC_LANES; li++)
                        acc = f_to_u(u_to_f(acc) +
                                     u_to_f(VR(dev, l, in.dst, li)));
                    VRD(dev, l, in.dst, 0) = acc;
                    break;
                }
            }
        }
        break;
    case BV_OP_VCROSS:
    case BV_OP_MMUL:
    case BV_OP_MMUL_B:
    case BV_OP_VSHUFFLE:
        return bvrt_trap(dev, w, "vector op not simulated (VCROSS/MMUL/VSHUFFLE)");

    /* ── Memory (SPMP flat) ── */
    case BV_OP_LOAD:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                size_t addr = (size_t)(int32_t)w->s[l][in.src1] + in.imm;
                if (addr + 4 > dev->mem_size) return bvrt_trap(dev, w, "LOAD oob");
                uint32_t v;
                memcpy(&v, dev->mem + addr, 4);
                wr_s(dev, l, in.dst, v);
            }
        break;
    case BV_OP_LOADV:   /* load 64 bytes into vector reg */
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                size_t addr = (size_t)(int32_t)w->s[l][in.src1] + in.imm;
                if (addr + BV_VEC_LANES * 4 > dev->mem_size)
                    return bvrt_trap(dev, w, "LOADV oob");
                for (int li = 0; li < BV_VEC_LANES; li++) {
                    uint32_t v;
                    memcpy(&v, dev->mem + addr + li * 4, 4);
                    wr_s(dev, l, in.dst * BV_VEC_LANES + li, v);
                }
            }
        break;
    case BV_OP_STORE:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                size_t addr = (size_t)(int32_t)w->s[l][in.src1] + in.imm;
                if (addr + 4 > dev->mem_size) return bvrt_trap(dev, w, "STORE oob");
                memcpy(dev->mem + addr, &w->s[l][in.dst], 4);
            }
        break;
    case BV_OP_STOREV:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                size_t addr = (size_t)(int32_t)w->s[l][in.src1] + in.imm;
                if (addr + BV_VEC_LANES * 4 > dev->mem_size)
                    return bvrt_trap(dev, w, "STOREV oob");
                for (int li = 0; li < BV_VEC_LANES; li++) {
                    uint32_t v = VR(dev, l, in.dst, li);
                    memcpy(dev->mem + addr + li * 4, &v, 4);
                }
            }
        break;
    case BV_OP_ATOMIC_ADD:   /* serialize lanes; dst gets old value */
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                size_t addr = (size_t)(int32_t)w->s[l][in.src1] + in.imm;
                if (addr + 4 > dev->mem_size) return bvrt_trap(dev, w, "ATOMIC oob");
                uint32_t old;
                memcpy(&old, dev->mem + addr, 4);
                uint32_t nv = old + w->s[l][in.src2];
                memcpy(dev->mem + addr, &nv, 4);
                wr_s(dev, l, in.dst, old);
            }
        break;

    /* ── Control ── */
    case BV_OP_BR:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                w->pc = (uint32_t)((int32_t)w->pc + 1 + in.imm);
                exec_branched = 1;
                break;
            }
        break;
    case BV_OP_BR_COND:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                w->pc = (uint32_t)((int32_t)w->pc + 1 + in.imm);
                exec_branched = 1;
                break;
            }
        break;
    case BV_OP_CALL:
    case BV_OP_RET: {
        if (in.op == BV_OP_CALL) {
            for (int l = 0; l < BV_WARP_SIZE; l++)
                if (lane_active(w, l) && pred_ok(dev, l, in.pred)) {
                    if (w->retsp[l] >= BVRT_MAX_RET)
                        return bvrt_trap(dev, w, "call depth exceeded");
                    w->ret[l][w->retsp[l]++] = w->pc + 1;
                }
            w->pc = (uint32_t)((int32_t)w->pc + 1 + in.imm);
            exec_branched = 1;
        } else {
            for (int l = 0; l < BV_WARP_SIZE; l++)
                if (lane_active(w, l)) {
                    if (w->retsp[l] == 0) return bvrt_trap(dev, w, "RET underflow");
                    w->pc = w->ret[l][--w->retsp[l]];
                }
            exec_branched = 1;
        }
        break;
    }
    case BV_OP_BAR:   /* single-warp: no-op barrier */
        break;
    case BV_OP_BAR_CLUSTER:
        break;
    case BV_OP_EXIT:
        w->active = 0;
        break;
    case BV_OP_TRAP:
        return bvrt_trap(dev, w, "TRAP");

    /* ── Special ── */
    case BV_OP_LANE_ID:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, (uint32_t)l);
        break;
    case BV_OP_WARP_SZ:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, BV_WARP_SIZE);
        break;
    case BV_OP_CLOCK:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst, w->cycles);
        break;
    case BV_OP_RAND:
        for (int l = 0; l < BV_WARP_SIZE; l++)
            if (lane_active(w, l) && pred_ok(dev, l, in.pred))
                wr_s(dev, l, in.dst,
                     (uint32_t)rand() ^ (uint32_t)(w->pc * 2654435761u));
        break;

    /* unimplemented classes -> trap */
    case BV_OP_LOAD2:
    case BV_OP_LOAD_DS:
    case BV_OP_STORE2:
    case BV_OP_PREFETCH:
    case BV_OP_ATOMIC_CAS:
    case BV_OP_ATOMIC_EXCH:
    case BV_OP_TEX_SAMPLE:
    case BV_OP_TEX_SAMPLE_3D:
    case BV_OP_TEX_GATHER:
    case BV_OP_RT_TRACE:
    case BV_OP_RT_INTERSECT:
    case BV_OP_RT_BVH_WALK:
    case BV_OP_RT_ANY_HIT:
    case BV_OP_RT_CLOSEST_HIT:
    case BV_OP_RT_MISS:
    case BV_OP_RT_NEARF_EVAL:
    case BV_OP_RT_NEARF_TRAIN:
    case BV_OP_NS_CONV2D:
    case BV_OP_NS_CONV2D_5:
    case BV_OP_NS_RELU:
    case BV_OP_NS_GELU:
    case BV_OP_NS_SILU:
    case BV_OP_NS_MAXPOOL:
    case BV_OP_NS_UPSAMPLE:
    case BV_OP_NS_ATTENTION:
    case BV_OP_NS_EXPERT_SELECT:
    case BV_OP_NS_MOE_FUSE:
    default:
        return bvrt_trap(dev, w, "unimplemented opcode");
    }

    /* Sequential-path instructions fall through here: advance the PC
     * after the switch.  Control-flow cases (BR, BR_COND, CALL, RET,
     * TRAP) return or set the PC themselves. */
    if (exec_branched)
        return BV_OK;
    w->pc += 1;
    w->cycles++;
    return BV_OK;
}

enum bv_status bvrt_step(bvrt_device *dev)
{
    struct bvrt_warp *w = &dev->warp;
    if (w->active == 0)
        return BV_HALT;
    if ((int32_t)w->pc < 0)
        return bvrt_trap(dev, w, "PC underflow");
    if (w->pc >= dev->img.ninsn)
        return bvrt_trap(dev, w, "PC out of range");

    struct bv_insn in = bv_decode(dev->code[w->pc]);
    dev->insns++;
    return execute_insn(dev, w, in);
}

enum bv_status bvrt_run(bvrt_device *dev, uint64_t max_insns)
{
    while (dev->warp.active != 0 && dev->insns < max_insns) {
        enum bv_status st = bvrt_step(dev);
        if (st != BV_OK)
            return st;
    }
    if (dev->warp.active == 0)
        return BV_HALT;
    return BV_TIMEOUT;
}

/* ─── Device management ─── */

bvrt_device *bvrt_create(size_t mem_bytes)
{
    if (mem_bytes == 0)
        mem_bytes = 1;
    if (mem_bytes > BVRT_MAX_MEM)
        mem_bytes = BVRT_MAX_MEM;

    bvrt_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;
    dev->mem = malloc(mem_bytes);
    if (!dev->mem) {
        free(dev);
        return NULL;
    }
    dev->mem_size = mem_bytes;
    dev->wp = &dev->warp;
    dev->img.version = BVBC_VERSION;
    dev->status = BV_OK;
    return dev;
}

void bvrt_destroy(bvrt_device *dev)
{
    if (!dev)
        return;
    free(dev->mem);
    free(dev);
}

int bvrt_load(bvrt_device *dev, const struct bvbc_image *img)
{
    if (!dev || !img)
        return -1;
    dev->img = *img;
    dev->code = img->insns;
    return 0;
}

void bvrt_reset(bvrt_device *dev)
{
    memset(&dev->warp, 0, sizeof(dev->warp));
    dev->insns = 0;
    dev->status = BV_OK;
}

void *bvrt_mem(bvrt_device *dev)
{
    return dev ? dev->mem : NULL;
}

int bvrt_launch(bvrt_device *dev, const char *kernel,
                const uint32_t *args, unsigned nargs)
{
    int ki = bvbc_find_kernel(&dev->img, kernel);
    if (ki < 0)
        return -1;
    const struct bvbc_kernel *k = &dev->img.kernels[ki];

    bvrt_reset(dev);
    dev->warp.pc = k->entry;
    dev->warp.active = 0xFFFFFFFFu;

    if (nargs > BV_S_NUM_ARGS)
        nargs = BV_S_NUM_ARGS;
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        for (unsigned i = 0; i < nargs; i++)
            dev->warp.s[l][BV_S_ARG_BASE + i] = args[i];
        /* thread id visible in S1 */
        dev->warp.s[l][BV_S_THREAD_ID] = (uint32_t)l;
    }
    return 0;
}

struct bvbc_image *bvrt_image(bvrt_device *dev) { return &dev->img; }
struct bvrt_warp *bvrt_warp(bvrt_device *dev)  { return &dev->warp; }