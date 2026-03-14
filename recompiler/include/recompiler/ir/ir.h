/**
 * @file ir.h
 * @brief Intermediate Representation for SM85CPU (game.com) instructions
 *
 * The IR layer decouples instruction semantics from code generation,
 * enabling optimization passes and future backend support.
 *
 * This IR targets the SM85CPU register model:
 *   r0-r15   8-bit general purpose (memory-mapped, PS0.RP-relocatable)
 *   rr0-rr14 16-bit register pairs
 *   SP       Dedicated stack pointer
 *   PS0      Processor status 0
 *   PS1      Processor status 1 (flags: C Z S V D H B I)
 *
 * MMU note:
 *   Writes to addresses 0x0024-0x0028 (MMU0-MMU4) change ROM bank windows
 *   and are represented as MMU_WRITE pseudo-ops so the optimizer and
 *   code-emitter can track control-flow-relevant window state.
 */

#ifndef RECOMPILER_IR_H
#define RECOMPILER_IR_H

#include "recompiler/architecture.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace gbrecomp {
namespace ir {

/* ============================================================================
 * IR Opcodes
 * ========================================================================== */

enum class Opcode : uint16_t {

    // === Data Movement ===
    MOV_REG_REG,        // dst8 = src8              (LD rd, rs)
    MOV_REG_IMM8,       // dst8 = imm8              (LD rd, #n)
    MOV_REG16_IMM16,    // dst16 = imm16            (LDW rrd, nn)
    MOV_REG16_REG16,    // dst16 = src16            (LDW rrd, rrs)
    LOAD8_IND,          // dst8 = mem[src16]        (LD rd, (rrs))
    LOAD8_IDX,          // dst8 = mem[src16+disp]   (LD rd, (rrs+d))
    LOAD8_DIR,          // dst8 = mem[imm16]        (LD rd, (nn))
    STORE8_IND,         // mem[dst16] = src8        (LD (rrd), rs)
    STORE8_IDX,         // mem[dst16+disp] = src8   (LD (rrd+d), rs)
    STORE8_DIR,         // mem[imm16] = src8        (LD (nn), rs)
    PUSH16,             // SP -= 2; mem16[SP] = src16
    POP16,              // dst16 = mem16[SP]; SP += 2

    // === 8-bit ALU (set PS1 flags C Z S V H) ===
    ADD8,               // dst = dst + src
    ADDC8,              // dst = dst + src + C
    SUB8,               // dst = dst - src
    SUBC8,              // dst = dst - src - C
    AND8,               // dst = dst & src
    OR8,                // dst = dst | src
    XOR8,               // dst = dst ^ src
    CMP8,               // sets flags for (dst - src), no store
    INC8,               // dst++
    DEC8,               // dst--
    CPL8,               // dst = ~dst
    DAA,                // decimal adjust
    SWAP8,              // swap high/low nibbles

    // === 16-bit ALU ===
    INC16,              // rrd++
    DEC16,              // rrd--
    ADD16,              // rrd += rrs
    SUB16,              // rrd -= rrs

    // === Multiply / Divide ===
    MUL8,               // 8×8 -> 16-bit result in rrd (rd = low, rd+1 = high)
    DIV16,              // 16÷8 -> quotient in rrd, remainder elsewhere

    // === Shifts / Rotates ===
    ROL,                // rotate left; old bit7 -> C and bit0
    ROR,                // rotate right; old bit0 -> C and bit7
    ROLC,               // rotate left through carry
    RORC,               // rotate right through carry
    SHL,                // shift left logical (bit0 = 0)
    SHR,                // shift right logical (bit7 = 0)
    SHAR,               // shift right arithmetic (sign extend)

    // === Bit Operations ===
    BIT,                // test bit n of reg, set Z
    CLR_BIT,            // clear bit n of reg
    SET_BIT,            // set bit n of reg

    // === Control Flow ===
    JUMP,               // unconditional absolute jump
    JUMP_CC,            // conditional absolute jump
    JUMP_REG16,         // indirect absolute jump via register pair
    JR,                 // unconditional relative jump
    JR_CC,              // conditional relative jump
    CALL,               // push PC; jump
    CALL_CC,            // conditional call
    RET,                // pop PC
    RET_CC,             // conditional return
    IRET,               // return from interrupt (pop PS0, PS1, PC)
    DBNZ,               // decrement & branch if not zero
    BBC,                // branch if bit n clear (relative)
    BBS,                // branch if bit n set (relative)
    DI,                 // disable interrupts
    EI,                 // enable interrupts

    // === Misc ===
    NOP,
    HALT,
    STOP,
    DAA_ALIAS,          // alias kept for compatibility

