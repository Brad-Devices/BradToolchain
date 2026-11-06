#ifndef BRAD_VECTOR_BRADGDB_H
#define BRAD_VECTOR_BRADGDB_H

#include "brad/bvrt.h"
#include <stdint.h>
#include <stddef.h>

/* ─── bradgdb — BradVector debugger (reference) ───
 *
 * Host-side symbol-aware debugger on top of BVRT.  Supports software
 * breakpoints, single-step, kernel launch, and register inspection.
 * Reference tool; PTI/hardware-debug integration is [Gen1] future work.
 */

#define BRADGDB_MAX_BREAK  64

struct bradgdb_break {
    uint32_t pc;      /* instruction index in the loaded kernel */
    int      enabled;
    int      hit;
};

struct bradgdb {
    bvrt_device     *dev;
    struct bradgdb_break breaks[BRADGDB_MAX_BREAK];
    int            nbreaks;
    uint64_t       steps;      /* instructions executed since attach */
    int            tracing;    /* echo each instruction when stepping */
};

/* Attach the debugger to a device.  Returns 0 or -1. */
int bradgdb_attach(struct bradgdb *g, bvrt_device *dev);

/* Add a breakpoint at an instruction index. Returns id (>=0) or -1. */
int bradgdb_add_break(struct bradgdb *g, uint32_t pc);

/* Disable/enable by id. */
void bradgdb_enable_break(struct bradgdb *g, int id, int enabled);

/* Step a single instruction.  Returns bvrt_step status. */
enum bv_status bradgdb_step(struct bradgdb *g);

/* Run until a breakpoint is hit, the warp halts, or a trap.  When a
 * breakpoint is hit, returns BV_OK (the warp PC is parked on it). */
enum bv_status bradgdb_continue(struct bradgdb *g);

/* Scalar register read for a lane.  Invalid lanes read 0. */
uint32_t bradgdb_read_s(struct bradgdb *g, int lane, unsigned reg);
/* Vector lane read. */
uint32_t bradgdb_read_v(struct bradgdb *g, int lane, unsigned reg,
                        unsigned lane16);
/* Predicate read. */
uint32_t bradgdb_read_p(struct bradgdb *g, int lane, unsigned preg);
/* Current warp PC. */
uint32_t bradgdb_pc(struct bradgdb *g);

#endif /* BRAD_VECTOR_BRADGDB_H */