#include "brad/bradisa.h"
#include "brad/bradcore.h"
#include "brad_core_emu.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

static uint32_t alu_exec(enum brad_isa_opcode op,
                          uint32_t a, uint32_t b)
{
    switch (op) {
    case BRAD_OP_ADD:  return a + b;
    case BRAD_OP_SUB:  return a - b;
    case BRAD_OP_MUL:  return a * b;
    case BRAD_OP_AND:  return a & b;
    case BRAD_OP_OR:   return a | b;
    case BRAD_OP_XOR:  return a ^ b;
    case BRAD_OP_SHL:  return a << (b & 0x1F);
    case BRAD_OP_SHR:  return a >> (b & 0x1F);
    default:           return 0;
    }
}

static void advance_pipeline(struct brad_core_state *core)
{
    struct brad_core_pipeline *p = &core->pipeline;
    for (int s = (int)p->depth - 1; s > 0; s--) {
        p->stage_insn[s] = p->stage_insn[s - 1];
        p->stage_pc[s]   = p->stage_pc[s - 1];
        p->stage_valid[s] = p->stage_valid[s - 1];
    }
    p->stage_insn[0] = NULL;
    p->stage_pc[0] = 0;
    p->stage_valid[0] = 0;
}

static void pipeline_flush(struct brad_core_state *core)
{
    struct brad_core_pipeline *p = &core->pipeline;
    for (unsigned s = 0; s < p->depth; s++) {
        if (s == p->exec_stage) continue;
        free(p->stage_insn[s]);
        p->stage_insn[s] = NULL;
        p->stage_valid[s] = 0;
        p->stage_pc[s] = 0;
    }
    free(p->stage_insn[p->exec_stage]);
    p->stage_insn[p->exec_stage] = NULL;
    p->stage_valid[p->exec_stage] = 0;
}

static void update_energy(struct brad_core_state *core)
{
    core->msr_energy += (uint64_t)core->msr_voltage *
                        (uint64_t)core->msr_freq / 1000000ULL;
}

static int execute(struct brad_core_state *core,
                    struct brad_isa_insn *d,
                    uint8_t *mem_base, size_t mem_size,
                    uint32_t insn_pc, int *taken)
{
    core->insn_retired++;
    *taken = 0;

    if (BRAD_IS_BRANCH(d->opcode) && d->opcode != BRAD_OP_RET)
        core->branches++;

    unsigned num_regs = (core->model == BRAD_PIPE_FALCON_LITE)
                        ? BRAD_NUM_REGS_LITE : BRAD_NUM_REGS;

    switch (d->opcode) {
    case BRAD_OP_ADD: case BRAD_OP_SUB: case BRAD_OP_MUL:
    case BRAD_OP_AND: case BRAD_OP_OR:  case BRAD_OP_XOR:
    case BRAD_OP_SHL: case BRAD_OP_SHR: {
        uint32_t a = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        uint32_t b = d->rs2 < num_regs ? core->regs[d->rs2] : 0;
        if (d->opcode == BRAD_OP_MUL && core->model == BRAD_PIPE_FALCON_LITE) {
            core->regs[d->rd] = 0;
            return 0;
        }
        uint32_t r = alu_exec(d->opcode, a, b);
        if (d->rd < num_regs) core->regs[d->rd] = r;
        return 0;
    }
    case BRAD_OP_ADDI: {
        uint32_t a = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        if (d->rd < num_regs) core->regs[d->rd] = a + d->immediate;
        return 0;
    }
    case BRAD_OP_LDW: {
        uint32_t base = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        uint32_t addr = base + d->immediate;
        if (addr + 4 > mem_size) return -EFAULT;
        uint32_t val;
        memcpy(&val, mem_base + addr, 4);
        if (d->rd < num_regs) core->regs[d->rd] = val;
        return 0;
    }
    case BRAD_OP_STW: {
        uint32_t base = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        uint32_t addr = base + d->immediate;
        if (addr + 4 > mem_size) return -EFAULT;
        uint32_t val = d->rs2 < num_regs ? core->regs[d->rs2] : 0;
        memcpy(mem_base + addr, &val, 4);
        return 0;
    }
    case BRAD_OP_BZ: {
        uint32_t val = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        if (val == 0) {
            core->pc = insn_pc + 4 + d->immediate;
            core->branches_taken++;
            *taken = 1;
        }
        return 0;
    }
    case BRAD_OP_BNZ: {
        uint32_t val = d->rs1 < num_regs ? core->regs[d->rs1] : 0;
        if (val != 0) {
            core->pc = insn_pc + 4 + d->immediate;
            core->branches_taken++;
            *taken = 1;
        }
        return 0;
    }
    case BRAD_OP_JMP: {
        core->pc = insn_pc + 4 + d->immediate;
        core->branches_taken++;
        *taken = 1;
        return 0;
    }
    case BRAD_OP_CALL: {
        if (BRAD_NUM_REGS > BRAD_REG_LR)
            core->regs[BRAD_REG_LR] = insn_pc + 4;
        core->pc = insn_pc + 4 + d->immediate;
        core->branches_taken++;
        *taken = 1;
        return 0;
    }
    case BRAD_OP_RET: {
        /* RET returns through the link register, not through whatever the
         * rs1 field happens to hold: BRAD_RET_RAW is 0xF0000000, whose rs1
         * field is zero.  A link register of zero means this frame was
         * entered without a CALL -- there is no caller to return to -- so
         * the core halts.  That is what lets a bare program end in RET. */
        uint32_t target = (BRAD_NUM_REGS > BRAD_REG_LR)
                          ? core->regs[BRAD_REG_LR] : 0;
        if (target == 0) {
            core->running = 0;
            return 0;
        }
        core->pc = target;
        core->branches_taken++;
        *taken = 1;
        return 0;
    }
    default:
        return 0;
    }
}

