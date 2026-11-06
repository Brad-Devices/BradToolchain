#ifndef BRAD_VECTOR_BVRT_H
#define BRAD_VECTOR_BVRT_H

#include "brad/bradvector.h"
#include "brad/bvbc.h"
#include <stdint.h>
#include <stddef.h>

/* ─── BVRT — BradVector Runtime (reference implementation) ───
 *
 * Host-side SIMT interpreter for .bvbc kernels.  Executes a single warp
 * of 32 threads, each with its own scalar/predicate register file and
 * a shared vector register file (per-warp), plus a flat host memory
 * ("SPMP").  Instruction semantics follow GPU_ISA.md and bradvector.h.
 *
 * Reference model, not a claim of hardware behavior.
 */

#define BVRT_MAX_MEM   (64 * 1024 * 1024)
#define BVRT_MAX_RET   16

struct bvrt_warp {
    uint32_t pc;

    /* active lanes: bit i set = lane i executes */
    uint32_t active;

    /* scalar registers, per lane: s[lane][reg] */
    uint32_t s[BV_WARP_SIZE][BV_SCALAR_REGS];

    /* vector registers, per lane: v[lane][reg][lane16] */
    uint32_t v[BV_WARP_SIZE][BV_VEC_REGS][BV_VEC_LANES];

    /* predicates, per lane */
    uint32_t p[BV_WARP_SIZE][BV_PRED_REGS];

    /* return stack, per lane */
    uint32_t ret[BV_WARP_SIZE][BVRT_MAX_RET];
    uint8_t  retsp[BV_WARP_SIZE];

    uint32_t cycles;
};

typedef struct bvrt_device bvrt_device;

/* Create a device with a backing memory of `mem_bytes` (clamped to
 * BVRT_MAX_MEM).  Returns NULL on allocation failure. */
bvrt_device *bvrt_create(size_t mem_bytes);

/* Destroy a device. */
void bvrt_destroy(bvrt_device *dev);

/* Load a .bvbc image into the device.  The image must remain valid
 * while the device uses it (instructions are referenced, not copied). */
int bvrt_load(bvrt_device *dev, const struct bvbc_image *img);

/* Launch the named kernel.  Copies up to BV_S_NUM_ARGS words from args
 * into S16..S31 of every lane.  Returns 0 or -1 (unknown kernel). */
int bvrt_launch(bvrt_device *dev, const char *kernel,
                const uint32_t *args, unsigned nargs);

/* Run until all lanes exit or a trap/max-steps occurs.
 * Returns enum bv_status.  On BV_TRAP and BV_TIMEOUT the warp PC is
 * left at the offending instruction. */
enum bv_status bvrt_run(bvrt_device *dev, uint64_t max_insns);

/* Execute exactly one instruction (used by bradgdb single-step).
 * Returns enum bv_status without stopping at lane exit. */
enum bv_status bvrt_step(bvrt_device *dev);

/* Accessors for debug tooling. */
struct bvbc_image *bvrt_image(bvrt_device *dev);
struct bvrt_warp *bvrt_warp(bvrt_device *dev);

/* Flat SPMP device memory access (host read/write).  Returns the base
 * pointer and its size; NULL when the device is invalid.  Data written
 * here is visible to kernel LOAD/STORE instructions. */
void   *bvrt_mem(bvrt_device *dev);

/* Reset per-warp state for a fresh launch. */
void bvrt_reset(bvrt_device *dev);

#endif /* BRAD_VECTOR_BVRT_H */