    // === MMU / Banking (pseudo-ops) ===
    MMU_WRITE,          // notify: MMU register window_idx <- src8
                        //   dst = window index operand (0-4)
                        //   src = value being written (register or immediate)
    BANK_HINT,          // annotation: MMU state may have changed
    CROSS_BANK_CALL,    // cross-window CALL
    CROSS_BANK_JUMP,    // cross-window JP

    // === Meta (pseudo-ops) ===
    LABEL,              // label definition
    COMMENT,            // debug annotation
    SOURCE_LOC,         // source location marker
};

/* ============================================================================
 * Operand Types
 * ========================================================================== */

enum class OperandType : uint8_t {
    NONE = 0,
    REG8,           // 8-bit register index (0-15 = r0-r15)
    REG16,          // 16-bit register pair index (0-7 = rr0-rr14, 8 = SP)
    IMM8,           // 8-bit immediate
    IMM16,          // 16-bit immediate (direct address or 16-bit literal)
    OFFSET,         // Signed 8-bit PC-relative offset (JR, BBC, BBS, DBNZ)
    ADDR,           // 16-bit absolute address
    COND,           // Condition code (Condition enum value)
    BIT_IDX,        // Bit index 0-7
    BANK,           // ROM bank / MMU window number
    MMU_WINDOW,     // MMU window index 0-4
    MEM_REG16,      // Memory at [reg16]       (indirect)
    MEM_IDX,        // Memory at [reg16+disp8] (indexed)
    MEM_IMM16,      // Memory at [imm16]       (direct)
    LABEL_REF,      // Reference to an IR label id
};

/* ============================================================================
 * IR Operand
 * ========================================================================== */

struct Operand {
    OperandType type = OperandType::NONE;

    union {
        uint8_t  reg8;          // r0-r15 index
        uint8_t  reg16;         // rr pair index (0-7 with 8=SP)
        uint8_t  imm8;
        uint16_t imm16;
        int8_t   offset;
        uint8_t  bit_idx;
        uint8_t  bank;
        uint8_t  mmu_window;    // 0-4
        uint8_t  condition;     // Condition enum value
        uint32_t label_id;
    } value = {0};

    // Factory helpers
    static Operand none()                     { return Operand{}; }
    static Operand reg8(uint8_t r);
    static Operand reg16(uint8_t r);
    static Operand imm8(uint8_t v);
    static Operand imm16(uint16_t v);
    static Operand offset(int8_t o);
    static Operand condition(uint8_t c);
    static Operand bit_idx(uint8_t b);
    static Operand mem_reg16(uint8_t r);          // [rr]
    static Operand mem_idx(uint8_t r, int8_t d);  // [rr+d]  — stores r in reg16 field
    static Operand mem_imm16(uint16_t addr);
    static Operand mmu_window(uint8_t w);
    static Operand label(uint32_t id);
    static Operand bank_ref(uint8_t b);
};

/* ============================================================================
 * PS1 Flag Effects   (SM85CPU flags: C Z S V H D)
 * N flag does not exist on SM85CPU; use D (decimal) instead.
 * ========================================================================== */

struct FlagEffects {
    bool affects_c : 1;  // Carry
    bool affects_z : 1;  // Zero
    bool affects_s : 1;  // Sign
    bool affects_v : 1;  // Overflow
    bool affects_h : 1;  // Half-carry / BCD auxiliary
    bool affects_d : 1;  // Decimal mode flag

    // If fixed_x is set AND affects_x is set, the flag is forced to x_value.
    bool fixed_c   : 1;
    bool fixed_z   : 1;
    bool c_value   : 1;
    bool z_value   : 1;

    static FlagEffects none();
    static FlagEffects czsvh();             // all arithmetic flags computed
    static FlagEffects z_only();            // only Z flag affected
    static FlagEffects czs();              // C, Z, S computed
    static FlagEffects c_only();
};

/* ============================================================================
 * IR Instruction
 * ========================================================================== */

struct IRInstruction {
    Opcode   opcode;
    Operand  dst;
    Operand  src;
    Operand  extra;               // 3rd operand (e.g. BIT n, r, or indexed disp)

    // Source location
    uint8_t  source_bank    = 0;
    uint16_t source_address = 0;

    // Cycle cost
    uint8_t cycles          = 0;
    uint8_t cycles_branch   = 0;  // additional cycles when branch taken

    // PS1 flag effects
    FlagEffects flags = FlagEffects::none();

    std::string comment;

    // --- Factory methods ---

    static IRInstruction make_nop(uint8_t bank, uint16_t addr);

