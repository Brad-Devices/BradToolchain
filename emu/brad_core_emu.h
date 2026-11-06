#ifndef BRAD_CORE_EMU_H
#define BRAD_CORE_EMU_H

#include "brad/bradisa.h"
#include "brad/bradcore.h"
#include <stdint.h>
#include <stddef.h>

#define BRAD_MEM_SIZE            (64ULL * 1024 * 1024)
#define BRAD_MAX_PROGRAM         4096
#define BRAD_PIPELINE_DEPTH      16
#define BRAD_CORES_PER_CLUSTER   16

struct brad_pipeline_stall {
    unsigned structural;
    unsigned data_hazard;
    unsigned control_hazard;
    unsigned mem_stall;
};

struct brad_core_pipeline {
    struct brad_isa_insn *stage_insn[BRAD_PIPELINE_DEPTH];
    unsigned stage_pc[BRAD_PIPELINE_DEPTH];
    unsigned stage_valid[BRAD_PIPELINE_DEPTH];
    unsigned depth;
    unsigned exec_stage;
    unsigned head;
    unsigned tail;
    unsigned drain;
};

struct brad_core_state {
    uint32_t regs[BRAD_NUM_REGS];
    uint32_t pc;
    uint32_t sp;
    uint32_t lr;
    int running;
    uint64_t cycles;
    uint64_t insn_retired;
    uint64_t branches;
    uint64_t branches_taken;
    uint64_t cache_hits;
    uint64_t cache_misses;
    struct brad_pipeline_stall stalls;
    struct brad_core_pipeline pipeline;
    enum brad_isa_pipeline_model model;

    /* Power management */
    uint32_t msr_status;
    uint32_t msr_cause;
    uint32_t msr_pm_ctrl;
    uint32_t msr_pstate;
    uint32_t msr_voltage;
    uint32_t msr_freq;
    uint64_t msr_energy;
    uint32_t msr_perf_cnt[2];
    uint32_t msr_perf_cnt_ctrl;
    uint32_t msr_cg_mask;

    /* Compressed mode */
    int compress_mode;
    uint16_t pending_half;  /* halfword buffer for packing */
    int has_pending;
};

struct brad_soc_emu {
    struct brad_core_state cores[BRAD_CORE_MAX_CLUSTERS][BRAD_CORES_PER_CLUSTER];
    uint8_t *memory;
    size_t mem_size;
    unsigned num_clusters;
    unsigned phoenix_per_cluster;
    unsigned falcon_per_cluster;
    unsigned kestrel_per_cluster;
    unsigned falcon_lite_per_cluster;
    uint64_t total_cycles;
};

int  brad_soc_emu_init(struct brad_soc_emu *soc,
                        unsigned num_clusters,
                        unsigned phoenix_per_cluster,
                        unsigned falcon_per_cluster);
int  brad_soc_emu_init_ex(struct brad_soc_emu *soc,
                           unsigned num_clusters,
                           unsigned phoenix_per_cluster,
                           unsigned falcon_per_cluster,
                           unsigned kestrel_per_cluster,
                           unsigned falcon_lite_per_cluster);
void brad_soc_emu_free(struct brad_soc_emu *soc);
int  brad_core_emu_init(struct brad_core_state *core,
                         enum brad_isa_pipeline_model model,
                         uint32_t entry_pc, uint32_t sp);
int  brad_core_emu_load(struct brad_soc_emu *soc,
                         const uint32_t *program, size_t num_words,
                         uint32_t addr);
int  brad_core_emu_step(struct brad_soc_emu *soc,
                         unsigned cluster, unsigned core_idx);
int  brad_core_emu_run(struct brad_soc_emu *soc,
                        unsigned cluster, unsigned core_idx,
                        uint64_t max_cycles);
int  brad_core_emu_run_all(struct brad_soc_emu *soc,
                            uint64_t max_cycles);
uint32_t brad_core_read_reg(struct brad_core_state *core,
                             unsigned reg);
void brad_core_write_reg(struct brad_core_state *core,
                          unsigned reg, uint32_t val);
int  brad_core_emu_disasm(uint32_t insn, char *buf, size_t buf_size);
uint32_t brad_core_read_msr(struct brad_core_state *core, unsigned msr);
void brad_core_write_msr(struct brad_core_state *core, unsigned msr, uint32_t val);

#endif
