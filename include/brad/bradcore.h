#ifndef BRAD_CORE_H
#define BRAD_CORE_H

#include <stdint.h>
#include <stddef.h>
#include "bradisa.h"

#define BRAD_CORE_MAX_CLUSTERS      4
#define BRAD_CORE_PHX_PER_CLUSTER   4
#define BRAD_CORE_FAL_PER_CLUSTER   4
#define BRAD_CORE_MAX_NAME          48
#define BRAD_CORE_CONSUMER_NAME     64
#define BRAD_CORE_MODEL_CODE_LEN    16
#define BRAD_CORE_HIDDEN_SKU_DIGITS 3
#define BRAD_CORE_MAX_PHOENIX       8
#define BRAD_CORE_MAX_FALCON        16

enum brad_core_type {
    BRAD_CORE_PHOENIX  = 0,
    BRAD_CORE_FALCON   = 1,
    BRAD_CORE_SENTINEL = 2,
};

enum brad_core_series {
    BRAD_SERIES_C = 0,
    BRAD_SERIES_G = 1,
    BRAD_SERIES_W = 2,
};

enum brad_core_suffix {
    BRAD_SUFFIX_NONE = 0,
    BRAD_SUFFIX_E    = 1,
    BRAD_SUFFIX_U    = 2,
    BRAD_SUFFIX_FP   = 3,
};

enum brad_core_sku {
    BRAD_CORE_P1E4 = 0,
    BRAD_CORE_P2E4 = 1,
    BRAD_CORE_P4E4 = 2,
    BRAD_CORE_P6E4 = 3,
    BRAD_CORE_P4E8 = 4,
    BRAD_CORE_P8E4 = 5,
    BRAD_CORE_P8E8 = 6,
    BRAD_CORE_G60  = 7,
    BRAD_CORE_G80  = 8,
    BRAD_CORE_W88  = 9,
    BRAD_CORE_W816 = 10,
};

enum brad_platform_tier {
    BRAD_PLATFORM_MIND_LITE     = 0,
    BRAD_PLATFORM_MIND_STANDARD = 1,
    BRAD_PLATFORM_MIND_PRO      = 2,
    BRAD_PLATFORM_MIND_ELITE    = 3,
    BRAD_PLATFORM_NEURO_ULTRA   = 4,
    BRAD_PLATFORM_NEURO_ELITE   = 5,
    BRAD_PLATFORM_XON           = 6,
};

enum brad_process_node {
    BRAD_PROCESS_4NM = 0,
    BRAD_PROCESS_3NM = 1,
    BRAD_PROCESS_2NM = 2,
};

enum brad_gen {
    BRAD_GEN_KINETIC  = 0,
    BRAD_GEN_SYNAPTIC = 1,
    BRAD_GEN_QUANTUM  = 2,
};

enum brad_core_consumer_tier {
    BRAD_CONSUMER_K3 = 0,
    BRAD_CONSUMER_K5 = 1,
    BRAD_CONSUMER_K7 = 2,
    BRAD_CONSUMER_K9 = 3,
    BRAD_CONSUMER_ULTRA5 = 4,
    BRAD_CONSUMER_ULTRA7 = 5,
    BRAD_CONSUMER_ULTRA9 = 6,
};

struct brad_core_naming {
    char                     consumer_name[BRAD_CORE_CONSUMER_NAME];
    char                     model_code[BRAD_CORE_MODEL_CODE_LEN];
    enum brad_gen            gen;
    enum brad_core_consumer_tier consumer_tier;
    enum brad_core_series    series;
    enum brad_core_suffix    suffix;
    unsigned                 hidden_sku_code;
    unsigned                 hidden_phoenix;
    unsigned                 hidden_falcon;
    unsigned                 hidden_platform;
};

enum brad_core_power_state {
    BRAD_PSTATE_TURBO      = 0,
    BRAD_PSTATE_BALANCED   = 1,
    BRAD_PSTATE_EFFICIENT  = 2,
    BRAD_PSTATE_RETENTION  = 3,
    BRAD_PSTATE_OFF        = 4,
};

struct brad_core_sku_cfg {
    enum brad_core_sku       sku;
    const char               name[BRAD_CORE_MAX_NAME];
    char                     model_code[BRAD_CORE_MODEL_CODE_LEN];
    unsigned                 phoenix_count;
    unsigned                 falcon_count;
    enum brad_platform_tier  platform;
    enum brad_process_node   process;
    unsigned                 freq_max_mhz;
    unsigned                 freq_falcon_mhz;
    unsigned                 tdp_w;
    unsigned                 l3_cache_kb;
    enum brad_core_series    series;
    enum brad_core_suffix    suffix;
};

