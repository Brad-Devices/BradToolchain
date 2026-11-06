#include "bradtimeline.h"
#include "brad/bradvector.h"
#include "brad/bvrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bvtl *bvtl_create(bvrt_device *dev)
{
    struct bvtl *tl = calloc(1, sizeof(*tl));
    if (tl)
        tl->dev = dev;
    return tl;
}

void bvtl_destroy(struct bvtl *tl)
{
    free(tl);
}

static int popcount32(uint32_t x)
{
    int n = 0;
    for (int i = 0; i < 32; i++)
        if (x & (1u << i))
            n++;
    return n;
}

/* Retire one instruction: step the device, then record. */
static enum bv_status bvtl_retire(struct bvtl *tl)
{
    struct bvrt_warp *w = bvrt_warp(tl->dev);
    if (!w || w->active == 0)
        return BV_HALT;
    struct bvbc_image *img = bvrt_image(tl->dev);
    uint32_t pc = w->pc;
    uint8_t  active_lanes = (uint8_t)popcount32(w->active);

    enum bv_status st = bvrt_step(tl->dev);
    tl->insns++;

    if (tl->nevents < BVTL_MAX_EVENTS) {
        struct bvtl_event *e = &tl->events[tl->nevents++];
        e->cycle = tl->insns;
        e->pc = pc;
        e->op = img && pc < img->ninsn ? BV_INSN_OP(img->insns[pc])
                                       : 0xFF;
        e->active_lanes = active_lanes;
        tl->op_hist[e->op]++;
    }

    if (st != BV_OK) {
        if (st == BV_TRAP || st == BV_OOB)
            tl->traps++;
        return st;
    }
    return BV_OK;
}

enum bv_status bvtl_step(struct bvtl *tl)
{
    if (!tl || !tl->dev)
        return BV_HALT;
    return bvtl_retire(tl);
}

enum bv_status bvtl_run(struct bvtl *tl, uint64_t max_insns)
{
    if (!tl || !tl->dev)
        return BV_HALT;
    while (tl->insns < max_insns) {
        enum bv_status st = bvtl_retire(tl);
        if (st != BV_OK)
            return st;
    }
    return BV_TIMEOUT;
}

void bvtl_summary(struct bvtl *tl)
{
    if (!tl)
        return;
    printf("BradTimeline summary\n");
    printf("  instructions retired: %llu\n",
           (unsigned long long)tl->insns);
    printf("  trace events:         %zu\n", tl->nevents);
    printf("  traps:                %llu\n",
           (unsigned long long)tl->traps);
    printf("  opcode histogram:\n");
    for (int i = 0; i < 256; i++) {
        if (tl->op_hist[i])
            printf("    %-14s %llu\n", bv_opcode_name((unsigned)i),
                   (unsigned long long)tl->op_hist[i]);
    }
}

int bvtl_export_csv(struct bvtl *tl, const char *path)
{
    if (!tl)
        return -1;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "cycle,pc,op,lane_count\n");
    for (size_t i = 0; i < tl->nevents; i++) {
        const struct bvtl_event *e = &tl->events[i];
        fprintf(f, "%llu,%u,%u,%u\n", (unsigned long long)e->cycle, e->pc,
                (unsigned)e->op, (unsigned)e->active_lanes);
    }
    fclose(f);
    return 0;
}