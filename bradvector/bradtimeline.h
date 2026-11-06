#ifndef BRAD_VECTOR_TIMELINE_H
#define BRAD_VECTOR_TIMELINE_H

#include "brad/bvrt.h"
#include <stdint.h>
#include <stddef.h>

/* ─── BradTimeline — BradVector execution profiler (reference) ───
 *
 * Wraps BVRT stepping and records a per-instruction timeline trace with
 * cumulative retire counts and active-lane counts, then aggregates an
 * opcode histogram.  The trace record is a timeline; a CSV renderer is
 * provided for host inspection.
 *
 * Reference tool — not a hardware performance claim.
 */

#define BVTL_MAX_EVENTS   65536
#define BVTL_MAX_KERNELS  64

struct bvtl_event {
    uint64_t  cycle;         /* cumulative instruction count */
    uint32_t  pc;
    uint8_t   op;
    uint8_t   active_lanes;  /* lanes active before this instruction */
};

struct bvtl_kernel_stat {
    char        name[64];
    uint64_t    insns;
    uint64_t    lanes_executed;   /* sum of active-lane counts */
};

struct bvtl {
    bvrt_device   *dev;            /* device being traced */
    struct bvtl_event   events[BVTL_MAX_EVENTS];
    size_t              nevents;
    struct bvtl_kernel_stat kern[BVTL_MAX_KERNELS];
    size_t              nkern;
    uint64_t            insns;
    uint64_t            traps;
    uint64_t            op_hist[256];   /* per-opcode retire histogram */
};

/* Create a tracer attached to a device (may be NULL until attached). */
struct bvtl *bvtl_create(bvrt_device *dev);
void         bvtl_destroy(struct bvtl *tl);

/* Capture the next executed instruction.  Returns bvrt_step status. */
enum bv_status bvtl_step(struct bvtl *tl);

/* Run the device to completion/trap with capture.  Returns bv_status. */
enum bv_status bvtl_run(struct bvtl *tl, uint64_t max_insns);

/* Print a summary report to stdout. */
void bvtl_summary(struct bvtl *tl);

/* Write the trace as CSV to `path`. Returns 0 or -1. */
int bvtl_export_csv(struct bvtl *tl, const char *path);

#endif /* BRAD_VECTOR_TIMELINE_H */