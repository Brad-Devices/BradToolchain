#include "brad/bradcore.h"
#include "brad_cache_emu.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>

int brad_cache_init(struct brad_cache *cache,
                     enum brad_cache_level level,
                     unsigned size_kb, unsigned ways,
                     unsigned latency_hit,
                     unsigned latency_miss)
{
    if (!cache)
        return -EINVAL;
    if (ways == 0 || size_kb == 0)
        return -EINVAL;

    size_t total_bytes = (size_t)size_kb * 1024;
    unsigned line_size = BRAD_CACHE_LINE_SIZE;
    unsigned num_lines = (unsigned)(total_bytes / line_size);
    unsigned sets = num_lines / ways;

    if (sets == 0)
        return -EINVAL;

    cache->level = level;
    cache->sets = sets;
    cache->ways = ways;
    cache->line_size = line_size;
    cache->latency_hit = latency_hit;
    cache->latency_miss = latency_miss;
    cache->hits = 0;
    cache->misses = 0;

    cache->lines = (struct brad_cache_line *)
        calloc(num_lines, sizeof(struct brad_cache_line));
    if (!cache->lines)
        return -ENOMEM;

    return 0;
}

void brad_cache_destroy(struct brad_cache *cache)
{
    if (cache && cache->lines) {
        free(cache->lines);
        cache->lines = NULL;
    }
}

static struct brad_cache_line *find_line(struct brad_cache *cache,
                                          uint32_t addr,
                                          int for_write)
{
    unsigned line_bits = BRAD_CACHE_LINE_BITS;
    unsigned set_bits = 0;
    unsigned temp = cache->sets;
    while (temp >>= 1) set_bits++;

    unsigned set = (addr >> line_bits) & ((1 << set_bits) - 1);
    uint32_t tag = addr >> (line_bits + set_bits);

    if (set >= cache->sets)
        return NULL;

    unsigned line_idx = set * cache->ways;
    struct brad_cache_line *set_lines = &cache->lines[line_idx];
    struct brad_cache_line *lru_line = &set_lines[0];
    unsigned lru_val = set_lines[0].lru;

    for (unsigned i = 0; i < cache->ways; i++) {
        struct brad_cache_line *cl = &set_lines[i];
        if (cl->valid && cl->tag == tag) {
            cl->lru = 0;
            cache->hits++;
            return cl;
        }
        if (cl->lru > lru_val) {
            lru_val = cl->lru;
            lru_line = cl;
        }
    }

    cache->misses++;
    memset(lru_line, 0, sizeof(*lru_line));
    lru_line->valid = 1;
    lru_line->tag = tag;
    lru_line->dirty = for_write ? 1 : 0;
    lru_line->lru = 0;
    return lru_line;
}

int brad_cache_read(struct brad_cache *cache,
                     uint32_t addr, void *buf, size_t size)
{
    if (!cache || !buf)
        return -EINVAL;
    struct brad_cache_line *cl = find_line(cache, addr, 0);
    if (!cl)
        return -EFAULT;
    memcpy(buf, cl->data, size);
    return (int)size;
}

int brad_cache_write(struct brad_cache *cache,
                      uint32_t addr, const void *buf, size_t size)
{
    if (!cache || !buf)
        return -EINVAL;
    struct brad_cache_line *cl = find_line(cache, addr, 1);
    if (!cl)
        return -EFAULT;
    cl->dirty = 1;
    memcpy(cl->data, buf, size);
    return (int)size;
}

int brad_cache_flush(struct brad_cache *cache)
{
    if (!cache || !cache->lines)
        return -EINVAL;
    unsigned num_lines = cache->sets * cache->ways;
    for (unsigned i = 0; i < num_lines; i++) {
        cache->lines[i].valid = 0;
        cache->lines[i].dirty = 0;
        cache->lines[i].lru = 0;
    }
    return 0;
}

