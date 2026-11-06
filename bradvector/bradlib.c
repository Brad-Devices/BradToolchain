#include "brad/bradlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── BradVector Platform API (reference implementation) ───
 *
 * Thin orchestration over bradc / bvbc / bvrt / bradgdb that owns its
 * buffers so host code has one stable surface to program against.
 */

struct bradlib_program {
    struct bvbc_image img;
    uint64_t         *insns;    /* owned instruction stream */
};

struct bradlib_session {
    struct bradlib_program *prog;
    bvrt_device            *dev;
    struct bradgdb          gdb;
    int                     gdb_attached;
    uint32_t                kernel_idx;
    size_t                  mem_bytes;
};

/* ─── compilation ─── */

struct bradlib_program *bradlib_compile(const char *source,
                                        struct bradc_error *err)
{
    if (!source)
        return NULL;

    struct bradlib_program *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->insns = calloc(BVBC_MAX_INSN, sizeof(uint64_t));
    if (!p->insns) {
        free(p);
        return NULL;
    }

    p->img.insns = p->insns;
    if (bradc_assemble(source, &p->img, err) != 0) {
        free(p->insns);
        free(p);
        return NULL;
    }
    return p;
}

void bradlib_free_program(struct bradlib_program *p)
{
    if (!p)
        return;
    free(p->insns);
    free(p);
}

unsigned bradlib_kernel_count(const struct bradlib_program *p)
{
    return p ? p->img.nkernels : 0;
}

const char *bradlib_kernel_name(const struct bradlib_program *p,
                                unsigned idx)
{
    if (!p || idx >= p->img.nkernels)
        return NULL;
    return p->img.kernels[idx].name;
}

unsigned bradlib_insn_count(const struct bradlib_program *p)
{
    return p ? p->img.ninsn : 0;
}

/* ─── serialization ─── */

size_t bradlib_serialize_size(const struct bradlib_program *p)
{
    return p ? bvbc_serialize_size(&p->img) : 0;
}

size_t bradlib_pack(const struct bradlib_program *p,
                    uint8_t *buf, size_t cap)
{
    if (!p || !buf)
        return 0;
    return bvbc_pack(&p->img, buf, cap);
}

/* ─── disassembly / symbols ─── */

int bradlib_disasm(const struct bradlib_program *p, uint32_t pc,
                   char *buf, size_t cap)
{
    if (!p || !buf || cap == 0)
        return -1;
    if (pc >= p->img.ninsn)
        return -1;

    struct bv_insn d = bv_decode(p->img.insns[pc]);
    int n = snprintf(buf, cap, "%4u:  %s  pred=%u dst=%u src1=%u src2=%u imm=%d",
                     pc, bv_opcode_name(d.op), d.pred, d.dst, d.src1,
                     d.src2, d.imm);
    return (n < 0) ? -1 : n;
}

int bradlib_symbols(const struct bradlib_program *p,
                    struct bradc_symbol *sym, size_t cap, size_t *n)
{
    if (!p || !n)
        return -1;
    return bradc_symbols(&p->img, sym, cap, n);
}

/* ─── runtime sessions ─── */

struct bradlib_session *bradlib_create(struct bradlib_program *p,
                                       size_t mem_bytes)
{
    if (!p)
        return NULL;

    struct bradlib_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->prog = p;
    s->dev = bvrt_create(mem_bytes);
    if (!s->dev) {
        free(s);
        return NULL;
    }
    s->mem_bytes = (mem_bytes < BVRT_MAX_MEM) ? mem_bytes : BVRT_MAX_MEM;
    if (bvrt_load(s->dev, &p->img) != 0) {
        bvrt_destroy(s->dev);
        free(s);
        return NULL;
    }
    if (bradgdb_attach(&s->gdb, s->dev) == 0)
        s->gdb_attached = 1;
    s->kernel_idx = (uint32_t)-1;
    return s;
}

void bradlib_destroy(struct bradlib_session *s)
{
    if (!s)
        return;
    if (s->dev)
        bvrt_destroy(s->dev);
    free(s);
}

int bradlib_launch(struct bradlib_session *s, const char *kernel,
                   const uint32_t *args, unsigned nargs)
{
    if (!s || !kernel)
        return -1;
    int rc = bvrt_launch(s->dev, kernel, args, nargs);
    if (rc == 0)
        s->kernel_idx = (uint32_t)bvbc_find_kernel(&s->prog->img, kernel);
    return rc;
}

enum bv_status bradlib_run(struct bradlib_session *s, uint64_t max_insns)
{
    if (!s || !s->dev)
        return BV_TRAP;
    return bvrt_run(s->dev, max_insns);
}

enum bv_status bradlib_step(struct bradlib_session *s)
{
    if (!s || !s->dev)
        return BV_TRAP;
    return bvrt_step(s->dev);
}

/* ─── inspection ─── */

uint32_t bradlib_read_s(struct bradlib_session *s, int lane, unsigned reg)
{
    if (!s || !s->dev)
        return 0;
    struct bvrt_warp *wp = bvrt_warp(s->dev);
    if (!wp || lane < 0 || lane >= BV_WARP_SIZE ||
        reg >= BV_SCALAR_REGS)
        return 0;
    return wp->s[lane][reg];
}

uint32_t bradlib_read_v(struct bradlib_session *s, int lane,
                        unsigned reg, unsigned lane16)
{
    if (!s || !s->dev)
        return 0;
    struct bvrt_warp *wp = bvrt_warp(s->dev);
    if (!wp || lane < 0 || lane >= BV_WARP_SIZE ||
        reg >= BV_VEC_REGS || lane16 >= BV_VEC_LANES)
        return 0;
    return wp->v[lane][reg][lane16];
}

uint32_t bradlib_read_p(struct bradlib_session *s, int lane, unsigned preg)
{
    if (!s || !s->dev)
        return 0;
    struct bvrt_warp *wp = bvrt_warp(s->dev);
    if (!wp || lane < 0 || lane >= BV_WARP_SIZE || preg >= BV_PRED_REGS)
        return 0;
    return wp->p[lane][preg];
}

uint32_t bradlib_pc(struct bradlib_session *s)
{
    if (!s || !s->dev)
        return 0;
    struct bvrt_warp *wp = bvrt_warp(s->dev);
    return wp ? wp->pc : 0;
}

uint64_t bradlib_cycles(struct bradlib_session *s)
{
    if (!s || !s->dev)
        return 0;
    struct bvrt_warp *wp = bvrt_warp(s->dev);
    return wp ? wp->cycles : 0;
}

void *bradlib_mem(struct bradlib_session *s)
{
    if (!s || !s->dev)
        return NULL;
    return bvrt_mem(s->dev);
}

size_t bradlib_mem_size(struct bradlib_session *s)
{
    return s ? s->mem_bytes : 0;
}

/* ─── debug ─── */

int bradlib_breakpoint(struct bradlib_session *s, uint32_t pc)
{
    if (!s || !s->gdb_attached)
        return -1;
    int id = bradgdb_add_break(&s->gdb, pc);
    if (id >= 0)
        bradgdb_enable_break(&s->gdb, id, 1);
    return id;
}

enum bv_status bradlib_continue(struct bradlib_session *s)
{
    if (!s || !s->gdb_attached)
        return BV_TRAP;
    return bradgdb_continue(&s->gdb);
}