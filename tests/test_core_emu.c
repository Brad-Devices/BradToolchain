#include "brad/bradisa.h"
#include "brad_core_emu.h"
#include "brad_cache_emu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ADD(rd, rs1, rs2)  BRAD_RRR(BRAD_OP_ADD, rd, rs1, rs2)
#define SUB(rd, rs1, rs2)  BRAD_RRR(BRAD_OP_SUB, rd, rs1, rs2)
#define MUL(rd, rs1, rs2)  BRAD_RRR(BRAD_OP_MUL, rd, rs1, rs2)
#define ADDI(rd, rs1, imm) BRAD_RI(BRAD_OP_ADDI, rd, rs1, imm)
#define LDW(rd, rs1, imm)  BRAD_RI(BRAD_OP_LDW, rd, rs1, imm)
#define STW(rs2, rs1, imm) ((BRAD_OP_STW << 28) | ((rs1) << 20) | ((rs2) << 16) | ((imm) & 0xFFFF))
#define BZ(rs1, imm)       BRAD_BR(BRAD_OP_BZ, rs1, imm)
#define BNZ(rs1, imm)      BRAD_BR(BRAD_OP_BNZ, rs1, imm)
#define JMP(imm)           BRAD_JMP(BRAD_OP_JMP, imm)
#define CALL(imm)          BRAD_JMP(BRAD_OP_CALL, imm)
#define RET()              BRAD_RET_RAW

static void test_disasm(void)
{
    char buf[64];
    brad_core_emu_disasm(ADDI(1, 1, 5), buf, sizeof(buf));
    assert(strstr(buf, "ADDI") != NULL);
}

static void test_simple_program(void)
{
    struct brad_soc_emu soc;
    assert(brad_soc_emu_init(&soc, 1, 1, 1) == 0);

    uint32_t prog[] = {
        ADDI(1, 1, 5),
        ADDI(2, 2, 3),
        ADD(0, 1, 2),
        RET(),
    };

    assert(brad_core_emu_load(&soc, prog, 4, 0) == 0);
    brad_core_emu_init(&soc.cores[0][0], BRAD_PIPE_PHOENIX, 0, 0);
    brad_core_emu_run(&soc, 0, 0, 100);

    struct brad_core_state *core = &soc.cores[0][0];
    assert(core->running == 0);
    assert(core->regs[1] == 5);
    assert(core->regs[2] == 3);
    assert(core->regs[0] == 8);
    assert(core->insn_retired >= 3);
    assert(core->cycles > 0);

    brad_soc_emu_free(&soc);
}

static void test_falcon_execution(void)
{
    struct brad_soc_emu soc;
    assert(brad_soc_emu_init(&soc, 1, 0, 2) == 0);

    uint32_t prog[] = {
        ADDI(1, 1, 10),
        ADDI(2, 2, 20),
        ADD(0, 1, 2),
        RET(),
    };

    assert(brad_core_emu_load(&soc, prog, 4, 0) == 0);

    for (unsigned i = 0; i < 2; i++)
        brad_core_emu_init(&soc.cores[0][i], BRAD_PIPE_FALCON, 0, 0);

    brad_core_emu_run(&soc, 0, 0, 100);
    brad_core_emu_run(&soc, 0, 1, 100);

    assert(soc.cores[0][0].regs[0] == 30);
    assert(soc.cores[0][1].regs[0] == 30);
    assert(soc.cores[0][0].model == BRAD_PIPE_FALCON);

    brad_soc_emu_free(&soc);
}

static void test_memory_ops(void)
{
    struct brad_soc_emu soc;
    assert(brad_soc_emu_init(&soc, 1, 1, 0) == 0);

    uint32_t prog[] = {
        ADDI(1, 1, 128),
        ADDI(2, 2, 66),
        STW(2, 1, 0),
        LDW(3, 1, 0),
        ADD(3, 3, 2),
        RET(),
    };

    assert(brad_core_emu_load(&soc, prog, 6, 0) == 0);
    brad_core_emu_init(&soc.cores[0][0], BRAD_PIPE_PHOENIX, 0, 0);
    brad_core_emu_run(&soc, 0, 0, 200);

    assert(soc.cores[0][0].regs[3] == 132);

    brad_soc_emu_free(&soc);
}

