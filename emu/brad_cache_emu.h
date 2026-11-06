#ifndef BRAD_CACHE_EMU_H
#define BRAD_CACHE_EMU_H

#include <stdint.h>
#include <stddef.h>

#define BRAD_CACHE_LINE_BITS   6
#define BRAD_CACHE_LINE_SIZE   (1 << BRAD_CACHE_LINE_BITS)
#define BRAD_CACHE_WAYS        8

enum brad_cache_level {
    CACHE_L1I = 0,
    CACHE_L1D = 1,
    CACHE_L2  = 2,
    CACHE_L3  = 3,
};

enum brad_cache_result {
    CACHE_HIT  = 0,
    CACHE_MISS = 1,
};

struct brad_cache_line {
    uint8_t  valid;
    uint8_t  dirty;
    uint8_t  lru;
    uint32_t tag;
    uint8_t  data[BRAD_CACHE_LINE_SIZE];
};

struct brad_cache {
    enum brad_cache_level level;
    unsigned sets;
    unsigned ways;
    unsigned line_size;
    unsigned latency_hit;
    unsigned latency_miss;
    struct brad_cache_line *lines;
    uint64_t hits;
    uint64_t misses;
};

struct brad_cache_hierarchy {
    struct brad_cache l1i[BRAD_CORE_MAX_CLUSTERS][BRAD_CORE_PHX_PER_CLUSTER + BRAD_CORE_FAL_PER_CLUSTER];
    struct brad_cache l1d[BRAD_CORE_MAX_CLUSTERS][BRAD_CORE_PHX_PER_CLUSTER + BRAD_CORE_FAL_PER_CLUSTER];
    struct brad_cache l2[BRAD_CORE_MAX_CLUSTERS];
    struct brad_cache l3;
};

int  brad_cache_init(struct brad_cache *cache,
                      enum brad_cache_level level,
                      unsigned size_kb, unsigned ways,
                      unsigned latency_hit,
                      unsigned latency_miss);
void brad_cache_destroy(struct brad_cache *cache);
int  brad_cache_read(struct brad_cache *cache,
                      uint32_t addr, void *buf, size_t size);
int  brad_cache_write(struct brad_cache *cache,
                       uint32_t addr, const void *buf, size_t size);
int  brad_cache_flush(struct brad_cache *cache);
int  brad_cache_flush_addr(struct brad_cache *cache, uint32_t addr);
void brad_cache_get_stats(struct brad_cache *cache,
                           uint64_t *hits, uint64_t *misses,
                           double *hit_rate);

int  brad_hierarchy_init(struct brad_cache_hierarchy *h,
                          unsigned num_clusters,
                          unsigned cores_per_cluster);
int  brad_hierarchy_read(struct brad_cache_hierarchy *h,
                          unsigned cluster, unsigned core,
                          int is_data, uint32_t addr,
                          void *buf, size_t size);
int  brad_hierarchy_write(struct brad_cache_hierarchy *h,
                           unsigned cluster, unsigned core,
                           uint32_t addr,
                           const void *buf, size_t size);
void brad_hierarchy_get_stats(struct brad_cache_hierarchy *h,
                               uint64_t *total_hits,
                               uint64_t *total_misses);

#endif
