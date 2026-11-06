#include "brad/bvbc.h"
#include <string.h>
#include <stdio.h>

/* Container layout (little-endian), byte offsets from start of buffer:
 *
 *   0        8   magic        "BVBC0001"
 *   8        4   version      BVBC_VERSION
 *   12       4   flags        reserved (0)
 *   16       4   nkern        kernel count
 *   20       4   nconst_total total const-region bytes (names + data)
 *   24       4   ninsn        instruction count
 *   28       4   reserved
 *   32       ... kernel table (nkern × 16 bytes)
 *   C0       ... const region
 *                [0, strpool_len)   packed kernel names (NUL-terminated)
 *                [strpool_len, nconst_total)  user constant data
 *   I0       ... instruction stream (ninsn × 8 bytes)
 *
 * Kernel-table entry fields:
 *   name_off   : byte offset of the name within the const region
 *   entry_off  : byte offset of the first instruction within the
 *                instruction stream (== entry_index * 8)
 */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

int bvbc_add_kernel(struct bvbc_image *img, const char *name,
                    uint32_t entry, uint32_t nargs)
{
    if (!img || img->nkernels >= BVBC_MAX_KERNELS)
        return -1;

    struct bvbc_kernel *k = &img->kernels[img->nkernels];
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= BVBC_MAX_NAME)
        return -1;
    if (img->pool_len + name_len + 1 > sizeof(img->string_pool))
        return -1;

    memcpy(img->string_pool + img->pool_len, name, name_len + 1);
    img->pool_len += name_len + 1;
    strncpy(k->name, name, BVBC_MAX_NAME - 1);
    k->name[BVBC_MAX_NAME - 1] = '\0';
    k->entry = entry;
    k->nargs = nargs;
    img->nkernels++;
    return img->nkernels - 1;
}

int bvbc_add_insn32(struct bvbc_image *img, uint32_t lo32, uint32_t hi32)
{
    if (!img || img->ninsn >= BVBC_MAX_INSN)
        return -1;
    img->insns[img->ninsn] = lo32 | ((uint64_t)hi32 << 32);
    return img->ninsn++;
}

int bvbc_find_kernel(const struct bvbc_image *img, const char *name)
{
    for (uint32_t i = 0; i < img->nkernels; i++)
        if (strcmp(img->kernels[i].name, name) == 0)
            return i;
    return -1;
}

static size_t strpool_len(const struct bvbc_image *img)
{
    return img->pool_len;
}

static size_t kernel_table_size(const struct bvbc_image *img)
{
    return 16u * img->nkernels;
}

static uint32_t kernel_name_off(const struct bvbc_image *img, uint32_t ki)
{
    size_t off = 0;
    for (uint32_t i = 0; i < ki && i < img->nkernels; i++)
        off += strlen(img->kernels[i].name) + 1;
    return (uint32_t)off;
}

size_t bvbc_serialize_size(const struct bvbc_image *img)
{
    return BVBC_HEADER_SIZE + kernel_table_size(img) +
           strpool_len(img) + img->nconst +
           (size_t)img->ninsn * BV_INSN_SIZE;
}

static size_t const_data_off(const struct bvbc_image *img)
{
    return strpool_len(img);
}

size_t bvbc_pack(const struct bvbc_image *img, uint8_t *buf, size_t cap)
{
    size_t total = bvbc_serialize_size(img);
    if (cap < total || !buf)
        return 0;

    uint8_t *p = buf;
    size_t c0 = BVBC_HEADER_SIZE + kernel_table_size(img);

    wr64(p, BVBC_MAGIC); p += 8;
    wr32(p, BVBC_VERSION);    p += 4;
    wr32(p, 0);               p += 4;                     /* flags */
    wr32(p, img->nkernels);   p += 4;
    wr32(p, (uint32_t)(strpool_len(img) + img->nconst));  p += 4;
    wr32(p, img->ninsn);      p += 4;
    wr32(p, 0);               p += 4;                     /* reserved */

    /* kernel table */
    for (uint32_t i = 0; i < img->nkernels; i++) {
        wr32(p, kernel_name_off(img, i));    p += 4;
        wr32(p, img->kernels[i].entry * BV_INSN_SIZE); p += 4;
        wr32(p, img->kernels[i].nargs);      p += 4;
        wr32(p, 0);                          p += 4;
    }

    /* const region: names, then user data */
    memcpy(buf + c0, img->string_pool, strpool_len(img));
    if (img->nconst)
        memcpy(buf + c0 + const_data_off(img), img->const_blob, img->nconst);

    /* instruction stream */
    for (uint32_t i = 0; i < img->ninsn; i++)
        wr64(buf + c0 + strpool_len(img) + img->nconst +
             (size_t)i * BV_INSN_SIZE, img->insns[i]);

    return total;
}

int bvbc_unpack(const uint8_t *buf, size_t len, struct bvbc_image *img)
{
    uint8_t magic[8];
    for (int i = 0; i < 8; i++)
        magic[i] = (uint8_t)(BVBC_MAGIC >> (8 * i));

    if (!buf || !img || len < BVBC_HEADER_SIZE)
        return -1;
    if (memcmp(buf, magic, 8) != 0)
        return -1;

    uint32_t ver     = rd32(buf + 8);
    uint32_t nkern   = rd32(buf + 16);
    uint32_t nconst  = rd32(buf + 20);
    uint32_t ninsn   = rd32(buf + 24);
    (void)ver;

    if (nkern > BVBC_MAX_KERNELS || ninsn > BVBC_MAX_INSN)
        return -1;

    size_t kt_size = 16u * nkern;
    size_t c0 = BVBC_HEADER_SIZE + kt_size;
    if (len < c0 + nconst + (size_t)ninsn * BV_INSN_SIZE)
        return -1;

    memset(img, 0, sizeof(*img));
    img->version = 1;
    img->nkernels = nkern;
    img->nconst = nconst;
    img->ninsn = ninsn;

    /* kernel table */
    for (uint32_t i = 0; i < nkern; i++) {
        const uint8_t *e = buf + BVBC_HEADER_SIZE + i * 16;
        uint32_t noff  = rd32(e);
        uint32_t eoff  = rd32(e + 4);
        uint32_t narg  = rd32(e + 8);

        if (noff >= nconst)
            return -1;
        if (eoff % BV_INSN_SIZE != 0)
            return -1;

        img->kernels[i].entry = eoff / BV_INSN_SIZE;
        img->kernels[i].nargs = narg;

        const uint8_t *nsrc = buf + c0 + noff;
        size_t nl = 0;
        while (nl < nconst - noff && nsrc[nl] && nl < BVBC_MAX_NAME - 1)
            nl++;
        memcpy(img->kernels[i].name, nsrc, nl);
        img->kernels[i].name[nl] = '\0';
    }

    /* compute names region length from the kernel names so the user
       const data sits just past it */
    size_t names_len = 0;
    for (uint32_t i = 0; i < nkern; i++)
        names_len += strlen(img->kernels[i].name) + 1;
    if (names_len > nconst)
        return -1;

    img->const_blob = (const char *)(buf + c0 + names_len);
    img->insns = (uint64_t *)(void *)(buf + c0 + nconst);
    return 0;
}