static void test_branch_ops(void)
{
    struct brad_soc_emu soc;
    assert(brad_soc_emu_init(&soc, 1, 1, 0) == 0);

    uint32_t prog[] = {
        ADDI(1, 1, 1),
        ADDI(2, 2, 2),
        BZ(1, 2),
        ADDI(3, 3, 3),
        BNZ(2, 1),
        ADDI(4, 4, 4),
        CALL(0),
        RET(),
    };

    assert(brad_core_emu_load(&soc, prog, 8, 0) == 0);
    brad_core_emu_init(&soc.cores[0][0], BRAD_PIPE_PHOENIX, 0, 0);
    brad_core_emu_run(&soc, 0, 0, 200);

    struct brad_core_state *core = &soc.cores[0][0];
    assert(core->regs[1] == 1);
    assert(core->regs[2] == 2);
    assert(core->regs[3] == 3);
    assert(core->regs[4] == 0);
    assert(core->branches >= 2);
    assert(core->branches_taken >= 1);

    brad_soc_emu_free(&soc);
}

static void test_multi_core_run_all(void)
{
    struct brad_soc_emu soc;
    assert(brad_soc_emu_init(&soc, 2, 1, 1) == 0);

    uint32_t prog[] = {
        ADDI(1, 1, 42),
        RET(),
    };

    assert(brad_core_emu_load(&soc, prog, 2, 0) == 0);

    for (unsigned c = 0; c < 2; c++)
        for (unsigned i = 0; i < 2; i++)
            brad_core_emu_init(&soc.cores[c][i],
                                i == 0 ? BRAD_PIPE_PHOENIX : BRAD_PIPE_FALCON,
                                0, 0);

    brad_core_emu_run_all(&soc, 500);

    for (unsigned c = 0; c < 2; c++)
        for (unsigned i = 0; i < 2; i++) {
            assert(soc.cores[c][i].running == 0);
            assert(soc.cores[c][i].cycles > 0);
            assert(soc.cores[c][i].insn_retired >= 1);
        }

    assert(soc.total_cycles > 0);
    brad_soc_emu_free(&soc);
}

static void test_cache_hierarchy(void)
{
    struct brad_cache_hierarchy h;
    assert(brad_hierarchy_init(&h, 1, 2) == 0);

    uint32_t data = 0xDEADBEEF;
    assert(brad_hierarchy_write(&h, 0, 0, 0x1000, &data, 4) == 4);

    uint32_t readback = 0;
    assert(brad_hierarchy_read(&h, 0, 0, 1, 0x1000, &readback, 4) == 4);
    assert(readback == 0xDEADBEEF);

    uint64_t th, tm;
    brad_hierarchy_get_stats(&h, &th, &tm);

    struct brad_cache *l1d = &h.l1d[0][0];
    assert(l1d->hits > 0);
}

static void test_pipeline_depth(void)
{
    struct brad_core_state phx, fal;
    brad_core_emu_init(&phx, BRAD_PIPE_PHOENIX, 0, 0);
    brad_core_emu_init(&fal, BRAD_PIPE_FALCON, 0, 0);

    assert(phx.pipeline.depth == 10);
    assert(fal.pipeline.depth == 5);
    assert(phx.model == BRAD_PIPE_PHOENIX);
    assert(fal.model == BRAD_PIPE_FALCON);
}

int main(void)
{
    test_disasm();
    test_simple_program();
    test_falcon_execution();
    test_memory_ops();
    test_branch_ops();
    test_multi_core_run_all();
    test_cache_hierarchy();
    test_pipeline_depth();

    printf("BradCore Gen1 emulator tests passed\n");
    return 0;
}