static int fetch(struct brad_core_state *core,
                  uint8_t *mem_base, size_t mem_size,
                  struct brad_isa_insn *d)
{
    int compressed = (core->msr_status & BRAD_STATUS_C) || core->compress_mode;
    if (compressed) {
        if (core->pc + 4 > mem_size)
            return -EFAULT;
        uint32_t raw_pair;
        memcpy(&raw_pair, mem_base + core->pc, 4);
        uint16_t lo = (uint16_t)(raw_pair & 0xFFFF);
        uint16_t hi = (uint16_t)(raw_pair >> 16);
        struct brad_isa_cinsn cd = brad_isa_cdecode(lo);
        *d = brad_isa_decode(0);
        d->opcode = brad_isa_cop_to_op(cd.cop);
        d->rd = cd.rd;
        d->rs1 = cd.rs1;
        d->rs2 = cd.rs2;
        int imm4 = (int)(cd.raw & 0xF);
        int off8 = (int)(int8_t)(cd.raw & 0xFF);
        switch (cd.cop) {
        case BRAD_COP_ADDI: case BRAD_COP_LDW: case BRAD_COP_STW:
            d->immediate = (int16_t)(imm4 << 2); break;
        case BRAD_COP_BZ: case BRAD_COP_BNZ:
        case BRAD_COP_JMP: case BRAD_COP_CALL:
            d->immediate = (int16_t)(off8 * 4); break;
        case BRAD_COP_MV:
            d->immediate = (int16_t)(cd.raw & 0xFF); break;
        default: d->immediate = 0; break;
        }
        core->pc = (core->pc & ~3) + 4;
        (void)hi;
        return 0;
    }
    if (core->pc + 4 > mem_size)
        return -EFAULT;
    uint32_t raw;
    memcpy(&raw, mem_base + core->pc, 4);
    *d = brad_isa_decode(raw);
    core->pc += 4;
    return 0;
}

static int run_phoenix_pipeline(struct brad_core_state *core,
                                 uint8_t *mem_base, size_t mem_size)
{
    struct brad_core_pipeline *p = &core->pipeline;
    advance_pipeline(core);

    if (p->stage_valid[p->exec_stage]) {
        struct brad_isa_insn *d = p->stage_insn[p->exec_stage];
        if (d) {
            uint32_t insn_pc = p->stage_pc[p->exec_stage];
            int taken = 0;
            int ret = execute(core, d, mem_base, mem_size, insn_pc, &taken);
            if (ret == 0 && taken) {
                core->stalls.control_hazard++;
                pipeline_flush(core);
                p->drain = 0;
                return 0;
            }
            (void)ret;
        }
    }

    if (!p->stage_insn[0] && core->running && !p->drain) {
        struct brad_isa_insn *d = malloc(sizeof(*d));
        if (fetch(core, mem_base, mem_size, d) == 0) {
            p->stage_insn[0] = d;
            p->stage_pc[0] = core->pc - 4;
            p->stage_valid[0] = 1;
        } else {
            free(d);
            p->drain = 1;
        }
    }

    core->cycles++;
    update_energy(core);

    if (p->drain) {
        unsigned empty = 1;
        for (unsigned i = 0; i < p->depth; i++)
            if (p->stage_valid[i]) { empty = 0; break; }
        if (empty) core->running = 0;
    }

    return 0;
}

