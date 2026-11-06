#include "brad_core_v2_emu.h"
#include "brad/bradisa.h"

#include <stdlib.h>
#include <string.h>

void brad_v2_init(brad_v2_state *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->mem = calloc(1, BRAD_V2_MEM_SIZE);
    s->mem_size = BRAD_V2_MEM_SIZE;
    s->regs[BRAD_V2_SP] = BRAD_V2_MEM_SIZE - 64;
    s->running = 1;
}

void brad_v2_free(brad_v2_state *s) {
    if (s) { free(s->mem); s->mem = NULL; }
}

int brad_v2_load(brad_v2_state *s, const uint32_t *code, size_t nwords, uint32_t entry) {
    if (!s || !s->mem) return -1;
    if (nwords * 4 > s->mem_size) return -1;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t w = code[i];
        s->mem[i*4 + 0] = w & 0xFF;
        s->mem[i*4 + 1] = (w >> 8) & 0xFF;
        s->mem[i*4 + 2] = (w >> 16) & 0xFF;
        s->mem[i*4 + 3] = (w >> 24) & 0xFF;
    }
    s->pc = entry;
    s->running = 1;
    s->steps = 0;
    s->insn_retired = 0;
    return 0;
}

static uint32_t mr32(brad_v2_state *s, uint32_t a) {
    if (a + 4 > s->mem_size) return 0;
    return s->mem[a] | (s->mem[a+1]<<8) | (s->mem[a+2]<<16) | (s->mem[a+3]<<24);
}
static void mw32(brad_v2_state *s, uint32_t a, uint32_t v) {
    if (a + 4 > s->mem_size) return;
    s->mem[a]=v&0xFF; s->mem[a+1]=(v>>8)&0xFF; s->mem[a+2]=(v>>16)&0xFF; s->mem[a+3]=(v>>24)&0xFF;
}
static uint16_t mr16(brad_v2_state *s, uint32_t a) {
    if (a + 2 > s->mem_size) return 0;
    return s->mem[a] | (s->mem[a+1]<<8);
}
static uint8_t mr8(brad_v2_state *s, uint32_t a) {
    if (a >= s->mem_size) return 0;
    return s->mem[a];
}
static void mw16(brad_v2_state *s, uint32_t a, uint16_t v) {
    if (a + 2 > s->mem_size) return;
    s->mem[a]=v&0xFF; s->mem[a+1]=(v>>8)&0xFF;
}
static void mw8(brad_v2_state *s, uint32_t a, uint8_t v) {
    if (a < s->mem_size) s->mem[a]=v;
}

#define REG(x) ((x)==0 ? 0u : s->regs[(x)])
#define SETREG(x,v) do { if ((x) != 0) s->regs[(x)] = (v); } while(0)

