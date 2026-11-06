#ifndef BRAD_VECTOR_BRADLIB_H
#define BRAD_VECTOR_BRADLIB_H

#include "brad/bradvector.h"
#include "brad/bvbc.h"
#include "brad/bvrt.h"
#include "bradc.h"
#include "bradgdb.h"
#include <stddef.h>
#include <stdint.h>

/* ─── BradVector Platform API (bradlib) ───
 *
 * The stable host-facing library surface for the BradVector toolchain:
 *
 *     compile (.bvbs source) → pack (.bvbc container) → create session
 *     → launch → run/step → inspect registers → breakpoints
 *
 * Everything is owned and freed by the library; no client-managed
 * buffers are exposed except the .bvbc pack target and register reads.
 * The C implementations behind this (bradc, bvbc, bvrt, bradgdb) are
 * the exact code that also ships to the browser as WebAssembly.
 *
 * Reference model, not a claim of shipped hardware behavior.
 */

struct bradlib_program;   /* compiled source (owns the image) */
struct bradlib_session;   /* device + debugger attached to a program */

/* ─── Compilation ─── */

/* Assemble BradVector assembly text.  Returns a program or NULL; on
 * failure fills `err` (may be NULL) with the line + message. */
struct bradlib_program *bradlib_compile(const char *source,
                                        struct bradc_error *err);
void bradlib_free_program(struct bradlib_program *p);

unsigned       bradlib_kernel_count(const struct bradlib_program *p);
const char    *bradlib_kernel_name(const struct bradlib_program *p,
                                   unsigned idx);
unsigned       bradlib_insn_count(const struct bradlib_program *p);

/* ─── .bvbc container serialization ─── */

size_t bradlib_serialize_size(const struct bradlib_program *p);
size_t bradlib_pack(const struct bradlib_program *p,
                    uint8_t *buf, size_t cap);

/* ─── Disassembly / symbols ─── */

/* Format one instruction ("   3:  IMUL  pred=0 dst=4 src1=3 src2=18 imm=0").
 * Returns bytes written (excluding NUL) or -1. */
int bradlib_disasm(const struct bradlib_program *p, uint32_t pc,
                   char *buf, size_t cap);
int bradlib_symbols(const struct bradlib_program *p,
                    struct bradc_symbol *sym, size_t cap, size_t *n);

/* ─── Runtime session ─── */

/* Create a device with `mem_bytes` of flat SPMP backing and load the
 * program.  The program must outlive the session.  NULL on failure. */
struct bradlib_session *bradlib_create(struct bradlib_program *p,
                                       size_t mem_bytes);
void bradlib_destroy(struct bradlib_session *s);

/* Launch a kernel, copying args into S16..S31 of every lane. */
int bradlib_launch(struct bradlib_session *s, const char *kernel,
                   const uint32_t *args, unsigned nargs);

enum bv_status bradlib_run(struct bradlib_session *s, uint64_t max_insns);
enum bv_status bradlib_step(struct bradlib_session *s);

/* ─── Inspection ─── */

uint32_t bradlib_read_s(struct bradlib_session *s, int lane, unsigned reg);
uint32_t bradlib_read_v(struct bradlib_session *s, int lane,
                        unsigned reg, unsigned lane16);
uint32_t bradlib_read_p(struct bradlib_session *s, int lane, unsigned preg);
uint32_t bradlib_pc(struct bradlib_session *s);
uint64_t bradlib_cycles(struct bradlib_session *s);

/* Host view of the flat SPMP device memory (kernel LOAD/STORE targets). */
void    *bradlib_mem(struct bradlib_session *s);
size_t   bradlib_mem_size(struct bradlib_session *s);

/* ─── Debug ─── */

/* Add+enable a breakpoint at instruction index `pc`; 0 or -1. */
int bradlib_breakpoint(struct bradlib_session *s, uint32_t pc);
/* Run until a breakpoint / halt / trap.  BV_OK on breakpoint hit. */
enum bv_status bradlib_continue(struct bradlib_session *s);

#endif /* BRAD_VECTOR_BRADLIB_H */