static int run_falcon_pipeline(struct brad_core_state *core,
                                uint8_t *mem_base, size_t mem_size)
{
    struct brad_core_pipeline *p = &core->pipeline;
    advance_pipeline(core);

    if (p->stage_valid[p->exec_stage]) {
        struct brad_isa_insn *d = p->stage_insn[p->exec_stage];
        if (d) {
            uint32_t insn_pc = p->stage_pc[p->exec_stage];
            int taken = 0;
            execute(core, d, mem_base, mem_size, insn_pc, &taken);
            free(d);
            p->stage_insn[p->exec_stage] = NULL;
            p->stage_valid[p->exec_stage] = 0;
            if (taken) {
                core->stalls.control_hazard++;
                pipeline_flush(core);
                p->drain = 0;
                return 0;
            }
        }
    }

    if (!p->stage_insn[0] && core->running && !p->drain) {
        struct brad_isa_insn *d = malloc(sizeof(*d));
        if (fetch(core, mem_base, mem_size, d) == 0) {
            p->stage_insn[0] = d;
            p->stage_pc[0] = core->pc - 4;
            p->stage_valid[0] = 1;
        } else {
            free(d);
            p->drain = 1;
        }
    }

    core->cycles++;
    update_energy(core);

    if (p->drain) {
        unsigned empty = 1;
        for (unsigned i = 0; i < p->depth; i++)
            if (p->stage_valid[i]) { empty = 0; break; }
        if (empty) core->running = 0;
    }

    return 0;
}

static int run_kestrel_pipeline(struct brad_core_state *core,
                                 uint8_t *mem_base, size_t mem_size)
{
    struct brad_core_pipeline *p = &core->pipeline;
    advance_pipeline(core);

    if (p->stage_valid[p->exec_stage]) {
        struct brad_isa_insn *d = p->stage_insn[p->exec_stage];
        if (d) {
            uint32_t insn_pc = p->stage_pc[p->exec_stage];
            int taken = 0;
            execute(core, d, mem_base, mem_size, insn_pc, &taken);
            if (taken) {
                core->stalls.control_hazard++;
                pipeline_flush(core);
                p->drain = 0;
                return 0;
            }
        }
    }

    if (!p->stage_insn[0] && core->running && !p->drain) {
        struct brad_isa_insn *d = malloc(sizeof(*d));
        if (fetch(core, mem_base, mem_size, d) == 0) {
            p->stage_insn[0] = d;
            p->stage_pc[0] = core->pc - 4;
            p->stage_valid[0] = 1;
        } else {
            free(d);
            p->drain = 1;
        }
    }

    core->cycles++;
    update_energy(core);

    if (p->drain) {
        unsigned empty = 1;
        for (unsigned i = 0; i < p->depth; i++)
            if (p->stage_valid[i]) { empty = 0; break; }
        if (empty) core->running = 0;
    }

    return 0;
}

static int run_falcon_lite_pipeline(struct brad_core_state *core,
                                     uint8_t *mem_base, size_t mem_size)
{
    struct brad_core_pipeline *p = &core->pipeline;
    advance_pipeline(core);

    if (p->stage_valid[p->exec_stage]) {
        struct brad_isa_insn *d = p->stage_insn[p->exec_stage];
        if (d) {
            uint32_t insn_pc = p->stage_pc[p->exec_stage];
            int taken = 0;
            execute(core, d, mem_base, mem_size, insn_pc, &taken);
            free(d);
            p->stage_insn[p->exec_stage] = NULL;
            p->stage_valid[p->exec_stage] = 0;
            if (taken) {
                core->stalls.control_hazard++;
                pipeline_flush(core);
                p->drain = 0;
                return 0;
            }
        }
    }

    if (!p->stage_insn[0] && core->running && !p->drain) {
        struct brad_isa_insn *d = malloc(sizeof(*d));
        if (fetch(core, mem_base, mem_size, d) == 0) {
            p->stage_insn[0] = d;
            p->stage_pc[0] = core->pc - 4;
            p->stage_valid[0] = 1;
        } else {
            free(d);
            p->drain = 1;
        }
    }

    core->cycles++;
    update_energy(core);

    if (p->drain) {
        unsigned empty = 1;
        for (unsigned i = 0; i < p->depth; i++)
            if (p->stage_valid[i]) { empty = 0; break; }
        if (empty) core->running = 0;
    }

    return 0;
}

