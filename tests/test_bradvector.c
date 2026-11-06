#include "brad/bradvector.h"
#include "brad/bvbc.h"
#include "brad/bvrt.h"
#include "bradc.h"
#include "bradgdb.h"
#include "bradtimeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Always evaluate `expr` (unlike assert, which is compiled out under
   -DNDEBUG and would skip side-effecting setup calls) and abort on
   failure regardless of build type. */
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            fprintf(stderr, "  FAIL %s\n", #expr);                      \
            exit(EXIT_FAILURE);                                         \
        }                                                               \
    } while (0)

#define TEST_MEM(_dev)  ((uint8_t *)bvrt_mem((_dev)))

/* saxpy: y[i] = a*x[i] + y[i] (integer ops).
 * Args: S16 = x base offset, S17 = y base offset, S18 = a.
 * S1 = lane id, S0 = 0.  Each lane handles index i = lane.
 * offset = lane*4 via S2 = S1+S1+S1+S1 (no immediates). */
static const char saxp_src[] =
    ".kernel saxpy\n"
    "IADD S2, S1, S1\n"        /* lane*2 */
    "IADD S2, S2, S2\n"        /* lane*4 */
    "IADD S7, S16, S2\n"       /* &x[i] */
    "LOAD S3, S7, 0\n"         /* x[i] */
    "IMUL S4, S3, S18\n"       /* a*x[i] */
    "IADD S8, S17, S2\n"       /* &y[i] */
    "LOAD S5, S8, 0\n"         /* y[i] */
    "IADD S6, S4, S5\n"        /* a*x[i]+y[i] */
    "STORE S8, S6, 0\n"        /* y[i] = ... */
    "EXIT\n"
    ".end\n";

#define SAXPY_NINS 10

static void assemble_saxpy(struct bvbc_image *img, uint64_t *insns)
{
    memset(img, 0, sizeof(*img));
    img->insns = insns;
    struct bradc_error err;
    CHECK(bradc_assemble(saxp_src, img, &err) == 0);
}

static void test_assemble_vector(void)
{
    uint64_t insns[256];
    struct bvbc_image img;
    assemble_saxpy(&img, insns);

    CHECK(img.nkernels == 1);
    CHECK(img.ninsn == SAXPY_NINS);
    int ki = bvbc_find_kernel(&img, "saxpy");
    CHECK(ki == 0);
    CHECK(img.kernels[ki].entry == 0);

    struct bv_insn m = bv_decode(img.insns[2]);   /* IADD S7, S16, S2 */
    CHECK(m.op == BV_OP_IADD);
    CHECK(m.dst == 7 && m.src1 == 16 && m.src2 == 2);

    struct bradc_error e2;
    memset(&e2, 0, sizeof(e2));
    CHECK(bradc_assemble(".kernel k\nFOO S1, S2\n.end\n", &img, &e2) < 0);
    CHECK(strstr(e2.msg, "unknown opcode"));
}

static void test_bvbc_roundtrip(void)
{
    uint64_t insns[256];
    struct bvbc_image img;
    assemble_saxpy(&img, insns);

    size_t n = bvbc_serialize_size(&img);
    CHECK(n > 0);
    CHECK(n >= 32 + 16 + (strlen("saxpy") + 1) + SAXPY_NINS * 8);
    uint8_t *buf = malloc(n);
    CHECK(buf);
    CHECK(bvbc_pack(&img, buf, n) == n);

    struct bvbc_image out;
    CHECK(bvbc_unpack(buf, n, &out) == 0);
    CHECK(out.nkernels == 1);
    CHECK(out.ninsn == SAXPY_NINS);
    CHECK(strcmp(out.kernels[0].name, "saxpy") == 0);
    for (uint32_t i = 0; i < out.ninsn; i++)
        CHECK(out.insns[i] == insns[i]);

    memset(&out, 0, sizeof(out));
    CHECK(bvbc_unpack(buf, n - 1, &out) < 0);

    free(buf);
}

