#ifndef BRAD_BVBC_H
#define BRAD_BVBC_H

#include "bradvector.h"
#include <stdint.h>
#include <stddef.h>

/* ─── .bvbc Portable Bytecode Container ───
 *
 * Compiled once from BradVector assembly (bradc), portable across all
 * BradGfx/BradFx tiers.  Install-time AOT converts a .bvbc image to the
 * tier-native .brsh stream (see BRAD_COMPUTE_ARCHITECTURE.md §6).
 *
* Container layout (little-endian):
 *
 *   offset  size  field
 *   0        8    magic    "BVBC0001" (uint64 LE)
 *   8        4    version  BVBC_VERSION  (0x00000001)
 *   12       4    flags     reserved, must be 0
 *   16       4    nkernels
 *   20       4    nconst    constant-data blob size in bytes
 *   24       4    ninsn     instruction count (8-byte words)
 *   28       4    reserved
 *   32       ...  kernel table: nkernels × struct bvbc_kernel_hdr
 *   ...      ...  constant blob: nconst bytes
 *   ...      ...  instruction stream: ninsn × 8 bytes
 *
 * Words in the instruction stream are serialized little-endian so the
 * container is endian-portable across host tiers.
 */

/* Container magic: ASCII "BVBC0001" stored little-endian as a uint64. */
#define BVBC_MAGIC UINT64_C(0x3130303043425642) /* "BVBC0001" */

#define BVBC_VERSION  1
#define BVBC_HEADER_SIZE  32

#define BVBC_MAX_KERNELS  256
#define BVBC_MAX_INSN     65536
#define BVBC_MAX_NAME     64

struct bvbc_kernel_hdr {
    uint32_t name_off;   /* byte offset of NUL-terminated name in const blob */
    uint32_t entry_off;  /* byte offset of first instruction in stream */
    uint32_t nargs;      /* kernel argument count (0–16, S16..S31) */
    uint32_t reserved;
};

struct bvbc_kernel {
    char        name[BVBC_MAX_NAME];
    uint32_t    entry;   /* index into instruction stream */
    uint32_t    nargs;
};

struct bvbc_image {
    uint32_t    version;
    uint32_t    nkernels;
    struct bvbc_kernel kernels[BVBC_MAX_KERNELS];

    const char *const_blob;       /* external, or NULL */
    size_t      nconst;

    uint64_t    *insns;           /* external, or NULL */
    uint32_t    ninsn;

    /* Builder scratch (owned by caller for in-memory assembly) */
    char        string_pool[4096];
    size_t      pool_len;
};

/* ─── In-memory assembly API (bradc) ─── */

/* Reserve a kernel slot and set its entry/name. Returns index or -1. */
int bvbc_add_kernel(struct bvbc_image *img, const char *name,
                    uint32_t entry, uint32_t nargs);

/* Append one instruction; returns index or -1 when full. */
int bvbc_add_insn32(struct bvbc_image *img, uint32_t lo32, uint32_t hi32);
static inline int bvbc_emit(struct bvbc_image *img, uint64_t insn)
{
    return bvbc_add_insn32(img, (uint32_t)insn, (uint32_t)(insn >> 32));
}

/* ─── Serialization ───
 * bvbc_serialize computes the full container size.  bvbc_pack writes it
 * into buf (must hold serialized size).  bvbc_unpack parses an image and
 * keeps pointers into the buffer (no copy of the stream). */

size_t bvbc_serialize_size(const struct bvbc_image *img);
size_t bvbc_pack(const struct bvbc_image *img, uint8_t *buf, size_t cap);
int    bvbc_unpack(const uint8_t *buf, size_t len, struct bvbc_image *img);

/* Find a kernel by name; returns index or -1. */
int bvbc_find_kernel(const struct bvbc_image *img, const char *name);

#endif /* BRAD_BVBC_H */