int brad_soc_emu_init(struct brad_soc_emu *soc,
                       unsigned num_clusters,
                       unsigned phoenix_per_cluster,
                       unsigned falcon_per_cluster)
{
    return brad_soc_emu_init_ex(soc, num_clusters,
                                phoenix_per_cluster, falcon_per_cluster,
                                0, 0);
}

int brad_soc_emu_init_ex(struct brad_soc_emu *soc,
                       unsigned num_clusters,
                       unsigned phoenix_per_cluster,
                       unsigned falcon_per_cluster,
                       unsigned kestrel_per_cluster,
                       unsigned falcon_lite_per_cluster)
{
    if (!soc)
        return -EINVAL;
    if (num_clusters > BRAD_CORE_MAX_CLUSTERS)
        return -EINVAL;

    unsigned total = phoenix_per_cluster + falcon_per_cluster
                     + kestrel_per_cluster + falcon_lite_per_cluster;
    if (total > BRAD_CORES_PER_CLUSTER)
        return -EINVAL;

    memset(soc, 0, sizeof(*soc));
    soc->num_clusters = num_clusters;
    soc->phoenix_per_cluster = phoenix_per_cluster;
    soc->falcon_per_cluster = falcon_per_cluster;
    soc->kestrel_per_cluster = kestrel_per_cluster;
    soc->falcon_lite_per_cluster = falcon_lite_per_cluster;

    soc->memory = (uint8_t *)calloc(1, BRAD_MEM_SIZE);
    if (!soc->memory)
        return -ENOMEM;
    soc->mem_size = BRAD_MEM_SIZE;

    unsigned idx = 0;
    for (unsigned c = 0; c < num_clusters; c++) {
        idx = 0;
        for (unsigned i = 0; i < phoenix_per_cluster; i++, idx++)
            brad_core_emu_init(&soc->cores[c][idx], BRAD_PIPE_PHOENIX, 0, 0);
        for (unsigned i = 0; i < falcon_per_cluster; i++, idx++)
            brad_core_emu_init(&soc->cores[c][idx], BRAD_PIPE_FALCON, 0, 0);
        for (unsigned i = 0; i < kestrel_per_cluster; i++, idx++)
            brad_core_emu_init(&soc->cores[c][idx], BRAD_PIPE_KESTREL, 0, 0);
        for (unsigned i = 0; i < falcon_lite_per_cluster; i++, idx++)
            brad_core_emu_init(&soc->cores[c][idx], BRAD_PIPE_FALCON_LITE, 0, 0);
    }

    return 0;
}

void brad_soc_emu_free(struct brad_soc_emu *soc)
{
    if (soc) {
        free(soc->memory);
        soc->memory = NULL;
        soc->mem_size = 0;
    }
}

int brad_core_emu_init(struct brad_core_state *core,
                        enum brad_isa_pipeline_model model,
                        uint32_t entry_pc, uint32_t sp)
{
    if (!core)
        return -EINVAL;

    memset(core, 0, sizeof(*core));
    core->model = model;
    core->pc = entry_pc;
    core->sp = sp;
    core->running = 1;
    core->msr_voltage = 1000;
    core->msr_freq = 1000;

    if (BRAD_NUM_REGS > BRAD_REG_SP) core->regs[BRAD_REG_SP] = sp;
    if (BRAD_NUM_REGS > BRAD_REG_LR) core->regs[BRAD_REG_LR] = 0;

    switch (model) {
    case BRAD_PIPE_PHOENIX:
        core->pipeline.depth = BRAD_PHOENIX_DEPTH;
        core->pipeline.exec_stage = BRAD_PHOENIX_EXEC_STAGE;
        core->msr_freq = 3000;
        core->msr_voltage = 1200;
        break;
    case BRAD_PIPE_FALCON:
        core->pipeline.depth = BRAD_FALCON_DEPTH;
        core->pipeline.exec_stage = BRAD_FALCON_EXEC_STAGE;
        core->msr_freq = 2400;
        core->msr_voltage = 700;
        break;
    case BRAD_PIPE_KESTREL:
        core->pipeline.depth = BRAD_KESTREL_DEPTH;
        core->pipeline.exec_stage = BRAD_KESTREL_EXEC_STAGE;
        core->msr_freq = 3500;
        core->msr_voltage = 900;
        break;
    case BRAD_PIPE_FALCON_LITE:
        core->pipeline.depth = BRAD_FALCON_LITE_DEPTH;
        core->pipeline.exec_stage = BRAD_FALCON_LITE_EXEC_STAGE;
        core->msr_freq = 1200;
        core->msr_voltage = 500;
        break;
    }

    return 0;
}