int brad_v2_step(brad_v2_state *s) {
    if (!s || !s->running) return 0;
    if (s->pc + 4 > s->mem_size) { s->running = 0; return 0; }

    uint32_t raw = mr32(s, s->pc);
    uint8_t op   = (raw >> BRAD_V2_OPCODE_SHIFT) & BRAD_V2_OPCODE_MASK;
    uint8_t rd   = (raw >> BRAD_V2_RD_SHIFT) & BRAD_V2_REG_MASK;
    uint8_t rs1  = (raw >> BRAD_V2_RS1_SHIFT) & BRAD_V2_REG_MASK;
    uint8_t rs2  = (raw >> BRAD_V2_RS2_SHIFT) & BRAD_V2_REG_MASK;
    uint32_t uimm16 = raw & BRAD_V2_IMM16_MASK;
    int32_t imm16 = (int32_t)(int16_t)uimm16;
    int32_t imm21 = (int32_t)(raw & BRAD_V2_IMM21_MASK);
    if (imm21 & 0x100000) imm21 |= (int32_t)0xFFE00000;

    uint32_t pc = s->pc;
    s->insn_retired++;
    s->steps++;

    switch (op) {
        /* ALU RRR */
        case BRAD_V2_OP_ADD:  SETREG(rd, REG(rs1) + REG(rs2)); break;
        case BRAD_V2_OP_SUB:  SETREG(rd, REG(rs1) - REG(rs2)); break;
        case BRAD_V2_OP_MUL:  SETREG(rd, REG(rs1) * REG(rs2)); break;
        case BRAD_V2_OP_MULHU: {
            uint64_t p = (uint64_t)REG(rs1) * (uint64_t)REG(rs2);
            SETREG(rd, (uint32_t)(p >> 32)); break;
        }
        case BRAD_V2_OP_DIV: {
            uint32_t d = REG(rs2);
            SETREG(rd, d ? (int32_t)REG(rs1) / (int32_t)d : 0); break;
        }
        case BRAD_V2_OP_DIVU: {
            uint32_t d = REG(rs2);
            SETREG(rd, d ? REG(rs1) / d : 0); break;
        }
        case BRAD_V2_OP_REM: {
            uint32_t d = REG(rs2);
            SETREG(rd, d ? (int32_t)REG(rs1) % (int32_t)d : 0); break;
        }
        case BRAD_V2_OP_REMU: {
            uint32_t d = REG(rs2);
            SETREG(rd, d ? REG(rs1) % d : 0); break;
        }
        case BRAD_V2_OP_AND:  SETREG(rd, REG(rs1) & REG(rs2)); break;
        case BRAD_V2_OP_OR:   SETREG(rd, REG(rs1) | REG(rs2)); break;
        case BRAD_V2_OP_XOR:  SETREG(rd, REG(rs1) ^ REG(rs2)); break;
        case BRAD_V2_OP_SHL:  SETREG(rd, REG(rs1) << (REG(rs2) & 0x1F)); break;
        case BRAD_V2_OP_SHR:  SETREG(rd, REG(rs1) >> (REG(rs2) & 0x1F)); break;
        case BRAD_V2_OP_SAR:  SETREG(rd, (uint32_t)((int32_t)REG(rs1) >> (REG(rs2) & 0x1F))); break;

        /* ALU RI */
        case BRAD_V2_OP_ADDI: SETREG(rd, REG(rs1) + (uint32_t)imm16); break;
        /* ANDI/ORI/XORI/SLTIU zero-extend the 16-bit field, matching
           brad_execute.v; the RI decode path sign-extends, so the emulator
           must use the raw field here. */
        case BRAD_V2_OP_ANDI: SETREG(rd, REG(rs1) & uimm16); break;
        case BRAD_V2_OP_ORI:  SETREG(rd, REG(rs1) | uimm16); break;
        case BRAD_V2_OP_XORI: SETREG(rd, REG(rs1) ^ uimm16); break;
        case BRAD_V2_OP_SLLI: SETREG(rd, REG(rs1) << (uimm16 & 0x1F)); break;
        case BRAD_V2_OP_SRLI: SETREG(rd, REG(rs1) >> (uimm16 & 0x1F)); break;
        case BRAD_V2_OP_SRAI: SETREG(rd, (uint32_t)((int32_t)REG(rs1) >> (uimm16 & 0x1F))); break;
        case BRAD_V2_OP_SLTI: {
            int32_t a = (int32_t)REG(rs1);
            SETREG(rd, (a < imm16) ? 1u : 0u); break;
        }
        case BRAD_V2_OP_SLTIU: {
            SETREG(rd, (REG(rs1) < uimm16) ? 1u : 0u); break;
        }

        /* Loads */
        case BRAD_V2_OP_LDW: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            SETREG(rd, mr32(s, addr)); break;
        }
        case BRAD_V2_OP_LHU: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            SETREG(rd, mr16(s, addr)); break;
        }
        case BRAD_V2_OP_LHBU: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            SETREG(rd, mr8(s, addr)); break;
        }

        /* Stores (SI: data=rd, base=rs1) */
        case BRAD_V2_OP_STW: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            mw32(s, addr, REG(rd)); break;
        }
        case BRAD_V2_OP_SH: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            mw16(s, addr, (uint16_t)(REG(rd) & 0xFFFFu)); break;
        }
        case BRAD_V2_OP_SB: {
            uint32_t addr = REG(rs1) + (uint32_t)imm16;
            uint32_t wordaddr = addr & ~3u;
            uint32_t old = mr32(s, wordaddr);
            int sh = (addr & 3) * 8;
            uint32_t v = (old & ~(0xFFu << sh)) | ((REG(rd) & 0xFFu) << sh);
            mw32(s, wordaddr, v); break;
        }

        /* Upper immediate */
        case BRAD_V2_OP_LUI: {
            SETREG(rd, (uint32_t)imm21 << 11); break;
        }
        case BRAD_V2_OP_AUIPC: {
            SETREG(rd, pc + ((uint32_t)imm21 << 11)); break;
        }

        /* Control transfer */
        case BRAD_V2_OP_JMP: {
            s->pc = pc + 4 + (imm21 << 2);
            return 1; /* taken, but we set pc; return running */
        }
        case BRAD_V2_OP_JAL: {
            SETREG(rd, pc + 4);
            s->pc = pc + 4 + (imm21 << 2);
            return 1;
        }
        case BRAD_V2_OP_CALL: {
            SETREG(BRAD_V2_LR, pc + 4);
            s->pc = pc + 4 + (imm21 << 2);
            return 1;
        }
        case BRAD_V2_OP_JALR: {
            uint32_t target = (REG(rs1) + (uint32_t)imm16) & ~1u;
            SETREG(rd, pc + 4);
            s->pc = target;
            return 1;
        }
        case BRAD_V2_OP_RET: {
            uint32_t lr = s->regs[BRAD_V2_LR];
            if (lr == 0) { s->running = 0; return 0; }
            s->pc = lr;
            return 1;
        }
        case BRAD_V2_OP_FENCE:
            break;

        /* Branches (BR: reg1=rd, reg2=rs1) */
        case BRAD_V2_OP_BEQ:
            if (REG(rd) == REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BNE:
            if (REG(rd) != REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BLT:
            if ((int32_t)REG(rd) < (int32_t)REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BGE:
            if ((int32_t)REG(rd) >= (int32_t)REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BLTU:
            if (REG(rd) < REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BGEU:
            if (REG(rd) >= REG(rs1)) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BZ:
            if (REG(rd) == 0) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;
        case BRAD_V2_OP_BNZ:
            if (REG(rd) != 0) { s->pc = pc + 4 + (imm16 << 2); return 1; }
            break;

        /* Atomics */
        case BRAD_V2_OP_LR_W: {
            uint32_t addr = REG(rs1);
            SETREG(rd, mr32(s, addr)); break;
        }
        case BRAD_V2_OP_SC_W: {
            uint32_t addr = REG(rs1);
            mw32(s, addr, REG(rs2));
            SETREG(rd, 0); /* success */
            break;
        }
        case BRAD_V2_OP_AMOSWAP_W: {
            uint32_t addr = REG(rs1);
            uint32_t old = mr32(s, addr);
            mw32(s, addr, REG(rs2));
            SETREG(rd, old); break;
        }
        case BRAD_V2_OP_AMOADD_W: {
            uint32_t addr = REG(rs1);
            uint32_t old = mr32(s, addr);
            mw32(s, addr, old + REG(rs2));
            SETREG(rd, old); break;
        }

        default:
            /* unknown opcode -> halt */
            s->running = 0;
            return 0;
    }

    s->pc = pc + 4;
    /* r0 hardwired */
    s->regs[0] = 0;
    return 1;
}

int brad_v2_run(brad_v2_state *s, uint64_t max_steps) {
    uint64_t start = s->steps;
    while (s->running && (s->steps - start) < max_steps) {
        if (!brad_v2_step(s)) break;
    }
    return 0;
}