    // 8-bit register moves
    static IRInstruction make_mov_r_r   (uint8_t dst, uint8_t src,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_mov_r_imm8(uint8_t dst, uint8_t imm8,
                                         uint8_t bank, uint16_t addr);

    // Memory load/store
    static IRInstruction make_load8_dir (uint8_t dst_reg, uint16_t addr,
                                         uint8_t bank, uint16_t src_addr);
    static IRInstruction make_store8_dir(uint16_t addr, uint8_t src_reg,
                                         uint8_t bank, uint16_t src_addr);
    static IRInstruction make_load8_ind (uint8_t dst_reg, uint8_t pair_idx,
                                         uint8_t bank, uint16_t src_addr);
    static IRInstruction make_store8_ind(uint8_t pair_idx, uint8_t src_reg,
                                         uint8_t bank, uint16_t src_addr);

    // 8-bit ALU
    static IRInstruction make_alu8      (Opcode op, uint8_t dst, uint8_t src,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_alu8_imm  (Opcode op, uint8_t dst, uint8_t imm8,
                                         uint8_t bank, uint16_t addr);

    // Control flow
    static IRInstruction make_jump      (uint32_t label_id,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_jump_cc   (uint8_t cond, uint32_t label_id,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_jr        (int8_t offset,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_jr_cc     (uint8_t cond, int8_t offset,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_call      (uint32_t label_id,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_ret       (uint8_t bank, uint16_t addr);
    static IRInstruction make_iret      (uint8_t bank, uint16_t addr);

    // Bit operations
    static IRInstruction make_bit       (uint8_t n, uint8_t reg,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_bbc       (uint8_t n, uint8_t reg, int8_t disp,
                                         uint8_t bank, uint16_t addr);
    static IRInstruction make_bbs       (uint8_t n, uint8_t reg, int8_t disp,
                                         uint8_t bank, uint16_t addr);

    // MMU pseudo-op
    static IRInstruction make_mmu_write (uint8_t window, uint8_t src_reg,
                                         uint8_t bank, uint16_t addr);

    // Meta
    static IRInstruction make_label     (uint32_t label_id);
    static IRInstruction make_comment   (const std::string& text);
};

/* ============================================================================
 * Basic Block
 * ========================================================================== */

struct BasicBlock {
    uint32_t id;
    std::string label;
    std::vector<IRInstruction> instructions;

    // CFG
    std::vector<uint32_t> successors;
    std::vector<uint32_t> predecessors;

    // ROM context
    uint8_t  bank          = 0;
    uint16_t start_address = 0;
    uint16_t end_address   = 0;

    // MMU state entering this block (0xFF = unknown for that window)
    uint8_t  mmu_state[5]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    bool is_entry             = false;
    bool is_interrupt_handler = false;
    bool is_reachable         = false;
};

/* ============================================================================
 * Function
 * ========================================================================== */

struct Function {
    std::string name;
    uint8_t  bank          = 0;
    uint16_t entry_address = 0;
    std::array<uint8_t, 5> mmu_state = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint64_t context_key = 0;

    std::vector<uint32_t> block_ids;

    bool is_interrupt_handler = false;
    bool is_entry_point       = false;
    bool crosses_banks        = false;
};

/* ============================================================================
 * IR Program
 * ========================================================================== */

struct Program {
    std::string rom_name;

    // All basic blocks
    std::map<uint32_t, BasicBlock> blocks;
    uint32_t next_block_id = 0;

    // Functions
    std::map<std::string, Function> functions;

    // Labels
    std::map<uint32_t, std::string> labels;
    std::map<std::string, uint32_t> label_by_name;
    uint32_t next_label_id = 0;

    // ROM info
    uint16_t rom_bank_count = 0;
    uint8_t  mmu_init[5]   = {0, 1, 2, 3, 4};  // default MMU0-MMU4 banks

    // Entry points
    uint16_t main_entry = arch::GC_RESET_ENTRY;
    std::vector<uint16_t> interrupt_vectors;

    uint32_t create_block(uint8_t bank, uint16_t addr,
                          const std::array<uint8_t, 5>& mmu_state = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    uint32_t create_label(const std::string& name);
    uint32_t get_or_create_label(const std::string& name);
    std::string get_label_name(uint32_t id) const;
    std::string make_address_label(uint8_t bank, uint16_t addr,
                                   const std::array<uint8_t, 5>& mmu_state = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) const;
    std::string make_function_name(uint8_t bank, uint16_t addr) const;
    std::string make_function_name(uint8_t bank, uint16_t addr,
                                   const std::array<uint8_t, 5>& mmu_state) const;
};

/* ============================================================================
 * Utilities
 * ========================================================================== */

const char* opcode_name(Opcode op);
std::string format_instruction(const IRInstruction& instr);
void        dump_program(const Program& program, std::ostream& out);

} // namespace ir
} // namespace gbrecomp

#endif // RECOMPILER_IR_H
