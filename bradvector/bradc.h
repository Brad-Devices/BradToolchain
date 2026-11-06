#ifndef BRAD_VECTOR_BRADC_H
#define BRAD_VECTOR_BRADC_H

#include "brad/bvbc.h"
#include <stddef.h>

/* ─── bradc — BradVector assembler ───
 *
 * Assembles BradVector assembly text into an in-memory .bvbc image.
 * Text syntax:
 *
 *   .kernel name                 begin a kernel (switches output)
 *   .end                         end the current kernel
 *   label:                       symbolic label (byte-addressable PC)
 *   OP dst, src1, src2           register operands (S#, V#, P#)
 *   OP dst, src1, imm            immediate in last position
 *   OP@P# ...                    predicated form (branch/select)
 *   ; or # to end of line       comment
 *
 * Branch/call targets resolve to PC-relative 8-bit offsets (units of
 * instructions).  LOAD/STORE encode an 8-bit signed byte offset.
 *
 * Assembly is a reference implementation; it does not (yet) perform
 * scheduling, register allocation, or .brsh AOT emission.
 */

#define BRADC_MAX_ERROR   256
#define BRADC_MAX_SYMBOLS 1024

struct bradc_symbol {
    char     name[64];
    uint32_t addr;
};

struct bradc_error {
    int      line;
    char     msg[BRADC_MAX_ERROR];
};

int bradc_assemble(const char *source, struct bvbc_image *img,
                   struct bradc_error *err);

/* Fill the symbol table from a assembled image (for bradgdb). */
int bradc_symbols(const struct bvbc_image *img,
                  struct bradc_symbol *sym, size_t cap, size_t *n);

#endif /* BRAD_VECTOR_BRADC_H */