int brad_core_emu_load(struct brad_soc_emu *soc,
                        const uint32_t *program, size_t num_words,
                        uint32_t addr)
{
    if (!soc || !program)
        return -EINVAL;
    if (addr + num_words * 4 > soc->mem_size)
        return -ENOMEM;
    memcpy(soc->memory + addr, program, num_words * 4);
    return 0;
}

int brad_core_emu_step(struct brad_soc_emu *soc,
                        unsigned cluster, unsigned core_idx)
{
    if (!soc)
        return -EINVAL;
    if (cluster >= soc->num_clusters)
        return -EINVAL;

    unsigned total = soc->phoenix_per_cluster + soc->falcon_per_cluster
                     + soc->kestrel_per_cluster + soc->falcon_lite_per_cluster;
    if (core_idx >= total)
        return -EINVAL;

    struct brad_core_state *core = &soc->cores[cluster][core_idx];
    if (!core->running)
        return 0;

    switch (core->model) {
    case BRAD_PIPE_PHOENIX:
        return run_phoenix_pipeline(core, soc->memory, soc->mem_size);
    case BRAD_PIPE_FALCON:
        return run_falcon_pipeline(core, soc->memory, soc->mem_size);
    case BRAD_PIPE_KESTREL:
        return run_kestrel_pipeline(core, soc->memory, soc->mem_size);
    case BRAD_PIPE_FALCON_LITE:
        return run_falcon_lite_pipeline(core, soc->memory, soc->mem_size);
    }
    return 0;
}

int brad_core_emu_run(struct brad_soc_emu *soc,
                       unsigned cluster, unsigned core_idx,
                       uint64_t max_cycles)
{
    if (!soc)
        return -EINVAL;
    uint64_t start = soc->cores[cluster][core_idx].cycles;
    while (soc->cores[cluster][core_idx].running) {
        brad_core_emu_step(soc, cluster, core_idx);
        if (soc->cores[cluster][core_idx].cycles - start >= max_cycles)
            break;
    }
    return 0;
}

int brad_core_emu_run_all(struct brad_soc_emu *soc, uint64_t max_cycles)
{
    if (!soc)
        return -EINVAL;

    uint64_t start = soc->total_cycles;
    int any_running = 1;

    while (any_running && soc->total_cycles - start < max_cycles) {
        any_running = 0;
        for (unsigned c = 0; c < soc->num_clusters; c++) {
            unsigned total = soc->phoenix_per_cluster
                             + soc->falcon_per_cluster
                             + soc->kestrel_per_cluster
                             + soc->falcon_lite_per_cluster;
            for (unsigned i = 0; i < total; i++) {
                if (soc->cores[c][i].running) {
                    brad_core_emu_step(soc, c, i);
                    any_running = 1;
                }
            }
        }
        soc->total_cycles++;
    }

    return 0;
}

uint32_t brad_core_read_reg(struct brad_core_state *core, unsigned reg)
{
    if (!core)
        return 0;
    unsigned num_regs = (core->model == BRAD_PIPE_FALCON_LITE)
                        ? BRAD_NUM_REGS_LITE : BRAD_NUM_REGS;
    if (reg >= num_regs)
        return 0;
    return core->regs[reg];
}

void brad_core_write_reg(struct brad_core_state *core,
                          unsigned reg, uint32_t val)
{
    if (!core)
        return;
    unsigned num_regs = (core->model == BRAD_PIPE_FALCON_LITE)
                        ? BRAD_NUM_REGS_LITE : BRAD_NUM_REGS;
    if (reg >= num_regs)
        return;
    if (reg == BRAD_REG_R0)
        return;
    core->regs[reg] = val;
}