int brad_cache_flush_addr(struct brad_cache *cache, uint32_t addr)
{
    if (!cache)
        return -EINVAL;
    unsigned line_bits = BRAD_CACHE_LINE_BITS;
    unsigned set_bits = 0;
    unsigned temp = cache->sets;
    while (temp >>= 1) set_bits++;
    unsigned set = (addr >> line_bits) & ((1 << set_bits) - 1);
    uint32_t tag = addr >> (line_bits + set_bits);

    if (set >= cache->sets)
        return -EINVAL;

    unsigned line_idx = set * cache->ways;
    for (unsigned i = 0; i < cache->ways; i++) {
        struct brad_cache_line *cl = &cache->lines[line_idx + i];
        if (cl->valid && cl->tag == tag) {
            cl->valid = 0;
            cl->dirty = 0;
            return 0;
        }
    }
    return 0;
}

void brad_cache_get_stats(struct brad_cache *cache,
                           uint64_t *hits, uint64_t *misses,
                           double *hit_rate)
{
    if (hits)     *hits     = cache->hits;
    if (misses)   *misses   = cache->misses;
    if (hit_rate) {
        uint64_t total = cache->hits + cache->misses;
        *hit_rate = (total > 0) ? (double)cache->hits / total * 100.0 : 0.0;
    }
}

int brad_hierarchy_init(struct brad_cache_hierarchy *h,
                         unsigned num_clusters,
                         unsigned cores_per_cluster)
{
    if (!h)
        return -EINVAL;
    if (num_clusters > BRAD_CORE_MAX_CLUSTERS)
        return -EINVAL;
    if (cores_per_cluster > BRAD_CORE_PHX_PER_CLUSTER + BRAD_CORE_FAL_PER_CLUSTER)
        return -EINVAL;

    memset(h, 0, sizeof(*h));

    for (unsigned c = 0; c < num_clusters; c++) {
        for (unsigned i = 0; i < cores_per_cluster; i++) {
            brad_cache_init(&h->l1i[c][i], CACHE_L1I, 32, 4, 1, 8);
            brad_cache_init(&h->l1d[c][i], CACHE_L1D, 32, 8, 2, 10);
        }
        brad_cache_init(&h->l2[c], CACHE_L2, 512, 8, 8, 25);
    }
    brad_cache_init(&h->l3, CACHE_L3, 8192, 16, 25, 80);

    return 0;
}

int brad_hierarchy_read(struct brad_cache_hierarchy *h,
                         unsigned cluster, unsigned core,
                         int is_data, uint32_t addr,
                         void *buf, size_t size)
{
    if (!h || !buf)
        return -EINVAL;

    struct brad_cache *l1 = is_data
        ? &h->l1d[cluster][core]
        : &h->l1i[cluster][core];

    int ret = brad_cache_read(l1, addr, buf, size);
    if (ret >= 0)
        return ret;

    if (brad_cache_read(&h->l2[cluster], addr, buf, size) >= 0)
        return (int)size;

    if (brad_cache_read(&h->l3, addr, buf, size) >= 0)
        return (int)size;

    memset(buf, 0, size);
    return (int)size;
}

int brad_hierarchy_write(struct brad_cache_hierarchy *h,
                          unsigned cluster, unsigned core,
                          uint32_t addr,
                          const void *buf, size_t size)
{
    if (!h || !buf)
        return -EINVAL;

    brad_cache_write(&h->l1d[cluster][core], addr, buf, size);
    return (int)size;
}

void brad_hierarchy_get_stats(struct brad_cache_hierarchy *h,
                               uint64_t *total_hits,
                               uint64_t *total_misses)
{
    if (!h) return;
    uint64_t hits = 0, misses = 0;
    for (unsigned c = 0; c < BRAD_CORE_MAX_CLUSTERS; c++) {
        for (unsigned i = 0; i < BRAD_CORE_PHX_PER_CLUSTER + BRAD_CORE_FAL_PER_CLUSTER; i++) {
            hits   += h->l1i[c][i].hits + h->l1d[c][i].hits;
            misses += h->l1i[c][i].misses + h->l1d[c][i].misses;
        }
        hits   += h->l2[c].hits;
        misses += h->l2[c].misses;
    }
    hits   += h->l3.hits;
    misses += h->l3.misses;
    if (total_hits)   *total_hits   = hits;
    if (total_misses) *total_misses = misses;
}