struct brad_core_cluster_cfg {
    unsigned                 cluster_id;
    unsigned                 num_phoenix;
    unsigned                 num_falcon;
    unsigned                 l2_size_kb;
    int                      snoop_enabled;
};

struct brad_core_info {
    enum brad_core_sku       sku;
    struct brad_core_sku_cfg cfg;
    unsigned                 active_clusters;
    unsigned                 total_phoenix;
    unsigned                 total_falcon;
    unsigned                 freq_current_mhz;
    enum brad_core_power_state p_state;
    uint64_t                 cycles_total;
    unsigned                 temp_c;
};

struct brad_core_regs {
    volatile uint32_t CORE_CTRL;
    volatile uint32_t CORE_STATUS;
    volatile uint32_t CORE_PWR_STATE;
    volatile uint32_t CORE_CLK_MHZ;
    volatile uint32_t CORE_VOLT_MV;
    volatile uint32_t CORE_L2_WAY_MASK;
    volatile uint64_t CORE_PC;
    volatile uint32_t CORE_INT_ID;
    volatile uint32_t CORE_CACHE_CTRL;
    volatile uint32_t CORE_PERF_CNTR0;
    volatile uint32_t CORE_PERF_CNTR1;
};

#define CORE_CTRL_ENABLE         (1U << 0)
#define CORE_CTRL_RESET          (1U << 1)
#define CORE_CTRL_HALT           (1U << 2)
#define CORE_CTRL_SINGLE_STEP    (1U << 3)
#define CORE_CTRL_FLUSH_DCACHE   (1U << 8)
#define CORE_CTRL_FLUSH_ICACHE   (1U << 9)
#define CORE_STATUS_RUNNING      (1U << 0)
#define CORE_STATUS_HALTED       (1U << 1)
#define CORE_STATUS_WFE          (1U << 2)
#define CORE_PWR_STATE_MASK      0x7

struct brad_cluster_regs {
    volatile uint32_t CL_CTRL;
    volatile uint32_t CL_STATUS;
    volatile uint32_t CL_L2_CTRL;
    volatile uint32_t CL_L2_SIZE;
    volatile uint32_t CL_SNOOP_CTRL;
    volatile uint32_t CL_PWR_STATE;
    volatile uint32_t CL_FABRIC_IF;
};

#define CL_CTRL_ENABLE          (1U << 0)
#define CL_CTRL_L2_FLUSH        (1U << 1)
#define CL_CTRL_SNOOP_ENABLE    (1U << 8)

int  brad_soc_init(enum brad_core_sku sku);
int  brad_core_init(enum brad_core_type type,
                     unsigned cluster, unsigned core_in_cluster);
int  brad_core_start(unsigned cluster, unsigned core);
int  brad_core_halt(unsigned cluster, unsigned core);
int  brad_core_set_freq(unsigned cluster, unsigned core,
                         unsigned freq_mhz);
int  brad_core_set_volt(unsigned cluster, unsigned core,
                         unsigned volt_mv);
int  brad_core_set_power_state(unsigned cluster, unsigned core,
                                unsigned state);
int  brad_core_flush_cache(unsigned cluster, unsigned core,
                            int dcache, int icache);
int  brad_cluster_init(unsigned cluster);
int  brad_cluster_set_l2_way(unsigned cluster, unsigned way_mask);
int  brad_core_query_sku(struct brad_core_sku_cfg *cfg);
int  brad_core_query_info(struct brad_core_info *info);
int  brad_core_query_freq_table(unsigned *freqs, unsigned *count);
int  brad_cluster_configure(struct brad_core_cluster_cfg *cfg);
int  brad_core_set_l3_partition(unsigned cluster, unsigned way_mask);
int  brad_core_get_temp(unsigned cluster, unsigned core,
                         unsigned *temp_c);
int  brad_core_query_naming(struct brad_core_naming *naming);
int  brad_core_decode_sku(unsigned hidden_code,
                           unsigned *phoenix, unsigned *falcon,
                           enum brad_platform_tier *platform);
int  brad_core_query_model_code(char *buf, size_t buf_size);
int  brad_core_query_series(enum brad_core_series *series,
                             enum brad_core_suffix *suffix);

#endif
