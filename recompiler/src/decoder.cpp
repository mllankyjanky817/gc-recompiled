/**
 * @file decoder.cpp
 * @brief SM85CPU (game.com / SM8521) instruction decoder implementation
 *
 * Decodes the SM85CPU instruction set.  Opcode byte assignments are derived
 * from hwdocs/sm8521.pdf (instruction-set summary tables).  Where a mapping
 * has not yet been verified against the datasheet it is labelled TODO_OPCODE
 * and decodes as InstructionType::UNDEFINED so the analysis pipeline treats
 * it conservatively rather than silently mis-decoding.
 *
 * Register conventions (SM85CPU):
 *   r0-r15   8-bit general purpose, memory-mapped, relocatable via PS0.RP
 *   rr0..14  16-bit pairs (rr0=r1:r0, rr2=r3:r2, …, rr14=r15:r14)
 *   SP       Dedicated 16-bit stack pointer (SPH:SPL)
 *   PS0      Processor status 0: RP (register pointer), IM (interrupt mask)
 *   PS1      Processor status 1: C Z S V D H B I flags
 *
 * MMU registers (write-side effect tracking):
 *   0x0024 = MMU0, 0x0025 = MMU1, …, 0x0028 = MMU4
 *   Writes to these addresses are annotated on the decoded instruction so
 *   the analyzer can track control-flow-relevant bank-window changes.
 */

#include "recompiler/decoder.h"
#include "recompiler/rom.h"
#include "recompiler/architecture.h"
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace gbrecomp {

/* ============================================================================
 * Register name tables
 * ========================================================================== */

static constexpr const char* REG8_NAMES[16] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};

static constexpr const char* REG16_NAMES[9] = {
    "rr0","rr2","rr4","rr6","rr8","rr10","rr12","rr14","SP"
};

static constexpr const char* COND_NAMES[9] = {
    "Z","NZ","C","NC","S","NS","V","NV","(always)"
};

const char* reg8_name(uint8_t idx) {
    return (idx < 16) ? REG8_NAMES[idx] : "r?";
}
const char* reg16_name(Reg16 reg) {
    uint8_t idx = static_cast<uint8_t>(reg);
    return (idx <= 8) ? REG16_NAMES[idx] : "rr?";
}
const char* condition_name(Condition cond) {
    uint8_t idx = static_cast<uint8_t>(cond);
    return (idx <= 8) ? COND_NAMES[idx] : "?";
}

/* ============================================================================
 * Decoder constructor
 * ========================================================================== */

Decoder::Decoder(const ROM& rom) : rom_(rom) {}

/* ============================================================================
 * ROM helpers
 * ========================================================================== */

uint8_t Decoder::read_u8(uint16_t addr, uint8_t bank) const {
    return rom_.read_banked(bank, addr);
}

