/* test_bradcvt.c — Brad-CVT end-to-end tests.
 *
 * Every positive case is real PTX text: translate it to a .bvbc image,
 * unpack and load that image into BVRT, launch one warp, run to halting,
 * and compare every lane's memory against a scalar CPU model.  That makes
 * the test load-bearing in both directions: a translator that emits a
 * plausible-looking but wrong instruction fails on the data, and a
 * translator that returns success without an image fails before it runs.
 *
 * The negative cases assert an exact status, a NULL image and zero length.
 * Returning OK with no output was the old stub's failure mode; it must not
 * be possible to reintroduce it quietly.
 */
#define _POSIX_C_SOURCE 200809L

#include "brad/bradcvt.h"
#include "brad/bvrt.h"
#include "brad/bvbc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        failures++;                                                          \
    }                                                                        \
} while (0)

static uint32_t bf(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static uint32_t *mem32(void *base, unsigned off)
{
    return (uint32_t *)((uint8_t *)base + off);
}

/* ─── PTX corpus ─── */

static const char PTX_SAXPY[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    "\n"
    ".visible .entry saxpy(\n"
    "    .param .u64 saxpy_param_0,\n"
    "    .param .u64 saxpy_param_1,\n"
    "    .param .f32 saxpy_param_2,\n"
    "    .param .u32 saxpy_param_3\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<5>;\n"
    "    .reg .b32 %r<3>;\n"
    "    .reg .b64 %rd<7>;\n"
    "\n"
    "    ld.param.u64 %rd1, [saxpy_param_0];\n"
    "    ld.param.u64 %rd2, [saxpy_param_1];\n"
    "    ld.param.f32 %f1, [saxpy_param_2];\n"
    "    ld.param.u32 %r1, [saxpy_param_3];\n"
    "    mov.u32 %r2, %tid.x;\n"
    "    setp.ge.u32 %p1, %r2, %r1;\n"
    "    @%p1 bra $L__BB0_2;\n"
    "    mul.wide.u32 %rd3, %r2, 4;\n"
    "    add.s64 %rd4, %rd1, %rd3;\n"
    "    add.s64 %rd5, %rd2, %rd3;\n"
    "    ld.global.f32 %f2, [%rd4];\n"
    "    ld.global.f32 %f3, [%rd5];\n"
    "    fma.rn.f32 %f4, %f2, %f1, %f3;\n"
    "    st.global.f32 [%rd5], %f4;\n"
    "$L__BB0_2:\n"
    "    ret;\n"
    "}\n";

static const char PTX_RELU[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry relu(\n"
    "    .param .u64 relu_param_0,\n"
    "    .param .u32 relu_param_1\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<3>;\n"
    "    .reg .b32 %r<3>;\n"
    "    .reg .b64 %rd<5>;\n"
    "    ld.param.u64 %rd1, [relu_param_0];\n"
    "    ld.param.u32 %r1, [relu_param_1];\n"
    "    mov.u32 %r2, %tid.x;\n"
    "    setp.ge.u32 %p1, %r2, %r1;\n"
    "    @%p1 bra $L__BB1_2;\n"
    "    mul.wide.u32 %rd2, %r2, 4;\n"
    "    add.s64 %rd3, %rd1, %rd2;\n"
    "    ld.global.f32 %f1, [%rd3];\n"
    "    setp.lt.f32 %p1, %f1, 0f00000000;\n"
    "    selp.f32 %f2, 0f00000000, %f1, %p1;\n"
    "    st.global.f32 [%rd3], %f2;\n"
    "$L__BB1_2:\n"
    "    ret;\n"
    "}\n";

static const char PTX_LOOP4[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry loop4(\n"
    "    .param .u64 loop4_param_0\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<2>;\n"
    "    .reg .b32 %r<3>;\n"
    "    .reg .b64 %rd<4>;\n"
    "    ld.param.u64 %rd1, [loop4_param_0];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd2, %r1, 4;\n"
    "    add.s64 %rd3, %rd1, %rd2;\n"
    "    mov.f32 %f1, 0f00000000;\n"
    "    mov.u32 %r2, 0;\n"
    "$L__BB2_1:\n"
    "    setp.lt.u32 %p1, %r2, 4;\n"
    "    @!%p1 bra $L__BB2_2;\n"
    "    add.f32 %f1, %f1, 0f3F800000;\n"
    "    add.u32 %r2, %r2, 1;\n"
    "    bra $L__BB2_1;\n"
    "$L__BB2_2:\n"
    "    st.global.f32 [%rd3], %f1;\n"
    "    ret;\n"
    "}\n";

/* 0.5 is not an integer, so this exercises the arbitrary-bit-pattern
 * constant path rather than the one-instruction ADDi fast path. */
static const char PTX_SCALE[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry scale(\n"
    "    .param .u64 scale_param_0\n"
    ")\n"
    "{\n"
    "    .reg .f32 %f<3>;\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .b64 %rd<4>;\n"
    "    ld.param.u64 %rd1, [scale_param_0];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd2, %r1, 4;\n"
    "    add.s64 %rd3, %rd1, %rd2;\n"
    "    ld.global.f32 %f1, [%rd3];\n"
    "    mul.f32 %f2, %f1, 0f3F000000;\n"
    "    st.global.f32 [%rd3], %f2;\n"
    "    ret;\n"
    "}\n";

static const char PTX_SIGN[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry sign(\n"
    "    .param .u64 sign_param_0,\n"
    "    .param .s32 sign_param_1\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<2>;\n"
    "    .reg .b32 %r<3>;\n"
    "    .reg .b64 %rd<4>;\n"
    "    ld.param.u64 %rd1, [sign_param_0];\n"
    "    ld.param.s32 %r1, [sign_param_1];\n"
    "    mov.u32 %r2, %tid.x;\n"
    "    mul.wide.u32 %rd2, %r2, 4;\n"
    "    add.s64 %rd3, %rd1, %rd2;\n"
    "    setp.lt.s32 %p1, %r1, 0;\n"
    "    selp.f32 %f1, 0f3F800000, 0f00000000, %p1;\n"
    "    st.global.f32 [%rd3], %f1;\n"
    "    ret;\n"
    "}\n";

/* %tid.x is S1 and %ctaid.x is 0 in the one-warp model.  %ntid.x and
 * %nctaid.x do not exist as registers; they are materialised as constants. */
static const char PTX_DIMS[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry dims(\n"
    "    .param .u64 dims_param_0,\n"
    "    .param .u64 dims_param_1,\n"
    "    .param .u64 dims_param_2\n"
    ")\n"
    "{\n"
    "    .reg .b32 %r<5>;\n"
    "    .reg .b64 %rd<8>;\n"
    "    ld.param.u64 %rd1, [dims_param_0];\n"
    "    ld.param.u64 %rd2, [dims_param_1];\n"
    "    ld.param.u64 %rd3, [dims_param_2];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd4, %r1, 4;\n"
    "    add.s64 %rd5, %rd1, %rd4;\n"
    "    mov.u32 %r2, %ntid.x;\n"
    "    st.global.u32 [%rd5], %r2;\n"
    "    add.s64 %rd6, %rd2, %rd4;\n"
    "    mov.u32 %r3, %nctaid.x;\n"
    "    st.global.u32 [%rd6], %r3;\n"
    "    add.s64 %rd7, %rd3, %rd4;\n"
    "    mov.u32 %r4, %ctaid.x;\n"
    "    st.global.u32 [%rd7], %r4;\n"
    "    ret;\n"
    "}\n";

/* Rung 1: a 64-bit load whose two words are distinct, stored back to a
 * different buffer.  A codegen that swapped the halves, or copied only the
 * low word twice, fails on the data. */
static const char PTX_LD64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry ld64(\n"
    "    .param .u64 ld64_param_0,\n"
    "    .param .u64 ld64_param_1\n"
    ")\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .b64 %rd<6>;\n"
    "    ld.param.u64 %rd1, [ld64_param_0];\n"
    "    ld.param.u64 %rd2, [ld64_param_1];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd3, %r1, 8;\n"
    "    add.s64 %rd4, %rd1, %rd3;\n"
    "    add.s64 %rd5, %rd2, %rd3;\n"
    "    ld.global.u64 %rd3, [%rd4];\n"
    "    mov.u64 %rd2, %rd3;\n"
    "    st.global.u64 [%rd5], %rd2;\n"
    "    ret;\n"
    "}\n";

/* Rung 1: a 64-bit store whose value came from a parameter load.  The
 * parameter is a 32-bit address, so the high word must be zero-extended,
 * not left undefined. */
static const char PTX_ST64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry st64(\n"
    "    .param .u64 st64_param_0,\n"
    "    .param .u64 st64_param_1\n"
    ")\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .b64 %rd<4>;\n"
    "    ld.param.u64 %rd1, [st64_param_0];\n"
    "    ld.param.u64 %rd2, [st64_param_1];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd3, %r1, 8;\n"
    "    add.s64 %rd1, %rd1, %rd3;\n"
    "    st.global.u64 [%rd1], %rd2;\n"
    "    ret;\n"
    "}\n";

/* Rung 1: non-zero static offsets on both sides.  The effective address is
 * materialised so the high word can use the +4 immediate. */
static const char PTX_OFF64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry off64(\n"
    "    .param .u64 off64_param_0,\n"
    "    .param .u64 off64_param_1\n"
    ")\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .b64 %rd<6>;\n"
    "    ld.param.u64 %rd1, [off64_param_0];\n"
    "    ld.param.u64 %rd2, [off64_param_1];\n"
    "    mov.u32 %r1, %tid.x;\n"
    "    mul.wide.u32 %rd3, %r1, 8;\n"
    "    add.s64 %rd4, %rd1, %rd3;\n"
    "    add.s64 %rd5, %rd2, %rd3;\n"
    "    ld.global.u64 %rd3, [%rd4+8];\n"
    "    st.global.u64 [%rd5+16], %rd3;\n"
    "    ret;\n"
    "}\n";

/* ─── Negative corpus ─── */

static const char PTX_BAD_SHARED[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_shared(.param .u64 p)\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .f32 %f<2>;\n"
    "    ld.shared.f32 %f1, [%r1];\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_TIDY[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_tidy()\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    mov.u32 %r1, %tid.y;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_MULTI[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry k1()\n"
    "{\n"
    "    ret;\n"
    "}\n"
    ".visible .entry k2()\n"
    "{\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_CVT[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_cvt(.param .u32 p)\n"
    "{\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .f32 %f<2>;\n"
    "    ld.param.u32 %r1, [p];\n"
    "    cvt.rn.f32.s32 %f1, %r1;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_F64LD[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_f64ld(.param .u64 p)\n"
    "{\n"
    "    .reg .b64 %rd<3>;\n"
    "    ld.param.u64 %rd1, [p];\n"
    "    ld.global.f64 %rd2, [%rd1];\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_PRED_RET[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_pred_ret(.param .u32 p)\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .b32 %r<2>;\n"
    "    ld.param.u32 %r1, [p];\n"
    "    setp.ne.u32 %p1, %r1, 0;\n"
    "    @%p1 ret;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_MOV_PRED[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_mov_pred()\n"
    "{\n"
    "    .reg .pred %p<3>;\n"
    "    mov.pred %p1, %p2;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_IMM64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_imm64()\n"
    "{\n"
    "    .reg .b64 %rd<2>;\n"
    "    mov.b64 %rd1, 0x100000000;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_SETP64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_setp64(.param .u64 p)\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .b64 %rd<3>;\n"
    "    ld.param.u64 %rd1, [p];\n"
    "    add.s64 %rd2, %rd1, %rd1;\n"
    "    setp.lt.s64 %p1, %rd1, %rd2;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_AND64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_and64(.param .u64 p)\n"
    "{\n"
    "    .reg .b64 %rd<3>;\n"
    "    ld.param.u64 %rd1, [p];\n"
    "    and.b64 %rd2, %rd1, %rd1;\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_PRED64[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry bad_pred64(.param .u64 p, .param .u32 q)\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .b32 %r<2>;\n"
    "    .reg .b64 %rd<3>;\n"
    "    ld.param.u64 %rd1, [p];\n"
    "    ld.param.u32 %r1, [q];\n"
    "    setp.ne.u32 %p1, %r1, 0;\n"
    "    @%p1 ld.global.u64 %rd2, [%rd1];\n"
    "    ret;\n"
    "}\n";

static const char PTX_BAD_SYNTAX[] =
    ".version 7.8\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry broken(\n";

/* ─── Harness ─── */

struct run {
    void *blob;
    size_t len;
    struct bvbc_image img;
    bvrt_device *dev;
};

static int run_load(struct run *r, const char *ptx, const char *name,
                    unsigned nargs)
{
    memset(r, 0, sizeof(*r));

    int st = brad_cvt_translate_ptx(ptx, strlen(ptx), &r->blob, &r->len);
    if (st != BRAD_CVT_OK) {
        fprintf(stderr, "%s: translate failed (%d): %s\n",
                name, st, brad_cvt_last_error());
        return -1;
    }
    if (!r->blob || r->len < 32 ||
        memcmp(r->blob, "BVBC0001", 8) != 0) {
        fprintf(stderr, "%s: translator produced no valid image\n", name);
        return -1;
    }
    if (bvbc_unpack((const uint8_t *)r->blob, r->len, &r->img) != 0) {
        fprintf(stderr, "%s: bvbc_unpack failed\n", name);
        return -1;
    }
    if (r->img.nkernels != 1 ||
        strcmp(r->img.kernels[0].name, name) != 0 ||
        r->img.kernels[0].nargs != nargs) {
        fprintf(stderr, "%s: container metadata is wrong "
                "(kernels=%u name='%s' nargs=%u)\n",
                name, r->img.nkernels, r->img.kernels[0].name,
                r->img.kernels[0].nargs);
        return -1;
    }

    r->dev = bvrt_create(4096);
    if (!r->dev || bvrt_load(r->dev, &r->img) != 0) {
        fprintf(stderr, "%s: BVRT load failed\n", name);
        return -1;
    }
    return 0;
}

static void run_ok(struct run *r, const char *what)
{
    enum bv_status st = bvrt_run(r->dev, 100000);
    if (st != BV_HALT) {
        fprintf(stderr, "%s: BVRT status %d at pc %u\n", what, (int)st,
                bvrt_warp(r->dev)->pc);
    }
    CHECK(st == BV_HALT);
}

static void run_free(struct run *r)
{
    bvrt_destroy(r->dev);
    brad_cvt_free(r->blob);
}

static void expect_fail(const char *ptx, int want, const char *what)
{
    void *bin = (void *)0x1;
    size_t len = 999;

    int st = brad_cvt_translate_ptx(ptx, strlen(ptx), &bin, &len);
    if (st != want) {
        fprintf(stderr, "%s: got %d (%s), want %d\n", what, st,
                brad_cvt_last_error(), want);
    }
    CHECK(st == want);
    CHECK(bin == NULL);
    CHECK(len == 0);
    CHECK(brad_cvt_last_error()[0] != '\0');
}

/* ─── Positive tests ─── */

static void test_saxpy(void)
{
    struct run r;
    if (run_load(&r, PTX_SAXPY, "saxpy", 4) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *x = mem32(bvrt_mem(r.dev), 0x100);
    uint32_t *y = mem32(bvrt_mem(r.dev), 0x200);
    for (int i = 0; i < 32; i++) {
        x[i] = bf((float)i);
        y[i] = bf(100.0f + (float)i);
    }

    uint32_t args[4] = { 0x100, 0x200, bf(2.0f), 32 };
    CHECK(bvrt_launch(r.dev, "saxpy", args, 4) == 0);
    run_ok(&r, "saxpy");

    for (int i = 0; i < 32; i++) {
        uint32_t want = bf(2.0f * (float)i + 100.0f + (float)i);
        if (y[i] != want) {
            fprintf(stderr, "saxpy lane %d: got 0x%08x want 0x%08x\n",
                    i, y[i], want);
        }
        CHECK(y[i] == want);
    }
    run_free(&r);
}

static void test_relu(void)
{
    struct run r;
    if (run_load(&r, PTX_RELU, "relu", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *v = mem32(bvrt_mem(r.dev), 0x300);
    float in[32];
    for (int i = 0; i < 32; i++) {
        switch (i) {
        case 0:  in[i] = -3.5f;  break;
        case 1:  in[i] = -1.0f;  break;
        case 2:  in[i] = -0.0f;  break;   /* must survive as -0.0 */
        case 3:  in[i] = 0.0f;   break;
        case 4:  in[i] = 2.25f;  break;
        case 5:  in[i] = -7.0f;  break;
        default: in[i] = (float)(i - 16); break;
        }
        v[i] = bf(in[i]);
    }

    uint32_t args[2] = { 0x300, 32 };
    CHECK(bvrt_launch(r.dev, "relu", args, 2) == 0);
    run_ok(&r, "relu");

    for (int i = 0; i < 32; i++) {
        /* Branch-style ReLU: x < 0 ? +0.0 : x, so -0.0 passes through
         * unchanged.  If float_key failed to normalise -0.0, lane 2 would
         * come back as +0.0 and this would fail. */
        uint32_t want = (in[i] < 0.0f) ? 0u : bf(in[i]);
        if (v[i] != want) {
            fprintf(stderr, "relu lane %d: got 0x%08x want 0x%08x\n",
                    i, v[i], want);
        }
        CHECK(v[i] == want);
    }
    run_free(&r);
}

static void test_loop4(void)
{
    struct run r;
    if (run_load(&r, PTX_LOOP4, "loop4", 1) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *o = mem32(bvrt_mem(r.dev), 0x400);
    for (int i = 0; i < 32; i++)
        o[i] = 0xDEADBEEFu;

    uint32_t args[1] = { 0x400 };
    CHECK(bvrt_launch(r.dev, "loop4", args, 1) == 0);
    run_ok(&r, "loop4");

    for (int i = 0; i < 32; i++) {
        uint32_t want = bf(4.0f);
        if (o[i] != want) {
            fprintf(stderr, "loop4 lane %d: got 0x%08x want 0x%08x\n",
                    i, o[i], want);
        }
        CHECK(o[i] == want);
    }
    run_free(&r);
}

static void test_scale(void)
{
    struct run r;
    if (run_load(&r, PTX_SCALE, "scale", 1) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *s = mem32(bvrt_mem(r.dev), 0x500);
    for (int i = 0; i < 32; i++)
        s[i] = bf((float)i);

    uint32_t args[1] = { 0x500 };
    CHECK(bvrt_launch(r.dev, "scale", args, 1) == 0);
    run_ok(&r, "scale");

    for (int i = 0; i < 32; i++) {
        uint32_t want = bf((float)i * 0.5f);
        if (s[i] != want) {
            fprintf(stderr, "scale lane %d: got 0x%08x want 0x%08x\n",
                    i, s[i], want);
        }
        CHECK(s[i] == want);
    }
    run_free(&r);
}

static void test_sign(void)
{
    struct run r;
    if (run_load(&r, PTX_SIGN, "sign", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *o = mem32(bvrt_mem(r.dev), 0x600);
    uint32_t args[2] = { 0x600, (uint32_t)-5 };

    for (int i = 0; i < 32; i++)
        o[i] = 0;
    CHECK(bvrt_launch(r.dev, "sign", args, 2) == 0);
    run_ok(&r, "sign negative");
    for (int i = 0; i < 32; i++)
        CHECK(o[i] == bf(1.0f));

    for (int i = 0; i < 32; i++)
        o[i] = 0xDEADBEEFu;
    args[1] = 5;
    CHECK(bvrt_launch(r.dev, "sign", args, 2) == 0);
    run_ok(&r, "sign positive");
    for (int i = 0; i < 32; i++)
        CHECK(o[i] == bf(0.0f));

    run_free(&r);
}

static void test_dims(void)
{
    struct run r;
    if (run_load(&r, PTX_DIMS, "dims", 3) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *ntid   = mem32(bvrt_mem(r.dev), 0x700);
    uint32_t *nctaid = mem32(bvrt_mem(r.dev), 0x780);
    uint32_t *ctaid  = mem32(bvrt_mem(r.dev), 0x800);
    for (int i = 0; i < 32; i++) {
        ntid[i] = 0xFFFFFFFFu;
        nctaid[i] = 0xFFFFFFFFu;
        ctaid[i] = 0xFFFFFFFFu;
    }

    uint32_t args[3] = { 0x700, 0x780, 0x800 };
    CHECK(bvrt_launch(r.dev, "dims", args, 3) == 0);
    run_ok(&r, "dims");

    for (int i = 0; i < 32; i++) {
        CHECK(ntid[i] == 32);
        CHECK(nctaid[i] == 1);
        CHECK(ctaid[i] == 0);
    }
    run_free(&r);
}

/* ─── Rung 1: 64-bit load/store ─── */

static void test_ld64(void)
{
    struct run r;
    if (run_load(&r, PTX_LD64, "ld64", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *in  = mem32(bvrt_mem(r.dev), 0x100);
    uint32_t *out = mem32(bvrt_mem(r.dev), 0x200);
    for (int i = 0; i < 64; i++) {
        in[i] = (i & 1) ? (0x22220000u + (uint32_t)(i / 2))
                        : (0x11110000u + (uint32_t)(i / 2));
        out[i] = 0;
    }

    uint32_t args[2] = { 0x100, 0x200 };
    CHECK(bvrt_launch(r.dev, "ld64", args, 2) == 0);
    run_ok(&r, "ld64");

    for (int i = 0; i < 64; i++) {
        if (out[i] != in[i]) {
            fprintf(stderr, "ld64 word %d: got 0x%08x want 0x%08x\n",
                    i, out[i], in[i]);
        }
        CHECK(out[i] == in[i]);
    }
    run_free(&r);
}

static void test_st64(void)
{
    struct run r;
    if (run_load(&r, PTX_ST64, "st64", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *out = mem32(bvrt_mem(r.dev), 0x200);
    for (int i = 0; i < 64; i++)
        out[i] = 0xFFFFFFFFu;

    uint32_t args[2] = { 0x200, 0xDEADBEEFu };
    CHECK(bvrt_launch(r.dev, "st64", args, 2) == 0);
    run_ok(&r, "st64");

    for (int i = 0; i < 32; i++) {
        CHECK(out[i * 2] == 0xDEADBEEFu);
        CHECK(out[i * 2 + 1] == 0);
    }
    run_free(&r);
}

/* Rung 1: non-zero static offsets on both sides exercise effective-address
 * materialisation. */
static void test_off64(void)
{
    struct run r;
    if (run_load(&r, PTX_OFF64, "off64", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t *in  = mem32(bvrt_mem(r.dev), 0x100);
    uint32_t *out = mem32(bvrt_mem(r.dev), 0x400);
    for (int i = 0; i < 96; i++) {
        in[i] = 0x50000000u + (uint32_t)i;
        out[i] = 0;
    }
    uint32_t want[32][2];
    for (int i = 0; i < 32; i++) {
        want[i][0] = in[(i + 1) * 2];
        want[i][1] = in[(i + 1) * 2 + 1];
    }

    uint32_t args[2] = { 0x100, 0x400 };
    CHECK(bvrt_launch(r.dev, "off64", args, 2) == 0);
    run_ok(&r, "off64");

    /* lane i loads input element i+1 and stores it to output element i+2. */
    for (int i = 0; i < 32; i++) {
        CHECK(out[(i + 2) * 2]     == want[i][0]);
        CHECK(out[(i + 2) * 2 + 1] == want[i][1]);
    }
    run_free(&r);
}

/* An unaligned base must trap at run time; the translator cannot know the
 * address value, so the check is emitted into the kernel. */
static void test_ld64_unaligned(void)
{
    struct run r;
    if (run_load(&r, PTX_LD64, "ld64", 2) != 0) {
        run_free(&r);
        CHECK(0);
        return;
    }

    uint32_t args[2] = { 0x104, 0x200 };
    CHECK(bvrt_launch(r.dev, "ld64", args, 2) == 0);
    CHECK(bvrt_run(r.dev, 100000) == BV_TRAP);
    run_free(&r);
}

/* ─── AOT path ─── */
static void test_aot(void)
{
    char in_path[] = "/tmp/bradcvt_ptx_XXXXXX";
    int fd = mkstemp(in_path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;

    size_t n = strlen(PTX_SAXPY);
    CHECK(write(fd, PTX_SAXPY, n) == (ssize_t)n);
    CHECK(close(fd) == 0);

    char out_path[256];
    snprintf(out_path, sizeof(out_path), "%s.bvbc", in_path);

    struct brad_cvt_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = BRAD_CVT_AOT;
    cfg.input_path = in_path;
    cfg.output_path = out_path;

    struct brad_cvt_kernel_info info;
    int st = brad_cvt_compile(&cfg, &info);
    if (st != BRAD_CVT_OK)
        fprintf(stderr, "aot: %s\n", brad_cvt_last_error());
    CHECK(st == BRAD_CVT_OK);
    CHECK(strcmp(info.name, "saxpy") == 0);
    CHECK(info.num_regs > 0);
    CHECK(info.shared_mem_bytes == 0);
    CHECK(info.thread_count == 32);
    CHECK(info.block_count == 1);
    CHECK(info.instruction_count > 10);

    FILE *f = fopen(out_path, "rb");
    CHECK(f != NULL);
    if (f) {
        uint8_t hdr[32];
        CHECK(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
        CHECK(memcmp(hdr, "BVBC0001", 8) == 0);
        fclose(f);
    }
    (void)unlink(out_path);
    (void)unlink(in_path);
}

static void test_compile_missing_file(void)
{
    struct brad_cvt_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = BRAD_CVT_AOT;
    cfg.input_path = "/tmp/bradcvt_missing_file_12345.ptx";

    int st = brad_cvt_compile(&cfg, NULL);
    CHECK(st == BRAD_CVT_ERR_PARSE);
    CHECK(strstr(brad_cvt_last_error(), "read input") != NULL);
}

/* ─── Negative and API tests ─── */

static void test_rejections(void)
{
    expect_fail("", BRAD_CVT_ERR_PARSE, "empty source");
    expect_fail(PTX_BAD_SYNTAX, BRAD_CVT_ERR_PARSE, "unterminated entry");
    expect_fail(PTX_BAD_MULTI, BRAD_CVT_ERR_UNSUPPORTED, "multiple entries");
    expect_fail(PTX_BAD_SHARED, BRAD_CVT_ERR_UNSUPPORTED, "ld.shared");
    expect_fail(PTX_BAD_TIDY, BRAD_CVT_ERR_UNSUPPORTED, "%tid.y");
    expect_fail(PTX_BAD_CVT, BRAD_CVT_ERR_UNSUPPORTED, "int<->float cvt");
    expect_fail(PTX_BAD_F64LD, BRAD_CVT_ERR_UNSUPPORTED, "64-bit float load");
    expect_fail(PTX_BAD_PRED64, BRAD_CVT_ERR_UNSUPPORTED, "predicated 64-bit load");
    expect_fail(PTX_BAD_PRED_RET, BRAD_CVT_ERR_UNSUPPORTED, "predicated ret");
    expect_fail(PTX_BAD_MOV_PRED, BRAD_CVT_ERR_UNSUPPORTED, "mov.pred");
    expect_fail(PTX_BAD_IMM64, BRAD_CVT_ERR_UNSUPPORTED, "64-bit immediate");
    expect_fail(PTX_BAD_SETP64, BRAD_CVT_ERR_UNSUPPORTED, "64-bit setp");
    expect_fail(PTX_BAD_AND64, BRAD_CVT_ERR_UNSUPPORTED, "64-bit bitwise");
}

/* The IR stores names in 64-byte fields.  A longer identifier must be a
 * parse error, not a silent truncation that could alias another symbol. */
static void test_long_ident(void)
{
    char long_name[80];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    char src[512];
    snprintf(src, sizeof(src),
        ".version 7.8\n"
        ".target sm_70\n"
        ".address_size 64\n"
        ".visible .entry long_ident(.param .u32 %s)\n"
        "{\n"
        "    ret;\n"
        "}\n", long_name);

    expect_fail(src, BRAD_CVT_ERR_PARSE, "overlong parameter name");
}

/* The ISA branch immediate is 8 bits.  A translator that silently wraps it
 * would branch to the wrong instruction, so this must be a hard error. */
static void test_branch_range(void)
{
    size_t cap = 64 * 1024;
    char *src = malloc(cap);
    CHECK(src != NULL);
    if (!src)
        return;

    size_t n = 0;
    n += (size_t)snprintf(src + n, cap - n,
        ".version 7.8\n"
        ".target sm_70\n"
        ".address_size 64\n"
        ".visible .entry far(.param .u32 p)\n"
        "{\n"
        "    .reg .pred %%p<2>;\n"
        "    .reg .b32 %%r<3>;\n"
        "    ld.param.u32 %%r1, [p];\n"
        "    setp.ne.u32 %%p1, %%r1, 0;\n"
        "    @%%p1 bra $far_end;\n");
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(src + n, cap - n,
                              "    add.u32 %%r2, %%r2, 1;\n");
    n += (size_t)snprintf(src + n, cap - n,
        "$far_end:\n"
        "    ret;\n"
        "}\n");

    expect_fail(src, BRAD_CVT_ERR_TRANSLATION, "branch out of range");
    free(src);
}

static void test_api_guards(void)
{
    size_t len = 123;

    CHECK(brad_cvt_translate_ptx(PTX_SAXPY, strlen(PTX_SAXPY), NULL, &len)
          == BRAD_CVT_ERR_PARSE);
    CHECK(brad_cvt_translate_ptx(PTX_SAXPY, strlen(PTX_SAXPY), NULL, NULL)
          == BRAD_CVT_ERR_PARSE);

    void *bin = (void *)0x1;
    len = 999;
    CHECK(brad_cvt_translate_ptx(NULL, 0, &bin, &len)
          == BRAD_CVT_ERR_PARSE);
    CHECK(bin == NULL);
    CHECK(len == 0);
    CHECK(brad_cvt_last_error()[0] != '\0');

    void *cubin = (void *)0x1;
    size_t cubin_len = 99;
    CHECK(brad_cvt_translate_cubin("not a cubin", 11, &cubin, &cubin_len)
          == BRAD_CVT_ERR_UNSUPPORTED);
    CHECK(cubin == NULL);
    CHECK(cubin_len == 0);
    CHECK(strstr(brad_cvt_last_error(), "cubin") != NULL);

    CHECK(brad_cvt_jit_invoke(NULL, 0, NULL, 0)
          == BRAD_CVT_ERR_UNSUPPORTED);
    CHECK(strstr(brad_cvt_last_error(), "JIT") != NULL);

    CHECK(strcmp(brad_cvt_status_str(BRAD_CVT_OK), "OK") == 0);
    for (int s = 0; s >= -4; s--) {
        const char *str = brad_cvt_status_str(s);
        CHECK(str != NULL && str[0] != '\0');
    }
}

int main(void)
{
    test_saxpy();
    test_relu();
    test_loop4();
    test_scale();
    test_sign();
    test_dims();
    test_ld64();
    test_st64();
    test_off64();
    test_ld64_unaligned();
    test_aot();
    test_compile_missing_file();
    test_rejections();
    test_long_ident();
    test_branch_range();
    test_api_guards();

    if (failures) {
        fprintf(stderr, "bradcvt: %d check(s) failed\n", failures);
        return 1;
    }
    printf("bradcvt: all checks passed\n");
    return 0;
}
