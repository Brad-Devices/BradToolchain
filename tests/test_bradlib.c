/* test_bradlib.c — BradVector Platform API (bradlib) integration test.
 *
 * Exercises the stable host surface: compile, pack/round-trip,
 * disassembly, session lifecycle, launch/run, register inspection,
 * breakpoints + continue.  Verifies the same saxpy contract as
 * test_bradvector.c.
 */

#include "brad/bradlib.h"
#include "brad/bvbc.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
} while (0)

static const char saxpy_src[] =
    ".kernel saxpy\n"
    "IADD S2, S1, S1\n"      /* lane*2 */
    "IADD S2, S2, S2\n"      /* lane*4 */
    "IADD S7, S16, S2\n"     /* &x[i] */
    "LOAD S3, S7, 0\n"       /* x[i] */
    "IMUL S4, S3, S18\n"     /* a*x[i] */
    "IADD S8, S17, S2\n"     /* &y[i] */
    "LOAD S5, S8, 0\n"       /* y[i] */
    "IADD S6, S4, S5\n"      /* a*x[i]+y[i] */
    "STORE S8, S6, 0\n"      /* y[i] = ... */
    "EXIT\n"
    ".end\n";

#define WARP       (32u)
#define Y_OFF      (WARP * 4u)

int main(void)
{
    /* ── compile ── */
    struct bradlib_program *p = bradlib_compile(saxpy_src, NULL);
    CHECK(p != NULL, "compile failed");
    CHECK(bradlib_kernel_count(p) == 1, "kernel count");
    CHECK(strcmp(bradlib_kernel_name(p, 0), "saxpy") == 0, "kernel name");
    CHECK(bradlib_insn_count(p) == 10, "insn count");

    /* ── disassembly ── */
    char dis[128];
    CHECK(bradlib_disasm(p, 3, dis, sizeof(dis)) > 0, "disasm + len");
    CHECK(strstr(dis, "LOAD") != NULL, "disasm contains LOAD");
    CHECK(bradlib_disasm(p, 10, dis, sizeof(dis)) < 0, "disasm OOB rejects");

    /* ── symbols ── */
    struct bradc_symbol syms[8];
    size_t nsym = 0;
    CHECK(bradlib_symbols(p, syms, 8, &nsym) == 0, "symbols ok");

    /* ── pack round-trip ── */
    size_t cap = bradlib_serialize_size(p);
    CHECK(cap > 0, "serialize size");
    {
        uint8_t buf[512];
        CHECK(cap <= sizeof(buf), "container fits");
        CHECK(bradlib_pack(p, buf, cap) == cap, "pack writes full size");
        struct bvbc_image back;
        memset(&back, 0, sizeof(back));
        CHECK(bvbc_unpack(buf, cap, &back) == 0, "unpack round-trip");
        CHECK(back.ninsn == bradlib_insn_count(p), "round-trip ninsn");
        CHECK(back.nkernels == 1 &&
              strcmp(back.kernels[0].name, "saxpy") == 0, "round-trip kernel");
    }

    /* ── session + run ── */
    struct bradlib_session *s = bradlib_create(p, 1u << 20);
    CHECK(s != NULL, "create session");
    CHECK(bradlib_mem(s) != NULL, "host mem");

    uint8_t *m = (uint8_t *)bradlib_mem(s);
    size_t msz = bradlib_mem_size(s);
    CHECK(msz == (1u << 20), "mem size");

    for (int l = 0; l < (int)WARP; l++) {
        uint32_t v = (uint32_t)(l * 3 + 1);
        memcpy(m + (size_t)l * 4, &v, 4);
        v = (uint32_t)(l + 7);
        memcpy(m + Y_OFF + (size_t)l * 4, &v, 4);
    }

    uint32_t args[3] = { 0, Y_OFF, 4u };
    CHECK(bradlib_launch(s, "saxpy", args, 3) == 0, "launch");
    CHECK(bradlib_launch(s, "nope", args, 3) != 0, "unknown kernel rejects");

    CHECK(bradlib_run(s, 10000) == BV_HALT, "run halts");

    int ok = 1;
    for (int l = 0; l < (int)WARP; l++) {
        uint32_t got;
        memcpy(&got, m + Y_OFF + (size_t)l * 4, 4);
        uint32_t exp = 4u * (uint32_t)(l * 3 + 1) + (uint32_t)(l + 7);
        if (got != exp) ok = 0;
    }
    CHECK(ok, "saxpy results correct");

    /* ── single-step at breakpoint + register inspection ── */
    CHECK(bradlib_breakpoint(s, 4) >= 0, "breakpoint add");
    CHECK(bradlib_launch(s, "saxpy", args, 3) == 0, "relaunch");

    /* run past step 0..3 lands on breakpoint 4 (IMUL S4,S3,S18) */
    enum bv_status st = BV_OK;
    unsigned guard = 0;
    while (st == BV_OK && bradlib_pc(s) != 4 && guard++ < 64)
        st = bradlib_step(s);
    CHECK(bradlib_pc(s) == 4, "breakpoint pc = 4");
    CHECK(bradlib_read_s(s, 0, 3) == 1, "S3 = x[0] = 1");
    CHECK(bradlib_read_s(s, 0, 18) == 4, "S18 = a = 4");
    CHECK(bradlib_cycles(s) >= 4, "cycles counted");

    st = bradlib_run(s, 10000);
    CHECK(st == BV_HALT, "run to completion after breakpoint");

    /* ── junk args ── */
    CHECK(bradlib_read_s(s, 32, 0) == 0, "read_s rejects bad lane");
    CHECK(bradlib_read_v(s, 0, 0, 0) == 0, "read_v safe");
    CHECK(bradlib_read_p(s, 0, 0) == 0, "read_p safe");

    bradlib_destroy(s);
    bradlib_free_program(p);
    printf("bradlib tests passed\n");
    return 0;
}