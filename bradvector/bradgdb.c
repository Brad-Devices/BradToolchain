#include "bradgdb.h"
#include "brad/bvrt.h"
#include "brad/bradvector.h"
#include <stdio.h>
#include <string.h>

int bradgdb_attach(struct bradgdb *g, bvrt_device *dev)
{
    if (!g || !dev)
        return -1;
    memset(g, 0, sizeof(*g));
    g->dev = dev;
    return 0;
}

int bradgdb_add_break(struct bradgdb *g, uint32_t pc)
{
    if (!g || g->nbreaks >= BRADGDB_MAX_BREAK)
        return -1;
    for (int i = 0; i < g->nbreaks; i++)
        if (g->breaks[i].pc == pc)
            return i;
    int id = g->nbreaks++;
    g->breaks[id].pc = pc;
    g->breaks[id].enabled = 1;
    g->breaks[id].hit = 0;
    return id;
}

void bradgdb_enable_break(struct bradgdb *g, int id, int enabled)
{
    if (g && id >= 0 && id < g->nbreaks)
        g->breaks[id].enabled = enabled;
}

static int at_breakpoint(const struct bradgdb *g, uint32_t pc)
{
    for (int i = 0; i < g->nbreaks; i++)
        if (g->breaks[i].enabled && g->breaks[i].pc == pc)
            return i;
    return -1;
}

enum bv_status bradgdb_step(struct bradgdb *g)
{
    enum bv_status st = bvrt_step(g->dev);
    if (st == BV_OK)
        g->steps++;
    if (g->tracing && g->dev) {
        struct bvrt_warp *w = bvrt_warp(g->dev);
        struct bvbc_image *img = bvrt_image(g->dev);
        if (w && img && (int32_t)w->pc - 1 >= 0 &&
            (uint32_t)w->pc - 1 < img->ninsn)
            printf("%4u: %s\n", (uint32_t)w->pc - 1,
                   bv_opcode_name(BV_INSN_OP(img->insns[(uint32_t)w->pc - 1])));
    }
    return st;
}

enum bv_status bradgdb_continue(struct bradgdb *g)
{
    if (!g || !g->dev)
        return BV_HALT;
    for (;;) {
        struct bvrt_warp *w = bvrt_warp(g->dev);
        if (!w || w->active == 0)
            return BV_HALT;
        int bp = at_breakpoint(g, w->pc);
        if (bp >= 0) {
            g->breaks[bp].hit++;
            return BV_OK;
        }
        enum bv_status st = bradgdb_step(g);
        if (st != BV_OK)
            return st;
    }
}

uint32_t bradgdb_read_s(struct bradgdb *g, int lane, unsigned reg)
{
    struct bvrt_warp *w = g ? bvrt_warp(g->dev) : 0;
    if (!w || lane < 0 || lane >= BV_WARP_SIZE || reg >= BV_SCALAR_REGS)
        return 0;
    return w->s[lane][reg];
}

uint32_t bradgdb_read_v(struct bradgdb *g, int lane, unsigned reg,
                        unsigned lane16)
{
    struct bvrt_warp *w = g ? bvrt_warp(g->dev) : 0;
    if (!w || lane < 0 || lane >= BV_WARP_SIZE || reg >= BV_VEC_REGS ||
        lane16 >= BV_VEC_LANES)
        return 0;
    return w->v[lane][reg][lane16];
}

uint32_t bradgdb_read_p(struct bradgdb *g, int lane, unsigned preg)
{
    struct bvrt_warp *w = g ? bvrt_warp(g->dev) : 0;
    if (!w || lane < 0 || lane >= BV_WARP_SIZE || preg >= BV_PRED_REGS)
        return 0;
    return w->p[lane][preg];
}

uint32_t bradgdb_pc(struct bradgdb *g)
{
    struct bvrt_warp *w = g ? bvrt_warp(g->dev) : 0;
    return w ? w->pc : 0;
}