static void run_saxpy(struct bvbc_image *img, int trace)
{
    static uint32_t x[BV_WARP_SIZE];
    static uint32_t y[BV_WARP_SIZE];
    static uint32_t exp[BV_WARP_SIZE];
    const uint32_t a = 4;
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        x[l] = (uint32_t)(l * 3 + 1);
        y[l] = (uint32_t)(l + 7);
        exp[l] = a * x[l] + y[l];
    }

    bvrt_device *dev = bvrt_create(1 << 20);
    CHECK(dev);
    CHECK(bvrt_load(dev, img) == 0);

    /* x[] then y[] placed at offsets 0 and 32*4 */
    size_t off = 0;
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        uint32_t v = x[l];
        memcpy(TEST_MEM(dev) + off, &v, 4);
        off += 4;
    }
    size_t yoff = off;
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        uint32_t v = y[l];
        memcpy(TEST_MEM(dev) + off, &v, 4);
        off += 4;
    }

    uint32_t args[3] = { 0, (uint32_t)yoff, a };
    CHECK(bvrt_launch(dev, "saxpy", args, 3) == 0);
    enum bv_status st = bvrt_run(dev, 10000);
    CHECK(st == BV_HALT);

    for (int l = 0; l < BV_WARP_SIZE; l++) {
        uint32_t got;
        memcpy(&got, TEST_MEM(dev) + (size_t)(yoff + l * 4), 4);
        CHECK(got == exp[l]);
        if (trace)
            printf("  lane %2d: y=%u (exp %u)\n", l, got, exp[l]);
    }

    bvrt_destroy(dev);
}

static void test_bvrt_saxpy(void)
{
    uint64_t insns[256];
    struct bvbc_image img;
    assemble_saxpy(&img, insns);
    run_saxpy(&img, 0);
    printf("  BVRT saxpy ok (32 lanes)\n");
}

/* MOV must be a bit copy, not a float add: ADDi 0 canonicalises -0.0 and
 * can quiet a NaN.  SEL must read its condition from FLAGS, not reuse PRED
 * as both execution mask and select condition.  Both are pinned here by
 * executing real kernels, including a divergent select. */
static const char bitexact_src[] =
    ".kernel bitexact\n"
    "MOV S1, S16\n"
    "STORE S17, S1, 0\n"
    "MOV S2, S18\n"
    "STORE S17, S2, 4\n"
    "EXIT\n"
    ".end\n";

static const char select_src[] =
    ".kernel sel\n"
    "IADD S2, S1, S1\n"        /* lane*2 */
    "IADD S2, S2, S2\n"        /* lane*4 */
    "IADD S3, S20, S2\n"       /* &out[lane] */
    "CMP P0, S1, S16\n"        /* lane == 16 */
    "SEL S4, S18, S19, P0\n"   /* P0 ? 0x11111111 : 0x22222222 */
    "STORE S3, S4, 0\n"
    "EXIT\n"
    ".end\n";

static void test_bitexact_and_select(void)
{
    bvrt_device *dev = bvrt_create(1 << 20);
    CHECK(dev);
    memset(TEST_MEM(dev), 0, 1 << 20);

    uint64_t insns[256];
    struct bvbc_image img;
    memset(&img, 0, sizeof(img));
    img.insns = insns;
    struct bradc_error err;
    CHECK(bradc_assemble(bitexact_src, &img, &err) == 0);

    /* Pin the encoding: MOV is OR with the hardwired zero. */
    struct bv_insn mv = bv_decode(img.insns[0]);
    CHECK(mv.op == BV_OP_OR);
    CHECK(mv.dst == 1 && mv.src1 == 16 && mv.src2 == BV_S_ZERO);

    CHECK(bvrt_load(dev, &img) == 0);
    uint32_t args[3] = { 0x80000000u, 0, 0x7FC00001u };
    CHECK(bvrt_launch(dev, "bitexact", args, 3) == 0);
    CHECK(bvrt_run(dev, 1000) == BV_HALT);
    uint32_t got[2];
    memcpy(got, TEST_MEM(dev), sizeof(got));
    CHECK(got[0] == 0x80000000u);   /* -0.0 survives */
    CHECK(got[1] == 0x7FC00001u);   /* NaN payload survives */

    memset(&img, 0, sizeof(img));
    img.insns = insns;
    CHECK(bradc_assemble(select_src, &img, &err) == 0);
    CHECK(bvrt_load(dev, &img) == 0);

    uint32_t sargs[5] = { 16, 0, 0x11111111u, 0x22222222u, 0x100 };
    CHECK(bvrt_launch(dev, "sel", sargs, 5) == 0);
    CHECK(bvrt_run(dev, 1000) == BV_HALT);
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        uint32_t v = 0;
        memcpy(&v, TEST_MEM(dev) + 0x100 + (size_t)l * 4, 4);
        uint32_t want = (l == 16) ? 0x11111111u : 0x22222222u;
        CHECK(v == want);
    }

    bvrt_destroy(dev);
    printf("  BVRT bit-exact MOV and divergent SEL ok\n");
}