uint32_t brad_core_read_msr(struct brad_core_state *core, unsigned msr)
{
    if (!core) return 0;
    switch (msr) {
    case BRAD_MSR_STATUS:    return core->msr_status;
    case BRAD_MSR_CAUSE:     return core->msr_cause;
    case BRAD_MSR_PM_CTRL:   return core->msr_pm_ctrl;
    case BRAD_MSR_PSTATE:    return core->msr_pstate;
    case BRAD_MSR_VOLTAGE:   return core->msr_voltage;
    case BRAD_MSR_FREQ:      return core->msr_freq;
    case BRAD_MSR_ENERGY:    return (uint32_t)(core->msr_energy & 0xFFFFFFFF);
    case BRAD_MSR_PERF_CNT0: return core->msr_perf_cnt[0];
    case BRAD_MSR_PERF_CNT1: return core->msr_perf_cnt[1];
    case BRAD_MSR_PERF_CNT_CTRL: return core->msr_perf_cnt_ctrl;
    case BRAD_MSR_CG_MASK:   return core->msr_cg_mask;
    default: return 0;
    }
}

void brad_core_write_msr(struct brad_core_state *core, unsigned msr, uint32_t val)
{
    if (!core) return;
    switch (msr) {
    case BRAD_MSR_STATUS:
        core->msr_status = val;
        core->compress_mode = (val & BRAD_STATUS_C) ? 1 : 0;
        break;
    case BRAD_MSR_PM_CTRL:
        core->msr_pm_ctrl = val;
        if ((val & BRAD_PM_CSTATE_MASK) >= BRAD_PM_C2)
            core->msr_voltage = 500;
        else if ((val & BRAD_PM_CSTATE_MASK) >= BRAD_PM_C1)
            core->msr_voltage = core->msr_voltage * 8 / 10;
        break;
    case BRAD_MSR_PSTATE:
        core->msr_pstate = val & 0x1F;
        break;
    case BRAD_MSR_VOLTAGE:
        if (val >= 300 && val <= 1500)
            core->msr_voltage = val;
        break;
    case BRAD_MSR_FREQ:
        if (val >= 100 && val <= 10000)
            core->msr_freq = val;
        break;
    case BRAD_MSR_PERF_CNT_CTRL:
        core->msr_perf_cnt_ctrl = val;
        break;
    case BRAD_MSR_CG_MASK:
        core->msr_cg_mask = val;
        break;
    }
}

int brad_core_emu_disasm(uint32_t insn, char *buf, size_t buf_size)
{
    if (!buf || buf_size < 32)
        return -EINVAL;

    struct brad_isa_insn d = brad_isa_decode(insn);
    const char *opname = brad_isa_opcode_name(d.opcode);

    switch (d.opcode) {
    case BRAD_OP_ADD: case BRAD_OP_SUB: case BRAD_OP_MUL:
    case BRAD_OP_AND: case BRAD_OP_OR:  case BRAD_OP_XOR:
    case BRAD_OP_SHL: case BRAD_OP_SHR:
        snprintf(buf, buf_size, "%s r%u, r%u, r%u",
                 opname, d.rd, d.rs1, d.rs2);
        break;
    case BRAD_OP_ADDI:
        snprintf(buf, buf_size, "%s r%u, r%u, %d",
                 opname, d.rd, d.rs1, d.immediate);
        break;
    case BRAD_OP_LDW:
        snprintf(buf, buf_size, "%s r%u, [r%u + %d]",
                 opname, d.rd, d.rs1, d.immediate);
        break;
    case BRAD_OP_STW:
        snprintf(buf, buf_size, "%s r%u, [r%u + %d]",
                 opname, d.rs2, d.rs1, d.immediate);
        break;
    case BRAD_OP_BZ: case BRAD_OP_BNZ:
        snprintf(buf, buf_size, "%s r%u, %d",
                 opname, d.rs1, d.immediate);
        break;
    case BRAD_OP_JMP: case BRAD_OP_CALL:
        snprintf(buf, buf_size, "%s %d", opname, d.immediate);
        break;
    case BRAD_OP_RET:
        snprintf(buf, buf_size, "%s", opname);
        break;
    default:
        snprintf(buf, buf_size, "??? 0x%08X", insn);
        break;
    }
    return 0;
}