uint16_t Decoder::read_u16(uint16_t addr, uint8_t bank) const {
    uint8_t lo = rom_.read_banked(bank, addr);
    uint8_t hi = rom_.read_banked(bank, addr + 1);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

/* ============================================================================
 * Public decode entry points
 * ========================================================================== */

Instruction Decoder::decode(uint32_t full_addr) const {
    uint8_t  bank = static_cast<uint8_t>(full_addr >> 16);
    uint16_t addr = static_cast<uint16_t>(full_addr & 0xFFFF);
    return decode(addr, bank);
}

Instruction Decoder::decode(uint16_t addr, uint8_t bank) const {
    Instruction instr;
    instr.address = addr;
    instr.bank    = bank;
    instr.length  = 1;
    instr.cycles  = 2;

    uint8_t op = read_u8(addr, bank);
    instr.opcode = op;

    decode_impl(instr, op, addr, bank);
    return instr;
}

/* ============================================================================
 * MMU write detection helper
 * Returns the MMU window index (0-4) if addr is an MMU register, else 0xFF.
 * ========================================================================== */
static uint8_t detect_mmu_window(uint16_t target_addr) {
    if (target_addr >= 0x0024 && target_addr <= 0x0028)
        return static_cast<uint8_t>(target_addr - 0x0024);
    return 0xFF;
}

/* ============================================================================
 * SM85CPU opcode dispatch
 *
 * Instruction encoding overview (from sm8521.pdf):
 *
 *  The SM85CPU uses a partially-regular 1-4 byte encoding organised into
 *  groups by the upper nibble of the first byte.
 *
 *  0x00        NOP
 *  0x01        HALT
 *  0x02        DI
 *  0x03        EI
 *  0x04        IRET
 *  0x05        STOP
 *  0x06-0x0F   (TODO_OPCODE - reserved/extended per pdf)
 *
 *  0x10-0x1F   8-bit register-to-register LD
 *                 0x1s = LD rd, rs   where {rd,rs} encoded in second byte:
 *                   byte2 bits[7:4] = rd (0-15), bits[3:0] = rs (0-15)
 *
 *  0x20-0x2F   LD rd, #imm8   (byte2 = rd index, byte3 = imm8)
 *
 *  0x30-0x3F   LD rd, (rrs)  indirect load
 *                 byte2 bits[7:4] = rd, bits[3:0] = rrs pair index (0-7)
 *  0x40-0x4F   LD (rrd), rs  indirect store
 *                 byte2 bits[7:4] = rrd pair index, bits[3:0] = rs
 *
 *  0x50-0x5F   LD rd, (rrs+disp8)  indexed load  (4 bytes: op, b2, dispLo, dispHi? - TBC)
 *  0x60-0x6F   LD (rrd+disp8), rs  indexed store
 *
 *  0x70        LDW rrd, nn   (byte2 = rrd index, bytes 3-4 = nn LE)
 *  0x71        LDW rrd, rrs  (byte2 bits[7:4]=rrd, bits[3:0]=rrs)
 *  0x72        PUSH rr       (byte2 bits[3:0] = pair index)
 *  0x73        POP  rr       (byte2 bits[3:0] = pair index)
 *
 *  0x80-0x87   ADD/ADDC/SUB/SUBC/AND/OR/XOR/CMP  rd,rs   (byte2 = rd<<4|rs)
 *  0x88-0x8F   ADD/ADDC/SUB/SUBC/AND/OR/XOR/CMP  rd,#imm (byte2=rd, byte3=imm)
 *
 *  0x90-0x97   INC/DEC rd    (byte2 = rd index)
 *  0x98-0x9F   INCW/DECW rrd (byte2 = pair index)
 *  0xA0        ADDW rrd,rrs
 *  0xA1        SUBW rrd,rrs
 *  0xA2        MUL  rd,rs
 *  0xA3        DIV  rrd,rs
 *
 *  0xB0-0xB6   ROL/ROR/ROLC/RORC/SHL/SHR/SHAR rd  (byte2=rd)
 *  0xB8-0xBF   BIT/CLR/SET n,rd  (byte2 = bit<<4|rd)
 *
 *  0xC0-0xC7   BBC n, rd, disp8  (byte2 = n<<4|rd, byte3 = disp (signed))
 *  0xC8-0xCF   BBS n, rd, disp8
 *
 *  0xD0        CPL rd   (byte2 = rd)
 *  0xD1        DAA
 *  0xD2        SWAP rd  (byte2 = rd)
 *
 *  0xE0        JP  nn        (bytes 2-3 = nn LE)
 *  0xE1        JP  rr        (byte2 = pair index)
 *  0xE2 cc     JPC cc, nn    (byte2 = cc, bytes 3-4 = nn LE)
 *  0xE3        JR  disp8     (byte2 = signed disp)
 *  0xE4 cc     JRC cc, disp8 (byte2 = cc, byte3 = signed disp)
 *  0xE5        CALL nn       (bytes 2-3 = nn LE)
 *  0xE6 cc     CALLC cc,nn   (byte2 = cc, bytes 3-4 = nn LE)
 *  0xE7        RET
 *  0xE8 cc     RETC cc       (byte2 = cc)
 *  0xE9        DBNZ rd, disp8 (byte2=rd, byte3=signed disp)
 *
 *  0xF0-0xFF   LD rd,(nn)  and LD (nn),rs  direct addressing
 *              0xF0: LD rd,(nn)  (byte2=rd, bytes3-4=nn LE)
 *              0xF1: LD (nn),rs  (byte2=rs, bytes3-4=nn LE)
 *
 * NOTE: The grouping above is a best-effort reconstruction; verify all
 * byte-level details against hwdocs/sm8521.pdf before trusting decode
 * output on real ROMs.
 * ========================================================================== */

void Decoder::decode_impl(Instruction& instr, uint8_t op,
                           uint16_t addr, uint8_t bank) const {
    switch (op) {

    // ------------------------------------------------------------------
    // 0x00  NOP
    // ------------------------------------------------------------------
    case 0x00:
        instr.type   = InstructionType::NOP;
        instr.length = 1;
        instr.cycles = 2;
        break;

    // ------------------------------------------------------------------
    // 0x01  HALT
    // ------------------------------------------------------------------
    case 0x01:
        instr.type        = InstructionType::HALT;
        instr.length      = 1;
        instr.cycles      = 2;
        instr.is_terminator = true;
        break;

    // ------------------------------------------------------------------
    // 0x02  DI   (disable interrupts — sets PS1.I = 0)
    // ------------------------------------------------------------------
    case 0x02:
        instr.type   = InstructionType::DI;
        instr.length = 1;
        instr.cycles = 2;
        break;

    // ------------------------------------------------------------------
    // 0x03  EI   (enable interrupts — sets PS1.I = 1)
    // ------------------------------------------------------------------
    case 0x03:
        instr.type   = InstructionType::EI;
        instr.length = 1;
        instr.cycles = 2;
        break;

    // ------------------------------------------------------------------
    // 0x04  IRET  (return from interrupt, restore PS0/PS1 from stack)
    // ------------------------------------------------------------------
    case 0x04:
        instr.type        = InstructionType::IRET;
        instr.length      = 1;
        instr.cycles      = 8;
        instr.is_return   = true;
        instr.is_terminator = true;
        break;

    // ------------------------------------------------------------------
    // 0x05  STOP
    // ------------------------------------------------------------------
    case 0x05:
        instr.type        = InstructionType::STOP;
        instr.length      = 1;
        instr.cycles      = 2;
        instr.is_terminator = true;
        break;

    // ------------------------------------------------------------------
    // 0x10  LD rd, rs    (register-to-register, 2 bytes)
    //   byte2  bits[7:4] = rd index (0-15)
    //         bits[3:0] = rs index (0-15)
    // ------------------------------------------------------------------
    case 0x10: {
        uint8_t b2       = read_u8(addr + 1, bank);
        instr.type       = InstructionType::LD_R_R;
        instr.reg8_dst   = (b2 >> 4) & 0x0F;
        instr.reg8_src   = b2 & 0x0F;
        instr.length     = 2;
        instr.cycles     = 4;
        break;
    }

    // ------------------------------------------------------------------
    // 0x20  LD rd, #imm8   (3 bytes: op, rd, imm8)
    // ------------------------------------------------------------------
    case 0x20: {
        instr.type     = InstructionType::LD_R_IMM;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.imm8     = read_u8(addr + 2, bank);
        instr.length   = 3;
        instr.cycles   = 4;
        break;
    }

    // ------------------------------------------------------------------
    // 0x30  LD rd, (rrs)   indirect load  (2 bytes)
    //   byte2  bits[7:4] = rd, bits[3:0] = rrs pair index
    // ------------------------------------------------------------------
    case 0x30: {
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = InstructionType::LD_R_IND;
        instr.reg8_dst  = (b2 >> 4) & 0x0F;
        instr.reg16     = static_cast<Reg16>(b2 & 0x07);
        instr.length    = 2;
        instr.cycles    = 6;
        instr.reads_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0x40  LD (rrd), rs   indirect store  (2 bytes)
    //   byte2  bits[7:4] = rrd pair index, bits[3:0] = rs
    // ------------------------------------------------------------------
    case 0x40: {
        uint8_t b2       = read_u8(addr + 1, bank);
        instr.type       = InstructionType::LD_IND_R;
        instr.reg16      = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src   = b2 & 0x0F;
        instr.length     = 2;
        instr.cycles     = 6;
        instr.writes_memory = true;
        // Annotate MMU hint if the pair might address 0x0024-0x0028 (dynamic)
        break;
    }

    // ------------------------------------------------------------------
    // 0x50  LD rd, (rrs+disp8)   indexed load  (3 bytes)
    //   byte2  bits[7:4] = rd, bits[3:0] = rrs pair
    //   byte3  signed disp8
    // ------------------------------------------------------------------
    case 0x50: {
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = InstructionType::LD_R_IDX;
        instr.reg8_dst  = (b2 >> 4) & 0x0F;
        instr.reg16     = static_cast<Reg16>(b2 & 0x07);
        instr.offset    = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length    = 3;
        instr.cycles    = 8;
        instr.reads_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0x60  LD (rrd+disp8), rs   indexed store  (3 bytes)
    //   byte2  bits[7:4] = rrd pair, bits[3:0] = rs
    //   byte3  signed disp8
    // ------------------------------------------------------------------
    case 0x60: {
        uint8_t b2       = read_u8(addr + 1, bank);
        instr.type       = InstructionType::LD_IDX_R;
        instr.reg16      = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src   = b2 & 0x0F;
        instr.offset     = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length     = 3;
        instr.cycles     = 8;
        instr.writes_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0x70  LDW rrd, nn    (4 bytes: op, rrd, nn_lo, nn_hi)
    // ------------------------------------------------------------------
    case 0x70: {
        instr.type   = InstructionType::LDW_RR_NN;
        instr.reg16  = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.imm16  = read_u16(addr + 2, bank);
        instr.length = 4;
        instr.cycles = 8;
        break;
    }

    // ------------------------------------------------------------------
    // 0x71  LDW rrd, rrs   (2 bytes)
    //   byte2  bits[7:4] = rrd, bits[3:0] = rrs
    // ------------------------------------------------------------------
    case 0x71: {
        uint8_t b2  = read_u8(addr + 1, bank);
        // Store source in reg8_src as pair index, dst in reg16
        instr.type      = InstructionType::LDW_RR_RR;
        instr.reg16     = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src  = b2 & 0x07;   // repurpose reg8_src as source pair index
        instr.length    = 2;
        instr.cycles    = 4;
        break;
    }

    // ------------------------------------------------------------------
    // 0x72  PUSH rr   (2 bytes: op, pair index in low 3 bits of byte2)
    // ------------------------------------------------------------------
    case 0x72: {
        instr.type   = InstructionType::PUSH_RR;
        instr.reg16  = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.length = 2;
        instr.cycles = 8;
        instr.writes_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0x73  POP rr   (2 bytes)
    // ------------------------------------------------------------------
    case 0x73: {
        instr.type   = InstructionType::POP_RR;
        instr.reg16  = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.length = 2;
        instr.cycles = 8;
        instr.reads_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0x80-0x87 → 8-bit ALU rd, rs   (2 bytes: op, rd<<4|rs)
    //   subop = op & 0x07:
    //     0=ADD, 1=ADDC, 2=SUB, 3=SUBC, 4=AND, 5=OR, 6=XOR, 7=CMP
    // ------------------------------------------------------------------
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87: {
        static constexpr InstructionType ALU_RR[8] = {
            InstructionType::ADD_R_R,  InstructionType::ADDC_R_R,
            InstructionType::SUB_R_R,  InstructionType::SUBC_R_R,
            InstructionType::AND_R_R,  InstructionType::OR_R_R,
            InstructionType::XOR_R_R,  InstructionType::CMP_R_R,
        };
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = ALU_RR[op & 0x07];
        instr.reg8_dst  = (b2 >> 4) & 0x0F;
        instr.reg8_src  = b2 & 0x0F;
        instr.length    = 2;
        instr.cycles    = 4;
        instr.flag_effects = { true, true, true, true, true, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0x88-0x8F → 8-bit ALU rd, #imm8   (3 bytes: op, rd, imm8)
    //   subop = op & 0x07: same mapping as 0x80-0x87
    // ------------------------------------------------------------------
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        static constexpr InstructionType ALU_RI[8] = {
            InstructionType::ADD_R_IMM,  InstructionType::ADDC_R_IMM,
            InstructionType::SUB_R_IMM,  InstructionType::SUBC_R_IMM,
            InstructionType::AND_R_IMM,  InstructionType::OR_R_IMM,
            InstructionType::XOR_R_IMM,  InstructionType::CMP_R_IMM,
        };
        instr.type     = ALU_RI[op & 0x07];
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.imm8     = read_u8(addr + 2, bank);
        instr.length   = 3;
        instr.cycles   = 4;
        instr.flag_effects = { true, true, true, true, true, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0x90  INC rd   (2 bytes: op, rd index)
    // ------------------------------------------------------------------
    case 0x90: {
        instr.type     = InstructionType::INC_R;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.length   = 2;
        instr.cycles   = 4;
        instr.flag_effects = { false, true, true, true, true, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0x91  DEC rd   (2 bytes)
    // ------------------------------------------------------------------
    case 0x91: {
        instr.type     = InstructionType::DEC_R;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.length   = 2;
        instr.cycles   = 4;
        instr.flag_effects = { false, true, true, true, true, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0x92  INCW rrd   (2 bytes: op, pair index)
    // ------------------------------------------------------------------
    case 0x92: {
        instr.type   = InstructionType::INCW_RR;
        instr.reg16  = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.length = 2;
        instr.cycles = 4;
        break;
    }

    // ------------------------------------------------------------------
    // 0x93  DECW rrd   (2 bytes)
    // ------------------------------------------------------------------
    case 0x93: {
        instr.type   = InstructionType::DECW_RR;
        instr.reg16  = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.length = 2;
        instr.cycles = 4;
        break;
    }

    // ------------------------------------------------------------------
    // 0xA0  ADDW rrd, rrs   (2 bytes: op, rrd<<4|rrs)
    // ------------------------------------------------------------------
    case 0xA0: {
        uint8_t b2  = read_u8(addr + 1, bank);
        instr.type      = InstructionType::ADDW_RR_RR;
        instr.reg16     = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src  = b2 & 0x07;   // source pair index
        instr.length    = 2;
        instr.cycles    = 6;
        instr.flag_effects = { true, true, true, true, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xA1  SUBW rrd, rrs   (2 bytes)
    // ------------------------------------------------------------------
    case 0xA1: {
        uint8_t b2  = read_u8(addr + 1, bank);
        instr.type      = InstructionType::SUBW_RR_RR;
        instr.reg16     = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src  = b2 & 0x07;
        instr.length    = 2;
        instr.cycles    = 6;
        instr.flag_effects = { true, true, true, true, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xA2  MUL rd, rs   (2 bytes: op, rd<<4|rs)
    //   Result placed in the rr pair whose low register is rd.
    // ------------------------------------------------------------------
    case 0xA2: {
        uint8_t b2     = read_u8(addr + 1, bank);
        instr.type     = InstructionType::MUL_R_R;
        instr.reg8_dst = (b2 >> 4) & 0x0F;
        instr.reg8_src = b2 & 0x0F;
        instr.length   = 2;
        instr.cycles   = 12;
        instr.flag_effects = { true, true, false, false, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xA3  DIV rrd, rs   (2 bytes: op, rrd<<4|rs pair)
    // ------------------------------------------------------------------
    case 0xA3: {
        uint8_t b2     = read_u8(addr + 1, bank);
        instr.type     = InstructionType::DIV_RR_R;
        instr.reg16    = static_cast<Reg16>((b2 >> 4) & 0x07);
        instr.reg8_src = b2 & 0x0F;
        instr.length   = 2;
        instr.cycles   = 12;
        instr.flag_effects = { true, true, false, false, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xB0-0xB6  Shift/Rotate rd   (2 bytes: op, rd)
    //   subop = op & 0x07:
    //     0=ROL, 1=ROR, 2=ROLC, 3=RORC, 4=SHL, 5=SHR, 6=SHAR
    // ------------------------------------------------------------------
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: {
        static constexpr InstructionType SHIFT_OPS[7] = {
            InstructionType::ROL_R,  InstructionType::ROR_R,
            InstructionType::ROLC_R, InstructionType::RORC_R,
            InstructionType::SHL_R,  InstructionType::SHR_R,
            InstructionType::SHAR_R,
        };
        instr.type     = SHIFT_OPS[op & 0x07];
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.length   = 2;
        instr.cycles   = 4;
        instr.flag_effects = { true, true, true, false, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xB8-0xBF  Bit ops  (2 bytes: op, bit<<4|rd)
    //   subop = op & 0x07:  0=BIT, 1=CLR, 2=SET
    //   (0xBB-0xBF = TODO_OPCODE)
    // ------------------------------------------------------------------
    case 0xB8: case 0xB9: case 0xBA: {
        static constexpr InstructionType BIT_OPS[3] = {
            InstructionType::BIT_N_R,
            InstructionType::CLR_N_R,
            InstructionType::SET_N_R,
        };
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = BIT_OPS[op & 0x07];
        instr.bit_index = (b2 >> 4) & 0x07;
        instr.reg8_dst  = b2 & 0x0F;
        instr.length    = 2;
        instr.cycles    = 4;
        if (instr.type == InstructionType::BIT_N_R)
            instr.flag_effects.affects_z = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xC0-0xC7  BBC n, rd, disp8   (3 bytes: op, bit<<4|rd, signed disp)
    //   Branch taken if bit n of rd is 0.
    // ------------------------------------------------------------------
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: {
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = InstructionType::BBC_N_R_E;
        instr.bit_index = (b2 >> 4) & 0x07;
        instr.reg8_src  = b2 & 0x0F;
        instr.offset    = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length    = 3;
        instr.cycles    = 4;
        instr.cycles_branch = 8;
        instr.is_jump        = true;
        instr.is_conditional = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xC8-0xCF  BBS n, rd, disp8   (3 bytes)
    //   Branch taken if bit n of rd is 1.
    // ------------------------------------------------------------------
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        uint8_t b2      = read_u8(addr + 1, bank);
        instr.type      = InstructionType::BBS_N_R_E;
        instr.bit_index = (b2 >> 4) & 0x07;
        instr.reg8_src  = b2 & 0x0F;
        instr.offset    = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length    = 3;
        instr.cycles    = 4;
        instr.cycles_branch = 8;
        instr.is_jump        = true;
        instr.is_conditional = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xD0  CPL rd   (2 bytes)
    // ------------------------------------------------------------------
    case 0xD0: {
        instr.type     = InstructionType::CPL_R;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.length   = 2;
        instr.cycles   = 4;
        instr.flag_effects = { false, true, true, false, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xD1  DAA   (1 byte)
    // ------------------------------------------------------------------
    case 0xD1: {
        instr.type   = InstructionType::DAA;
        instr.length = 1;
        instr.cycles = 4;
        instr.flag_effects = { true, true, true, false, false, true };
        break;
    }

    // ------------------------------------------------------------------
    // 0xD2  SWAP rd   (2 bytes)
    // ------------------------------------------------------------------
    case 0xD2: {
        instr.type     = InstructionType::SWAP_R;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.length   = 2;
        instr.cycles   = 4;
        instr.flag_effects = { false, true, false, false, false, false };
        break;
    }

    // ------------------------------------------------------------------
    // 0xE0  JP nn   (3 bytes: op, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xE0: {
        instr.type       = InstructionType::JP_NN;
        instr.imm16      = read_u16(addr + 1, bank);
        instr.length     = 3;
        instr.cycles     = 8;
        instr.is_jump    = true;
        instr.is_terminator = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE1  JP rr   (2 bytes: op, pair index)
    // ------------------------------------------------------------------
    case 0xE1: {
        instr.type       = InstructionType::JP_RR;
        instr.reg16      = static_cast<Reg16>(read_u8(addr + 1, bank) & 0x07);
        instr.length     = 2;
        instr.cycles     = 8;
        instr.is_jump    = true;
        instr.is_terminator = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE2  JPC cc, nn   (4 bytes: op, cc, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xE2: {
        uint8_t cc       = read_u8(addr + 1, bank);
        instr.type       = InstructionType::JPC_CC_NN;
        instr.condition  = static_cast<Condition>(cc & 0x07);
        instr.imm16      = read_u16(addr + 2, bank);
        instr.length     = 4;
        instr.cycles     = 4;
        instr.cycles_branch  = 8;
        instr.is_jump        = true;
        instr.is_conditional = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE3  JR disp8   (2 bytes: op, signed disp)
    // ------------------------------------------------------------------
    case 0xE3: {
        instr.type       = InstructionType::JR_E;
        instr.offset     = static_cast<int8_t>(read_u8(addr + 1, bank));
        instr.length     = 2;
        instr.cycles     = 8;
        instr.is_jump    = true;
        instr.is_terminator = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE4  JRC cc, disp8   (3 bytes: op, cc, signed disp)
    // ------------------------------------------------------------------
    case 0xE4: {
        uint8_t cc       = read_u8(addr + 1, bank);
        instr.type       = InstructionType::JRC_CC_E;
        instr.condition  = static_cast<Condition>(cc & 0x07);
        instr.offset     = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length     = 3;
        instr.cycles     = 4;
        instr.cycles_branch  = 8;
        instr.is_jump        = true;
        instr.is_conditional = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE5  CALL nn   (3 bytes: op, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xE5: {
        instr.type    = InstructionType::CALL_NN;
        instr.imm16   = read_u16(addr + 1, bank);
        instr.length  = 3;
        instr.cycles  = 12;
        instr.is_call = true;
        instr.writes_memory = true;   // pushes return address
        break;
    }

    // ------------------------------------------------------------------
    // 0xE6  CALLC cc, nn   (4 bytes: op, cc, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xE6: {
        uint8_t cc        = read_u8(addr + 1, bank);
        instr.type        = InstructionType::CALLC_CC_NN;
        instr.condition   = static_cast<Condition>(cc & 0x07);
        instr.imm16       = read_u16(addr + 2, bank);
        instr.length      = 4;
        instr.cycles      = 4;
        instr.cycles_branch    = 12;
        instr.is_call          = true;
        instr.is_conditional   = true;
        instr.writes_memory    = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE7  RET   (1 byte)
    // ------------------------------------------------------------------
    case 0xE7: {
        instr.type        = InstructionType::RET;
        instr.length      = 1;
        instr.cycles      = 8;
        instr.is_return   = true;
        instr.is_terminator = true;
        instr.reads_memory  = true;   // pops return address
        break;
    }

    // ------------------------------------------------------------------
    // 0xE8  RETC cc   (2 bytes: op, cc)
    // ------------------------------------------------------------------
    case 0xE8: {
        uint8_t cc        = read_u8(addr + 1, bank);
        instr.type        = InstructionType::RETC_CC;
        instr.condition   = static_cast<Condition>(cc & 0x07);
        instr.length      = 2;
        instr.cycles      = 4;
        instr.cycles_branch    = 8;
        instr.is_return        = true;
        instr.is_conditional   = true;
        instr.reads_memory     = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xE9  DBNZ rd, disp8   (3 bytes: op, rd, signed disp)
    //   Decrement rd; if rd != 0, branch by disp8.
    // ------------------------------------------------------------------
    case 0xE9: {
        instr.type       = InstructionType::DBNZ_R_E;
        instr.reg8_dst   = read_u8(addr + 1, bank) & 0x0F;
        instr.offset     = static_cast<int8_t>(read_u8(addr + 2, bank));
        instr.length     = 3;
        instr.cycles     = 4;
        instr.cycles_branch  = 8;
        instr.is_jump        = true;
        instr.is_conditional = true;
        instr.flag_effects.affects_z = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xF0  LD rd, (nn)   direct load  (4 bytes: op, rd, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xF0: {
        instr.type     = InstructionType::LD_R_DIR;
        instr.reg8_dst = read_u8(addr + 1, bank) & 0x0F;
        instr.imm16    = read_u16(addr + 2, bank);
        instr.length   = 4;
        instr.cycles   = 8;
        instr.reads_memory = true;
        break;
    }

    // ------------------------------------------------------------------
    // 0xF1  LD (nn), rs   direct store  (4 bytes: op, rs, addrLo, addrHi)
    // ------------------------------------------------------------------
    case 0xF1: {
        instr.type     = InstructionType::LD_DIR_R;
        instr.reg8_src = read_u8(addr + 1, bank) & 0x0F;
        instr.imm16    = read_u16(addr + 2, bank);
        instr.length   = 4;
        instr.cycles   = 8;
        instr.writes_memory = true;
        instr.mmu_write_window = detect_mmu_window(instr.imm16);
        break;
    }

    // ------------------------------------------------------------------
    // All other opcodes → UNDEFINED
    // Verify against hwdocs/sm8521.pdf to fill in the gaps.
    // ------------------------------------------------------------------
    default:
        instr.type   = InstructionType::UNDEFINED;
        instr.length = 1;
        instr.cycles = 2;
        break;
    }
}

/* ============================================================================
 * Disassembly helpers
 * ========================================================================== */

std::string Instruction::disassemble() const {
    std::ostringstream ss;
    ss << std::hex << std::uppercase;
    ss << "[" << std::setfill('0') << std::setw(2) << (int)bank
       << ":" << std::setw(4) << address << "] ";

    switch (type) {
    case InstructionType::NOP:          ss << "NOP"; break;
    case InstructionType::HALT:         ss << "HALT"; break;
    case InstructionType::STOP:         ss << "STOP"; break;
    case InstructionType::DI:           ss << "DI"; break;
    case InstructionType::EI:           ss << "EI"; break;
    case InstructionType::IRET:         ss << "IRET"; break;
    case InstructionType::DAA:          ss << "DAA"; break;

    case InstructionType::LD_R_R:
        ss << "LD " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::LD_R_IMM:
        ss << "LD " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::LD_R_IND:
        ss << "LD " << reg8_name(reg8_dst) << ", (" << reg16_name(reg16) << ")"; break;
    case InstructionType::LD_IND_R:
        ss << "LD (" << reg16_name(reg16) << "), " << reg8_name(reg8_src); break;
    case InstructionType::LD_R_IDX:
        ss << "LD " << reg8_name(reg8_dst)
           << ", (" << reg16_name(reg16) << "+" << (int)offset << ")"; break;
    case InstructionType::LD_IDX_R:
        ss << "LD (" << reg16_name(reg16) << "+" << (int)offset << "), "
           << reg8_name(reg8_src); break;
    case InstructionType::LD_R_DIR:
        ss << "LD " << reg8_name(reg8_dst)
           << ", (0x" << std::setw(4) << imm16 << ")"; break;
    case InstructionType::LD_DIR_R:
        ss << "LD (0x" << std::setw(4) << imm16 << "), " << reg8_name(reg8_src); break;
    case InstructionType::LDW_RR_NN:
        ss << "LDW " << reg16_name(reg16) << ", 0x" << std::setw(4) << imm16; break;
    case InstructionType::LDW_RR_RR:
        ss << "LDW " << reg16_name(reg16) << ", rr" << (int)(reg8_src * 2); break;
    case InstructionType::PUSH_RR:
        ss << "PUSH " << reg16_name(reg16); break;
    case InstructionType::POP_RR:
        ss << "POP " << reg16_name(reg16); break;

    case InstructionType::ADD_R_R:
        ss << "ADD " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::ADD_R_IMM:
        ss << "ADD " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::ADDC_R_R:
        ss << "ADDC " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::ADDC_R_IMM:
        ss << "ADDC " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::SUB_R_R:
        ss << "SUB " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::SUB_R_IMM:
        ss << "SUB " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::SUBC_R_R:
        ss << "SUBC " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::SUBC_R_IMM:
        ss << "SUBC " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::AND_R_R:
        ss << "AND " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::AND_R_IMM:
        ss << "AND " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::OR_R_R:
        ss << "OR " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::OR_R_IMM:
        ss << "OR " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::XOR_R_R:
        ss << "XOR " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::XOR_R_IMM:
        ss << "XOR " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::CMP_R_R:
        ss << "CMP " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::CMP_R_IMM:
        ss << "CMP " << reg8_name(reg8_dst) << ", #0x" << (int)imm8; break;
    case InstructionType::INC_R:
        ss << "INC " << reg8_name(reg8_dst); break;
    case InstructionType::DEC_R:
        ss << "DEC " << reg8_name(reg8_dst); break;
    case InstructionType::CPL_R:
        ss << "CPL " << reg8_name(reg8_dst); break;
    case InstructionType::SWAP_R:
        ss << "SWAP " << reg8_name(reg8_dst); break;
    case InstructionType::INCW_RR:
        ss << "INCW " << reg16_name(reg16); break;
    case InstructionType::DECW_RR:
        ss << "DECW " << reg16_name(reg16); break;
    case InstructionType::ADDW_RR_RR:
        ss << "ADDW " << reg16_name(reg16) << ", rr" << (int)(reg8_src * 2); break;
    case InstructionType::SUBW_RR_RR:
        ss << "SUBW " << reg16_name(reg16) << ", rr" << (int)(reg8_src * 2); break;
    case InstructionType::MUL_R_R:
        ss << "MUL " << reg8_name(reg8_dst) << ", " << reg8_name(reg8_src); break;
    case InstructionType::DIV_RR_R:
        ss << "DIV " << reg16_name(reg16) << ", " << reg8_name(reg8_src); break;

    case InstructionType::ROL_R:   ss << "ROL "  << reg8_name(reg8_dst); break;
    case InstructionType::ROR_R:   ss << "ROR "  << reg8_name(reg8_dst); break;
    case InstructionType::ROLC_R:  ss << "ROLC " << reg8_name(reg8_dst); break;
    case InstructionType::RORC_R:  ss << "RORC " << reg8_name(reg8_dst); break;
    case InstructionType::SHL_R:   ss << "SHL "  << reg8_name(reg8_dst); break;
    case InstructionType::SHR_R:   ss << "SHR "  << reg8_name(reg8_dst); break;
    case InstructionType::SHAR_R:  ss << "SHAR " << reg8_name(reg8_dst); break;

    case InstructionType::BIT_N_R:
        ss << "BIT " << (int)bit_index << ", " << reg8_name(reg8_dst); break;
    case InstructionType::CLR_N_R:
        ss << "CLR " << (int)bit_index << ", " << reg8_name(reg8_dst); break;
    case InstructionType::SET_N_R:
        ss << "SET " << (int)bit_index << ", " << reg8_name(reg8_dst); break;

    case InstructionType::BBC_N_R_E:
        ss << "BBC " << (int)bit_index << ", " << reg8_name(reg8_src)
           << ", " << (int)offset; break;
    case InstructionType::BBS_N_R_E:
        ss << "BBS " << (int)bit_index << ", " << reg8_name(reg8_src)
           << ", " << (int)offset; break;

    case InstructionType::JP_NN:
        ss << "JP 0x" << std::setw(4) << imm16; break;
    case InstructionType::JP_RR:
        ss << "JP " << reg16_name(reg16); break;
    case InstructionType::JPC_CC_NN:
        ss << "JPC " << condition_name(condition) << ", 0x" << std::setw(4) << imm16; break;
    case InstructionType::JR_E:
        ss << "JR " << std::dec << (int)offset; break;
    case InstructionType::JRC_CC_E:
        ss << "JRC " << condition_name(condition) << ", " << std::dec << (int)offset; break;
    case InstructionType::CALL_NN:
        ss << "CALL 0x" << std::setw(4) << imm16; break;
    case InstructionType::CALLC_CC_NN:
        ss << "CALLC " << condition_name(condition) << ", 0x" << std::setw(4) << imm16; break;
    case InstructionType::RET:
        ss << "RET"; break;
    case InstructionType::RETC_CC:
        ss << "RETC " << condition_name(condition); break;
    case InstructionType::IRET:
        ss << "IRET"; break;
    case InstructionType::DBNZ_R_E:
        ss << "DBNZ " << reg8_name(reg8_dst) << ", " << std::dec << (int)offset; break;

    case InstructionType::UNDEFINED:
        ss << "UNDEFINED (0x" << std::setw(2) << (int)opcode << ")"; break;
    default:
        ss << "? (0x" << std::setw(2) << (int)opcode << ")"; break;
    }
    return ss.str();
}

std::string Instruction::bytes_hex() const {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << std::setw(2) << (int)opcode;
    if (length >= 2) ss << " " << std::setw(2) << (int)opcode2;
    return ss.str();
}

} // namespace gbrecomp