static void test_bradgdb(void)
{
    uint64_t insns[256];
    struct bvbc_image img;
    assemble_saxpy(&img, insns);

    bvrt_device *dev = bvrt_create(1 << 20);
    CHECK(dev);
    CHECK(bvrt_load(dev, &img) == 0);
    memset(TEST_MEM(dev), 0, 1 << 20);

    uint32_t xstart = 0, ystart = 4 * BV_WARP_SIZE;
    for (int l = 0; l < BV_WARP_SIZE; l++) {
        uint32_t v = (uint32_t)(l * 2);
        memcpy(TEST_MEM(dev) + (size_t)xstart + l * 4, &v, 4);
        memcpy(TEST_MEM(dev) + (size_t)ystart + l * 4, &v, 4);
    }
    uint32_t args[3] = { xstart, ystart, 2 };
    CHECK(bvrt_launch(dev, "saxpy", args, 3) == 0);

    struct bradgdb g;
    CHECK(bradgdb_attach(&g, dev) == 0);
    int b1 = bradgdb_add_break(&g, 4);   /* IMUL S4, S3, S18 */
    CHECK(b1 >= 0);

    CHECK(bradgdb_continue(&g) == BV_OK);   /* parked on IMUL */
    CHECK(bradgdb_pc(&g) == 4);

    uint32_t x0 = bradgdb_read_s(&g, 0, 3);
    CHECK(x0 == 0);               /* lane 0: x[0] = 0 */

    CHECK(bradgdb_step(&g) == BV_OK);       /* IMUL */
    CHECK(bradgdb_pc(&g) == 5);
    uint32_t p = bradgdb_read_s(&g, 0, 4);
    CHECK(p == 0);                /* 2*0 */

    for (int i = 0; i < 5; i++)
        CHECK(bradgdb_step(&g) == BV_OK);
    CHECK(bradgdb_step(&g) == BV_HALT);     /* EXIT */

    bvrt_destroy(dev);
}

static void test_bradtimeline(void)
{
    uint64_t insns[256];
    struct bvbc_image img;
    assemble_saxpy(&img, insns);

    bvrt_device *dev = bvrt_create(1 << 20);
    CHECK(dev);
    bvrt_load(dev, &img);
    memset(TEST_MEM(dev), 0, 1 << 20);
    uint32_t args[3] = { 0, 4 * BV_WARP_SIZE, 3 };
    CHECK(bvrt_launch(dev, "saxpy", args, 3) == 0);

    struct bvtl *tl = bvtl_create(dev);
    CHECK(tl);
    enum bv_status st = bvtl_run(tl, 10000);
    CHECK(st == BV_HALT);
    CHECK(tl->nevents == SAXPY_NINS);
    CHECK(tl->insns == SAXPY_NINS);
    CHECK(tl->op_hist[BV_OP_IMUL] == 1);
    CHECK(tl->op_hist[BV_OP_EXIT] == 1);

    /* first event: IADD with all 32 lanes active */
    CHECK(tl->events[0].op == BV_OP_IADD);
    CHECK(tl->events[0].active_lanes == BV_WARP_SIZE);

    CHECK(bvtl_export_csv(tl, "timeline.csv") == 0);
    {
        FILE *f = fopen("timeline.csv", "r");
        CHECK(f);
        int rows = 0;
        char line[128];
        while (fgets(line, sizeof(line), f))
            rows++;
        fclose(f);
        unlink("timeline.csv");
        CHECK(rows == SAXPY_NINS + 1);   /* header + events */
    }

    bvtl_destroy(tl);
    bvrt_destroy(dev);
}

int main(void)
{
    test_assemble_vector();
    test_bvbc_roundtrip();
    test_bvrt_saxpy();
    test_bitexact_and_select();
    test_bradgdb();
    test_bradtimeline();
    printf("BradVector tests passed\n");
    return 0;
}