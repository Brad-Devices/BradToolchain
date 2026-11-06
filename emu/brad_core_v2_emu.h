#ifndef BRAD_CORE_V2_EMU_H
#define BRAD_CORE_V2_EMU_H

#include "brad/bradisa.h"
#include <stdint.h>
#include <stddef.h>

#define BRAD_V2_MEM_SIZE (1024 * 1024)

typedef struct {
    uint32_t regs[BRAD_V2_NUM_REGS];
    uint32_t pc;
    uint8_t *mem;
    size_t mem_size;
    int running;
    uint64_t steps;
    uint64_t insn_retired;
} brad_v2_state;

void brad_v2_init(brad_v2_state *s);
void brad_v2_free(brad_v2_state *s);
int  brad_v2_load(brad_v2_state *s, const uint32_t *code, size_t nwords, uint32_t entry);
int  brad_v2_step(brad_v2_state *s);   /* 1 if still running */
int  brad_v2_run(brad_v2_state *s, uint64_t max_steps);

#endif
