/**
 * @file ir_builder.cpp
 * @brief IR builder implementation for SM85CPU (game.com / SM8521)
 *
 * Lowers decoded SM85CPU instructions into the game.com IR.
 */

#include "recompiler/ir/ir_builder.h"
#include "recompiler/decoder.h"
#include <sstream>
#include <iomanip>

namespace gbrecomp {
namespace ir {

/* ============================================================================
 * Operand factory methods
 * ========================================================================== */

Operand Operand::reg8(uint8_t r) {
    Operand op; op.type = OperandType::REG8; op.value.reg8 = r; return op;
}
Operand Operand::reg16(uint8_t r) {
    Operand op; op.type = OperandType::REG16; op.value.reg16 = r; return op;
}
Operand Operand::imm8(uint8_t v) {
    Operand op; op.type = OperandType::IMM8; op.value.imm8 = v; return op;
}
Operand Operand::imm16(uint16_t v) {
    Operand op; op.type = OperandType::IMM16; op.value.imm16 = v; return op;
}
Operand Operand::offset(int8_t o) {
    Operand op; op.type = OperandType::OFFSET; op.value.offset = o; return op;
}
Operand Operand::condition(uint8_t c) {
    Operand op; op.type = OperandType::COND; op.value.condition = c; return op;
}
Operand Operand::bit_idx(uint8_t b) {
    Operand op; op.type = OperandType::BIT_IDX; op.value.bit_idx = b; return op;
}
Operand Operand::mem_reg16(uint8_t r) {
    Operand op; op.type = OperandType::MEM_REG16; op.value.reg16 = r; return op;
}
Operand Operand::mem_idx(uint8_t r, int8_t /*d*/) {
    // Store pair index in reg16 field; caller passes disp as extra operand
    Operand op; op.type = OperandType::MEM_IDX; op.value.reg16 = r; return op;
}
Operand Operand::mem_imm16(uint16_t addr) {
    Operand op; op.type = OperandType::MEM_IMM16; op.value.imm16 = addr; return op;
}
Operand Operand::mmu_window(uint8_t w) {
    Operand op; op.type = OperandType::MMU_WINDOW; op.value.mmu_window = w; return op;
}
Operand Operand::label(uint32_t id) {
    Operand op; op.type = OperandType::LABEL_REF; op.value.label_id = id; return op;
}
Operand Operand::bank_ref(uint8_t b) {
    Operand op; op.type = OperandType::BANK; op.value.bank = b; return op;
}

/* ============================================================================
 * FlagEffects factory methods
 * ========================================================================== */

FlagEffects FlagEffects::none() { return FlagEffects{}; }

FlagEffects FlagEffects::czsvh() {
    FlagEffects f{};
    f.affects_c = f.affects_z = f.affects_s = f.affects_v = f.affects_h = true;
    return f;
}
FlagEffects FlagEffects::z_only() {
    FlagEffects f{}; f.affects_z = true; return f;
}
FlagEffects FlagEffects::czs() {
    FlagEffects f{}; f.affects_c = f.affects_z = f.affects_s = true; return f;
}
FlagEffects FlagEffects::c_only() {
    FlagEffects f{}; f.affects_c = true; return f;
}

/* ============================================================================
 * IRInstruction factory methods
 * ========================================================================== */

IRInstruction IRInstruction::make_nop(uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::NOP;
    i.source_bank = bank; i.source_address = addr; i.cycles = 2;
    return i;
}

IRInstruction IRInstruction::make_mov_r_r(uint8_t dst, uint8_t src,
                                          uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::MOV_REG_REG;
    i.dst = Operand::reg8(dst); i.src = Operand::reg8(src);
    i.source_bank = bank; i.source_address = addr; i.cycles = 4;
    return i;
}

IRInstruction IRInstruction::make_mov_r_imm8(uint8_t dst, uint8_t v,
                                              uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::MOV_REG_IMM8;
    i.dst = Operand::reg8(dst); i.src = Operand::imm8(v);
    i.source_bank = bank; i.source_address = addr; i.cycles = 4;
    return i;
}

IRInstruction IRInstruction::make_load8_dir(uint8_t dst_reg, uint16_t mem_addr,
                                             uint8_t bank, uint16_t src_addr) {
    IRInstruction i; i.opcode = Opcode::LOAD8_DIR;
    i.dst = Operand::reg8(dst_reg); i.src = Operand::mem_imm16(mem_addr);
    i.source_bank = bank; i.source_address = src_addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_store8_dir(uint16_t mem_addr, uint8_t src_reg,
                                              uint8_t bank, uint16_t src_addr) {
    IRInstruction i; i.opcode = Opcode::STORE8_DIR;
    i.dst = Operand::mem_imm16(mem_addr); i.src = Operand::reg8(src_reg);
    i.source_bank = bank; i.source_address = src_addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_load8_ind(uint8_t dst_reg, uint8_t pair_idx,
                                             uint8_t bank, uint16_t src_addr) {
    IRInstruction i; i.opcode = Opcode::LOAD8_IND;
    i.dst = Operand::reg8(dst_reg); i.src = Operand::mem_reg16(pair_idx);
    i.source_bank = bank; i.source_address = src_addr; i.cycles = 6;
    return i;
}

IRInstruction IRInstruction::make_store8_ind(uint8_t pair_idx, uint8_t src_reg,
                                              uint8_t bank, uint16_t src_addr) {
    IRInstruction i; i.opcode = Opcode::STORE8_IND;
    i.dst = Operand::mem_reg16(pair_idx); i.src = Operand::reg8(src_reg);
    i.source_bank = bank; i.source_address = src_addr; i.cycles = 6;
    return i;
}

IRInstruction IRInstruction::make_alu8(Opcode op, uint8_t dst, uint8_t src,
                                        uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = op;
    i.dst = Operand::reg8(dst); i.src = Operand::reg8(src);
    i.source_bank = bank; i.source_address = addr; i.cycles = 4;
    i.flags = FlagEffects::czsvh();
    return i;
}

IRInstruction IRInstruction::make_alu8_imm(Opcode op, uint8_t dst, uint8_t v,
                                            uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = op;
    i.dst = Operand::reg8(dst); i.src = Operand::imm8(v);
    i.source_bank = bank; i.source_address = addr; i.cycles = 4;
    i.flags = FlagEffects::czsvh();
    return i;
}

IRInstruction IRInstruction::make_jump(uint32_t label_id, uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::JUMP;
    i.dst = Operand::label(label_id);
    i.source_bank = bank; i.source_address = addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_jump_cc(uint8_t cond, uint32_t label_id,
                                           uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::JUMP_CC;
    i.dst = Operand::label(label_id); i.src = Operand::condition(cond);
    i.source_bank = bank; i.source_address = addr;
    i.cycles = 4; i.cycles_branch = 8;
    return i;
}

IRInstruction IRInstruction::make_jr(int8_t off, uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::JR;
    i.dst = Operand::offset(off);
    i.source_bank = bank; i.source_address = addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_jr_cc(uint8_t cond, int8_t off,
                                         uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::JR_CC;
    i.dst = Operand::offset(off); i.src = Operand::condition(cond);
    i.source_bank = bank; i.source_address = addr;
    i.cycles = 4; i.cycles_branch = 8;
    return i;
}

IRInstruction IRInstruction::make_call(uint32_t label_id, uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::CALL;
    i.dst = Operand::label(label_id);
    i.source_bank = bank; i.source_address = addr; i.cycles = 12;
    return i;
}

IRInstruction IRInstruction::make_ret(uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::RET;
    i.source_bank = bank; i.source_address = addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_iret(uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::IRET;
    i.source_bank = bank; i.source_address = addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_bit(uint8_t n, uint8_t reg,
                                       uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::BIT;
    i.dst = Operand::reg8(reg); i.src = Operand::bit_idx(n);
    i.source_bank = bank; i.source_address = addr; i.cycles = 4;
    i.flags = FlagEffects::z_only();
    return i;
}

IRInstruction IRInstruction::make_bbc(uint8_t n, uint8_t reg, int8_t disp,
                                       uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::BBC;
    i.dst = Operand::reg8(reg); i.src = Operand::bit_idx(n);
    i.extra = Operand::offset(disp);
    i.source_bank = bank; i.source_address = addr;
    i.cycles = 4; i.cycles_branch = 8;
    return i;
}

IRInstruction IRInstruction::make_bbs(uint8_t n, uint8_t reg, int8_t disp,
                                       uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::BBS;
    i.dst = Operand::reg8(reg); i.src = Operand::bit_idx(n);
    i.extra = Operand::offset(disp);
    i.source_bank = bank; i.source_address = addr;
    i.cycles = 4; i.cycles_branch = 8;
    return i;
}

IRInstruction IRInstruction::make_mmu_write(uint8_t window, uint8_t src_reg,
                                             uint8_t bank, uint16_t addr) {
    IRInstruction i; i.opcode = Opcode::MMU_WRITE;
    i.dst = Operand::mmu_window(window); i.src = Operand::reg8(src_reg);
    i.source_bank = bank; i.source_address = addr; i.cycles = 8;
    return i;
}

IRInstruction IRInstruction::make_label(uint32_t label_id) {
    IRInstruction i; i.opcode = Opcode::LABEL;
    i.dst = Operand::label(label_id);
    return i;
}

IRInstruction IRInstruction::make_comment(const std::string& text) {
    IRInstruction i; i.opcode = Opcode::COMMENT; i.comment = text;
    return i;
}

/* ============================================================================
 * Program methods
 * ========================================================================== */

uint32_t Program::create_block(uint8_t bank, uint16_t addr,
                               const std::array<uint8_t, 5>& mmu_state) {
    uint32_t id = next_block_id++;
    BasicBlock blk;
    blk.id = id; blk.bank = bank;
    blk.mmu_state[0] = mmu_state[0];
    blk.mmu_state[1] = mmu_state[1];
    blk.mmu_state[2] = mmu_state[2];
    blk.mmu_state[3] = mmu_state[3];
    blk.mmu_state[4] = mmu_state[4];
    blk.start_address = blk.end_address = addr;
    blk.label = make_address_label(bank, addr, mmu_state);
    blocks[id] = blk;
    return id;
}

uint32_t Program::create_label(const std::string& name) {
    uint32_t id = next_label_id++;
    labels[id] = name;
    label_by_name[name] = id;
    return id;
}

uint32_t Program::get_or_create_label(const std::string& name) {
    auto it = label_by_name.find(name);
    return (it != label_by_name.end()) ? it->second : create_label(name);
}

std::string Program::get_label_name(uint32_t id) const {
    auto it = labels.find(id);
    return (it != labels.end()) ? it->second : "unknown_label";
}

std::string Program::make_address_label(uint8_t bank, uint16_t addr,
                                        const std::array<uint8_t, 5>& mmu_state) const {
    std::ostringstream ss;
    ss << "loc_" << std::hex << std::setfill('0')
       << std::setw(2) << (int)bank << "_" << std::setw(4) << addr;
    if (mmu_state[0] != 0xFF) {
        ss << "_m"
           << std::setw(2) << (int)mmu_state[0]
           << std::setw(2) << (int)mmu_state[1]
           << std::setw(2) << (int)mmu_state[2]
           << std::setw(2) << (int)mmu_state[3]
           << std::setw(2) << (int)mmu_state[4];
    }
    return ss.str();
}

std::string Program::make_function_name(uint8_t bank, uint16_t addr) const {
    for (const auto& [name, fn] : functions) {
        if (fn.bank == bank && fn.entry_address == addr) {
            return name;
        }
    }

    std::ostringstream ss;
    ss << "func_" << std::hex << std::setfill('0')
       << std::setw(2) << (int)bank << "_" << std::setw(4) << addr;
    return ss.str();
}

std::string Program::make_function_name(uint8_t bank, uint16_t addr,
                                        const std::array<uint8_t, 5>& mmu_state) const {
    for (const auto& [name, fn] : functions) {
        if (fn.bank == bank && fn.entry_address == addr && fn.mmu_state == mmu_state) {
            return name;
        }
    }
    return make_function_name(bank, addr);
}

/* ============================================================================
 * IRBuilder
 * ========================================================================== */

IRBuilder::IRBuilder(const BuilderOptions& options) : options_(options) {}

void IRBuilder::emit(BasicBlock& block, IRInstruction instr, const Instruction& src) {
    if (options_.emit_source_locations) {
        instr.source_bank    = src.bank;
        instr.source_address = src.address;
    }
    block.instructions.push_back(std::move(instr));
}

Program IRBuilder::build(const AnalysisResult& analysis, const std::string& rom_name) {
    Program prog;
    prog.rom_name   = rom_name;
    prog.main_entry = analysis.entry_point;
    prog.interrupt_vectors = analysis.interrupt_vectors;

    for (const auto& [addr, func] : analysis.functions) {
        ir::Function ir_func;
        ir_func.name          = func.name;
        ir_func.bank          = func.bank;
        ir_func.entry_address = func.entry_address;
        ir_func.mmu_state     = func.mmu_state;
        ir_func.context_key   = func.context_key;
        ir_func.is_interrupt_handler = func.is_interrupt_handler;

        for (uint64_t blk_key : func.block_addresses) {
            auto it = analysis.blocks.find(blk_key);
            if (it == analysis.blocks.end()) continue;

            const gbrecomp::BasicBlock& src_blk = it->second;
            uint32_t bid = prog.create_block(func.bank, src_blk.start_address, src_blk.mmu_state);
            ir_func.block_ids.push_back(bid);
            ir::BasicBlock& dst_blk = prog.blocks[bid];
            dst_blk.end_address = src_blk.end_address;
            dst_blk.mmu_state[0] = src_blk.mmu_state[0];
            dst_blk.mmu_state[1] = src_blk.mmu_state[1];
            dst_blk.mmu_state[2] = src_blk.mmu_state[2];
            dst_blk.mmu_state[3] = src_blk.mmu_state[3];
            dst_blk.mmu_state[4] = src_blk.mmu_state[4];

            for (size_t idx : src_blk.instruction_indices) {
                if (idx < analysis.instructions.size()) {
                    const Instruction& instr = analysis.instructions[idx];
                    if (options_.emit_comments) {
                        auto c = IRInstruction::make_comment(instr.disassemble());
                        c.source_bank    = instr.bank;
                        c.source_address = instr.address;
                        dst_blk.instructions.push_back(c);
                    }
                    lower_instruction(instr, dst_blk);
                }
            }
        }
        prog.functions[func.name] = ir_func;
    }
    return prog;
}

void IRBuilder::lower_instruction(const Instruction& instr, BasicBlock& block) {
    using T = InstructionType;
    switch (instr.type) {
    // --- NOP / misc control ---
    case T::NOP:   lower_misc_ctrl(instr, block); break;
    case T::HALT:  lower_misc_ctrl(instr, block); break;
    case T::STOP:  lower_misc_ctrl(instr, block); break;
    case T::DI:    lower_misc_ctrl(instr, block); break;
    case T::EI:    lower_misc_ctrl(instr, block); break;

    // --- 8-bit data transfer ---
    case T::LD_R_R:    lower_ld_r_r  (instr, block); break;
    case T::LD_R_IMM:  lower_ld_r_imm(instr, block); break;
    case T::LD_R_IND:  lower_ld_r_ind(instr, block); break;
    case T::LD_IND_R:  lower_ld_ind_r(instr, block); break;
    case T::LD_R_IDX:  lower_ld_r_idx(instr, block); break;
    case T::LD_IDX_R:  lower_ld_idx_r(instr, block); break;
    case T::LD_R_DIR:  lower_ld_r_dir(instr, block); break;
    case T::LD_DIR_R:  lower_ld_dir_r(instr, block); break;

    // --- 16-bit data transfer ---
    case T::LDW_RR_NN: lower_ldw      (instr, block); break;
    case T::LDW_RR_RR: lower_ldw      (instr, block); break;
    case T::PUSH_RR:   lower_push_pop (instr, block); break;
    case T::POP_RR:    lower_push_pop (instr, block); break;

    // --- 8-bit ALU (reg-reg) ---
    case T::ADD_R_R:  case T::ADDC_R_R:
    case T::SUB_R_R:  case T::SUBC_R_R:
    case T::AND_R_R:  case T::OR_R_R:
    case T::XOR_R_R:  case T::CMP_R_R:
        lower_alu8_rr(instr, block); break;

    // --- 8-bit ALU (reg-imm) ---
    case T::ADD_R_IMM:  case T::ADDC_R_IMM:
    case T::SUB_R_IMM:  case T::SUBC_R_IMM:
    case T::AND_R_IMM:  case T::OR_R_IMM:
    case T::XOR_R_IMM:  case T::CMP_R_IMM:
        lower_alu8_imm(instr, block); break;

    // --- 8-bit inc/dec ---
    case T::INC_R: case T::DEC_R:
        lower_inc_dec(instr, block); break;

    // --- 8-bit misc ---
    case T::CPL_R: case T::DAA: case T::SWAP_R:
        lower_misc8(instr, block); break;

    // --- 16-bit ALU ---
    case T::INCW_RR: case T::DECW_RR:
        lower_inc_dec16(instr, block); break;
    case T::ADDW_RR_RR: case T::SUBW_RR_RR:
        lower_add_sub16(instr, block); break;

    // --- Multiply / divide ---
    case T::MUL_R_R: case T::DIV_RR_R:
        lower_mul_div(instr, block); break;

    // --- Shifts / rotates ---
    case T::ROL_R:  case T::ROR_R:
    case T::ROLC_R: case T::RORC_R:
    case T::SHL_R:  case T::SHR_R:
    case T::SHAR_R:
        lower_shift_rot(instr, block); break;

    // --- Bit operations ---
    case T::BIT_N_R: case T::CLR_N_R: case T::SET_N_R:
    case T::BBC_N_R_E: case T::BBS_N_R_E:
        lower_bit_op(instr, block); break;

    // --- Control flow ---
    case T::JP_NN:      case T::JP_RR:    case T::JPC_CC_NN:
    case T::JR_E:       case T::JRC_CC_E: case T::DBNZ_R_E:
        lower_jump(instr, block, *reinterpret_cast<Program*>(nullptr)); // prog not needed here
        break;

    case T::CALL_NN: case T::CALLC_CC_NN:
        lower_call(instr, block, *reinterpret_cast<Program*>(nullptr));
        break;

    case T::RET: case T::RETC_CC:
        lower_ret(instr, block); break;

    case T::IRET:
        emit(block, IRInstruction::make_iret(instr.bank, instr.address), instr);
        break;

    case T::INVALID:
    case T::UNDEFINED:
    default: {
        auto c = IRInstruction::make_comment("UNHANDLED: " + instr.disassemble());
        c.source_bank    = instr.bank;
        c.source_address = instr.address;
        block.instructions.push_back(c);
        break;
    }
    }
}

/* ============================================================================
 * Lowering helpers
 * ========================================================================== */

void IRBuilder::lower_ld_r_r(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_mov_r_r(instr.reg8_dst, instr.reg8_src,
                                            instr.bank, instr.address), instr);
}

void IRBuilder::lower_ld_r_imm(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_mov_r_imm8(instr.reg8_dst, instr.imm8,
                                               instr.bank, instr.address), instr);
}

void IRBuilder::lower_ld_r_ind(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_load8_ind(instr.reg8_dst,
                                              static_cast<uint8_t>(instr.reg16),
                                              instr.bank, instr.address), instr);
}

void IRBuilder::lower_ld_ind_r(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode         = Opcode::STORE8_IND;
    i.dst            = Operand::mem_reg16(static_cast<uint8_t>(instr.reg16));
    i.src            = Operand::reg8(instr.reg8_src);
    emit(block, i, instr);
}

void IRBuilder::lower_ld_r_idx(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode = Opcode::LOAD8_IDX;
    i.dst    = Operand::reg8(instr.reg8_dst);
    i.src    = Operand::mem_idx(static_cast<uint8_t>(instr.reg16), 0); // pair
    i.extra  = Operand::offset(instr.offset);
    emit(block, i, instr);
}

void IRBuilder::lower_ld_idx_r(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode = Opcode::STORE8_IDX;
    i.dst    = Operand::mem_idx(static_cast<uint8_t>(instr.reg16), 0);
    i.src    = Operand::reg8(instr.reg8_src);
    i.extra  = Operand::offset(instr.offset);
    emit(block, i, instr);
}

void IRBuilder::lower_ld_r_dir(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_load8_dir(instr.reg8_dst, instr.imm16,
                                              instr.bank, instr.address), instr);
}

void IRBuilder::lower_ld_dir_r(const Instruction& instr, BasicBlock& block) {
    auto i = IRInstruction::make_store8_dir(instr.imm16, instr.reg8_src,
                                            instr.bank, instr.address);
    // Emit MMU_WRITE pseudo-op if this store targets an MMU register
    if (instr.mmu_write_window != 0xFF) {
        emit(block, IRInstruction::make_mmu_write(instr.mmu_write_window,
                                                  instr.reg8_src,
                                                  instr.bank, instr.address), instr);
    }
    emit(block, i, instr);
}

void IRBuilder::lower_ldw(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    if (instr.type == InstructionType::LDW_RR_NN) {
        i.opcode = Opcode::MOV_REG16_IMM16;
        i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
        i.src    = Operand::imm16(instr.imm16);
    } else {
        i.opcode = Opcode::MOV_REG16_REG16;
        i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
        i.src    = Operand::reg16(instr.reg8_src);  // source pair stored in reg8_src
    }
    emit(block, i, instr);
}

void IRBuilder::lower_push_pop(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    if (instr.type == InstructionType::PUSH_RR) {
        i.opcode = Opcode::PUSH16;
        i.src    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
    } else {
        i.opcode = Opcode::POP16;
        i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
    }
    emit(block, i, instr);
}

// Map SM85CPU InstructionType -> IR Opcode for 8-bit ALU
static Opcode alu8_opcode(InstructionType t) {
    using T = InstructionType;
    switch (t) {
    case T::ADD_R_R:  case T::ADD_R_IMM:  return Opcode::ADD8;
    case T::ADDC_R_R: case T::ADDC_R_IMM: return Opcode::ADDC8;
    case T::SUB_R_R:  case T::SUB_R_IMM:  return Opcode::SUB8;
    case T::SUBC_R_R: case T::SUBC_R_IMM: return Opcode::SUBC8;
    case T::AND_R_R:  case T::AND_R_IMM:  return Opcode::AND8;
    case T::OR_R_R:   case T::OR_R_IMM:   return Opcode::OR8;
    case T::XOR_R_R:  case T::XOR_R_IMM:  return Opcode::XOR8;
    case T::CMP_R_R:  case T::CMP_R_IMM:  return Opcode::CMP8;
    default: return Opcode::NOP;
    }
}

void IRBuilder::lower_alu8_rr(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_alu8(alu8_opcode(instr.type),
                                         instr.reg8_dst, instr.reg8_src,
                                         instr.bank, instr.address), instr);
}

void IRBuilder::lower_alu8_imm(const Instruction& instr, BasicBlock& block) {
    emit(block, IRInstruction::make_alu8_imm(alu8_opcode(instr.type),
                                              instr.reg8_dst, instr.imm8,
                                              instr.bank, instr.address), instr);
}

void IRBuilder::lower_inc_dec(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode = (instr.type == InstructionType::INC_R) ? Opcode::INC8 : Opcode::DEC8;
    i.dst    = Operand::reg8(instr.reg8_dst);
    i.flags  = FlagEffects::czsvh();
    emit(block, i, instr);
}

void IRBuilder::lower_misc8(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    if (instr.type == InstructionType::CPL_R) {
        i.opcode = Opcode::CPL8; i.dst = Operand::reg8(instr.reg8_dst);
        i.flags  = FlagEffects::z_only();
    } else if (instr.type == InstructionType::DAA) {
        i.opcode = Opcode::DAA;
        i.flags  = FlagEffects::czsvh();
    } else { // SWAP_R
        i.opcode = Opcode::SWAP8; i.dst = Operand::reg8(instr.reg8_dst);
        i.flags  = FlagEffects::z_only();
    }
    emit(block, i, instr);
}

void IRBuilder::lower_inc_dec16(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode = (instr.type == InstructionType::INCW_RR) ? Opcode::INC16 : Opcode::DEC16;
    i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
    emit(block, i, instr);
}

void IRBuilder::lower_add_sub16(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    i.opcode = (instr.type == InstructionType::ADDW_RR_RR) ? Opcode::ADD16 : Opcode::SUB16;
    i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
    i.src    = Operand::reg16(instr.reg8_src);   // source pair index
    i.flags  = FlagEffects::czs();
    emit(block, i, instr);
}

void IRBuilder::lower_mul_div(const Instruction& instr, BasicBlock& block) {
    IRInstruction i;
    if (instr.type == InstructionType::MUL_R_R) {
        i.opcode = Opcode::MUL8;
        i.dst    = Operand::reg8(instr.reg8_dst);
        i.src    = Operand::reg8(instr.reg8_src);
    } else {
        i.opcode = Opcode::DIV16;
        i.dst    = Operand::reg16(static_cast<uint8_t>(instr.reg16));
        i.src    = Operand::reg8(instr.reg8_src);
    }
    i.flags = FlagEffects::czs();
    emit(block, i, instr);
}

void IRBuilder::lower_shift_rot(const Instruction& instr, BasicBlock& block) {
    using T = InstructionType;
    static const std::initializer_list<std::pair<T,Opcode>> TABLE = {
        {T::ROL_R,  Opcode::ROL},  {T::ROR_R,  Opcode::ROR},
        {T::ROLC_R, Opcode::ROLC}, {T::RORC_R, Opcode::RORC},
        {T::SHL_R,  Opcode::SHL},  {T::SHR_R,  Opcode::SHR},
        {T::SHAR_R, Opcode::SHAR},
    };
    Opcode op = Opcode::NOP;
    for (auto& [t, o] : TABLE) { if (t == instr.type) { op = o; break; } }
    IRInstruction i;
    i.opcode = op;
    i.dst    = Operand::reg8(instr.reg8_dst);
    i.flags  = FlagEffects::c_only();
    emit(block, i, instr);
}

void IRBuilder::lower_bit_op(const Instruction& instr, BasicBlock& block) {
    using T = InstructionType;
    if (instr.type == T::BIT_N_R) {
        emit(block, IRInstruction::make_bit(instr.bit_index, instr.reg8_dst,
                                            instr.bank, instr.address), instr);
    } else if (instr.type == T::CLR_N_R) {
        IRInstruction i; i.opcode = Opcode::CLR_BIT;
        i.dst = Operand::reg8(instr.reg8_dst); i.src = Operand::bit_idx(instr.bit_index);
        emit(block, i, instr);
    } else if (instr.type == T::SET_N_R) {
        IRInstruction i; i.opcode = Opcode::SET_BIT;
        i.dst = Operand::reg8(instr.reg8_dst); i.src = Operand::bit_idx(instr.bit_index);
        emit(block, i, instr);
    } else if (instr.type == T::BBC_N_R_E) {
        emit(block, IRInstruction::make_bbc(instr.bit_index, instr.reg8_src,
                                            instr.offset, instr.bank, instr.address), instr);
    } else { // BBS_N_R_E
        emit(block, IRInstruction::make_bbs(instr.bit_index, instr.reg8_src,
                                            instr.offset, instr.bank, instr.address), instr);
    }
}

void IRBuilder::lower_jump(const Instruction& instr, BasicBlock& block, Program& /*prog*/) {
    using T = InstructionType;
    if (instr.type == T::JP_NN) {
        // label_id resolved from imm16 — use imm16 as a placeholder label id
        emit(block, IRInstruction::make_jump(instr.imm16, instr.bank, instr.address), instr);
    } else if (instr.type == T::JPC_CC_NN) {
        emit(block, IRInstruction::make_jump_cc(static_cast<uint8_t>(instr.condition),
                                                instr.imm16, instr.bank, instr.address), instr);
    } else if (instr.type == T::JP_RR) {
        IRInstruction i; i.opcode = Opcode::JUMP_REG16;
        i.src = Operand::reg16(static_cast<uint8_t>(instr.reg16));
        emit(block, i, instr);
    } else if (instr.type == T::JR_E) {
        emit(block, IRInstruction::make_jr(instr.offset, instr.bank, instr.address), instr);
    } else if (instr.type == T::JRC_CC_E) {
        emit(block, IRInstruction::make_jr_cc(static_cast<uint8_t>(instr.condition),
                                              instr.offset, instr.bank, instr.address), instr);
    } else if (instr.type == T::DBNZ_R_E) {
        IRInstruction i; i.opcode = Opcode::DBNZ;
        i.dst = Operand::reg8(instr.reg8_dst); i.src = Operand::offset(instr.offset);
        emit(block, i, instr);
    }
}

void IRBuilder::lower_call(const Instruction& instr, BasicBlock& block, Program& /*prog*/) {
    if (instr.type == InstructionType::CALL_NN) {
        emit(block, IRInstruction::make_call(instr.imm16, instr.bank, instr.address), instr);
    } else { // CALLC_CC_NN
        IRInstruction i; i.opcode = Opcode::CALL_CC;
        i.dst = Operand::label(instr.imm16);
        i.src = Operand::condition(static_cast<uint8_t>(instr.condition));
        emit(block, i, instr);
    }
}

void IRBuilder::lower_ret(const Instruction& instr, BasicBlock& block) {
    if (instr.type == InstructionType::RET) {
        emit(block, IRInstruction::make_ret(instr.bank, instr.address), instr);
    } else { // RETC_CC
        IRInstruction i; i.opcode = Opcode::RET_CC;
        i.src = Operand::condition(static_cast<uint8_t>(instr.condition));
        emit(block, i, instr);
    }
}

void IRBuilder::lower_misc_ctrl(const Instruction& instr, BasicBlock& block) {
    using T = InstructionType;
    Opcode op;
    switch (instr.type) {
    case T::NOP:  op = Opcode::NOP; break;
    case T::HALT: op = Opcode::HALT; break;
    case T::STOP: op = Opcode::STOP; break;
    case T::DI:   op = Opcode::DI; break;
    case T::EI:   op = Opcode::EI; break;
    default:      op = Opcode::NOP; break;
    }
    IRInstruction i; i.opcode = op;
    emit(block, i, instr);
}

/* ============================================================================
 * Opcode name table
 * ========================================================================== */

const char* opcode_name(Opcode op) {
    switch (op) {
    case Opcode::MOV_REG_REG:    return "MOV_REG_REG";
    case Opcode::MOV_REG_IMM8:   return "MOV_REG_IMM8";
    case Opcode::MOV_REG16_IMM16:return "MOV_REG16_IMM16";
    case Opcode::MOV_REG16_REG16:return "MOV_REG16_REG16";
    case Opcode::LOAD8_IND:      return "LOAD8_IND";
    case Opcode::LOAD8_IDX:      return "LOAD8_IDX";
    case Opcode::LOAD8_DIR:      return "LOAD8_DIR";
    case Opcode::STORE8_IND:     return "STORE8_IND";
    case Opcode::STORE8_IDX:     return "STORE8_IDX";
    case Opcode::STORE8_DIR:     return "STORE8_DIR";
    case Opcode::PUSH16:         return "PUSH16";
    case Opcode::POP16:          return "POP16";
    case Opcode::ADD8:           return "ADD8";
    case Opcode::ADDC8:          return "ADDC8";
    case Opcode::SUB8:           return "SUB8";
    case Opcode::SUBC8:          return "SUBC8";
    case Opcode::AND8:           return "AND8";
    case Opcode::OR8:            return "OR8";
    case Opcode::XOR8:           return "XOR8";
    case Opcode::CMP8:           return "CMP8";
    case Opcode::INC8:           return "INC8";
    case Opcode::DEC8:           return "DEC8";
    case Opcode::CPL8:           return "CPL8";
    case Opcode::DAA:            return "DAA";
    case Opcode::SWAP8:          return "SWAP8";
    case Opcode::INC16:          return "INC16";
    case Opcode::DEC16:          return "DEC16";
    case Opcode::ADD16:          return "ADD16";
    case Opcode::SUB16:          return "SUB16";
    case Opcode::MUL8:           return "MUL8";
    case Opcode::DIV16:          return "DIV16";
    case Opcode::ROL:            return "ROL";
    case Opcode::ROR:            return "ROR";
    case Opcode::ROLC:           return "ROLC";
    case Opcode::RORC:           return "RORC";
    case Opcode::SHL:            return "SHL";
    case Opcode::SHR:            return "SHR";
    case Opcode::SHAR:           return "SHAR";
    case Opcode::BIT:            return "BIT";
    case Opcode::CLR_BIT:        return "CLR_BIT";
    case Opcode::SET_BIT:        return "SET_BIT";
    case Opcode::JUMP:           return "JUMP";
    case Opcode::JUMP_CC:        return "JUMP_CC";
    case Opcode::JUMP_REG16:     return "JUMP_REG16";
    case Opcode::JR:             return "JR";
    case Opcode::JR_CC:          return "JR_CC";
    case Opcode::CALL:           return "CALL";
    case Opcode::CALL_CC:        return "CALL_CC";
    case Opcode::RET:            return "RET";
    case Opcode::RET_CC:         return "RET_CC";
    case Opcode::IRET:           return "IRET";
    case Opcode::DBNZ:           return "DBNZ";
    case Opcode::BBC:            return "BBC";
    case Opcode::BBS:            return "BBS";
    case Opcode::DI:             return "DI";
    case Opcode::EI:             return "EI";
    case Opcode::NOP:            return "NOP";
    case Opcode::HALT:           return "HALT";
    case Opcode::STOP:           return "STOP";
    case Opcode::MMU_WRITE:      return "MMU_WRITE";
    case Opcode::BANK_HINT:      return "BANK_HINT";
    case Opcode::CROSS_BANK_CALL:return "CROSS_BANK_CALL";
    case Opcode::CROSS_BANK_JUMP:return "CROSS_BANK_JUMP";
    case Opcode::LABEL:          return "LABEL";
    case Opcode::COMMENT:        return "COMMENT";
    case Opcode::SOURCE_LOC:     return "SOURCE_LOC";
    default:                     return "?";
    }
}

std::string format_instruction(const IRInstruction& instr) {
    std::ostringstream ss;
    ss << opcode_name(instr.opcode);
    if (!instr.comment.empty()) ss << "  ; " << instr.comment;
    return ss.str();
}

void dump_program(const Program& prog, std::ostream& out) {
    out << "; ROM: " << prog.rom_name << "\n";
    for (const auto& [id, blk] : prog.blocks) {
        out << blk.label << ":\n";
        for (const auto& i : blk.instructions)
            out << "  " << format_instruction(i) << "\n";
    }
}

} // namespace ir
} // namespace gbrecomp
