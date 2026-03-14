/**
 * @file decoder.h
 * @brief SM85CPU (game.com / SM8521) instruction decoder
 *
 * Decodes the SM85CPU instruction set used by the Tiger game.com (SM8521).
 * The SM85CPU has 67 instruction families covering 8-bit and 16-bit
 * data transfer, arithmetic/logic, bit manipulation, shifts/rotates,
 * multiply/divide, and control flow.
 *
 * Opcode byte values are derived from hwdocs/sm8521.pdf.
 * Where exact bytes are marked TODO_OPCODE, verify against the datasheet
 * instruction-summary tables before trusting the decode result.
 */

#ifndef RECOMPILER_DECODER_H
#define RECOMPILER_DECODER_H

#include <cstdint>
#include <string>
#include <vector>

namespace gbrecomp {

/* ============================================================================
 * Register Definitions  (SM85CPU)
 * ========================================================================== */

/**
 * 8-bit general-purpose registers r0-r15.
 * These are memory-mapped and relocatable via PS0.RP (register pointer).
 * The 16 "slots" are indexed 0-15; which physical RAM cell each maps to
 * depends on PS0.RP at runtime.
 */
enum class Reg8 : uint8_t {
    R0  = 0,  R1  = 1,  R2  = 2,  R3  = 3,
    R4  = 4,  R5  = 5,  R6  = 6,  R7  = 7,
    R8  = 8,  R9  = 9,  R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15
};

/**
 * 16-bit register pairs.
 * rr0 = r1:r0 (r0 = low, r1 = high), rr2 = r3:r2, ..., rr14 = r15:r14.
 * Indexed 0-7 corresponding to rr0/rr2/.../rr14.
 * SP is the stack pointer (SPH:SPL), separate from the register file.
 */
enum class Reg16 : uint8_t {
    RR0  = 0,   // r1:r0
    RR2  = 1,   // r3:r2
    RR4  = 2,   // r5:r4
    RR6  = 3,   // r7:r6
    RR8  = 4,   // r9:r8
    RR10 = 5,   // r11:r10
    RR12 = 6,   // r13:r12
    RR14 = 7,   // r15:r14
    SP   = 8    // Stack pointer (dedicated; not in register file)
};

/**
 * Condition codes derived from PS1 flags.
 * Applies to JRC, JPC, CALLC, RETC instructions.
 */
enum class Condition : uint8_t {
    Z    = 0,  // Zero      (PS1.Z = 1)
    NZ   = 1,  // Not Zero  (PS1.Z = 0)
    C    = 2,  // Carry     (PS1.C = 1)
    NC   = 3,  // Not Carry (PS1.C = 0)
    S    = 4,  // Sign      (PS1.S = 1)
    NS   = 5,  // Not Sign  (PS1.S = 0)
    V    = 6,  // Overflow  (PS1.V = 1)
    NV   = 7,  // No Overflow(PS1.V = 0)
    ALWAYS = 8 // Unconditional
};

/* ============================================================================
 * Instruction Types  (SM85CPU 67 families)
 * ========================================================================== */

enum class InstructionType : uint16_t {
    // --- Invalid / Undefined ---
    INVALID   = 0,
    UNDEFINED,          // Opcode not in SM85CPU ISA

    // --- NOP ---
    NOP,

    // --- 8-bit Data Transfer ---
    LD_R_R,             // LD rd, rs            register-to-register
    LD_R_IMM,           // LD rd, #imm8         immediate
    LD_R_IND,           // LD rd, (rrs)         indirect via register pair
    LD_IND_R,           // LD (rrd), rs         store indirect
    LD_R_IDX,           // LD rd, (rrs+disp8)   indexed load
    LD_IDX_R,           // LD (rrd+disp8), rs   indexed store
    LD_R_DIR,           // LD rd, (nn)          direct 16-bit address load
    LD_DIR_R,           // LD (nn), rs          direct 16-bit address store

    // --- 16-bit Data Transfer ---
    LDW_RR_NN,          // LDW rrd, nn          load pair from 16-bit immediate
    LDW_RR_RR,          // LDW rrd, rrs         copy register pair
    PUSH_RR,            // PUSH rr
    POP_RR,             // POP rr

    // --- 8-bit Arithmetic/Logic (with flag effects) ---
    ADD_R_R,            // ADD rd, rs           rd += rs
    ADD_R_IMM,          // ADD rd, #imm8
    ADDC_R_R,           // ADDC rd, rs          rd += rs + C
    ADDC_R_IMM,         // ADDC rd, #imm8
    SUB_R_R,            // SUB rd, rs
    SUB_R_IMM,          // SUB rd, #imm8
    SUBC_R_R,           // SUBC rd, rs          rd -= rs + C
    SUBC_R_IMM,         // SUBC rd, #imm8
    AND_R_R,            // AND rd, rs
    AND_R_IMM,          // AND rd, #imm8
    OR_R_R,             // OR rd, rs
    OR_R_IMM,           // OR rd, #imm8
    XOR_R_R,            // XOR rd, rs
    XOR_R_IMM,          // XOR rd, #imm8
    CMP_R_R,            // CMP rd, rs           compare only (no writeback)
    CMP_R_IMM,          // CMP rd, #imm8
    INC_R,              // INC rd
    DEC_R,              // DEC rd
    CPL_R,              // CPL rd               bitwise NOT
    DAA,                // DAA                  decimal adjust
    SWAP_R,             // SWAP rd              swap high/low nibbles

    // --- 16-bit Arithmetic ---
    INCW_RR,            // INCW rrd
    DECW_RR,            // DECW rrd
    ADDW_RR_RR,         // ADDW rrd, rrs        16-bit add
    SUBW_RR_RR,         // SUBW rrd, rrs        16-bit subtract

    // --- Multiply / Divide ---
    MUL_R_R,            // MUL rd, rs           8×8 -> 16-bit result in rrd
    DIV_RR_R,           // DIV rrd, rs          16÷8 -> quotient+remainder

    // --- Shifts and Rotates ---
    ROL_R,              // ROL rd               rotate left; old bit7 -> C and bit0
    ROR_R,              // ROR rd               rotate right; old bit0 -> C and bit7
    ROLC_R,             // ROLC rd              rotate left through carry
    RORC_R,             // RORC rd              rotate right through carry
    SHL_R,              // SHL rd               shift left logical (bit0 = 0)
    SHR_R,              // SHR rd               shift right logical (bit7 = 0)
    SHAR_R,             // SHAR rd              shift right arithmetic (sign extended)

    // --- Bit Operations ---
    BIT_N_R,            // BIT n, rd            test bit n, set Z flag
    CLR_N_R,            // CLR n, rd            clear bit n
    SET_N_R,            // SET n, rd            set bit n
    BBC_N_R_E,          // BBC n, rd, disp8     branch if bit n clear (relative)
    BBS_N_R_E,          // BBS n, rd, disp8     branch if bit n set (relative)

    // --- Control Flow ---
    JP_NN,              // JP nn                unconditional absolute jump
    JP_RR,              // JP rr                indirect absolute jump via register pair
    JPC_CC_NN,          // JPC cc, nn           conditional absolute jump
    JR_E,               // JR disp8             unconditional relative jump
    JRC_CC_E,           // JRC cc, disp8        conditional relative jump
    CALL_NN,            // CALL nn
    CALLC_CC_NN,        // CALLC cc, nn         conditional call
    RET,                // RET
    RETC_CC,            // RETC cc              conditional return
    IRET,               // IRET                 return from interrupt (restores PS0/PS1)
    DBNZ_R_E,           // DBNZ rd, disp8       decrement & branch if not zero
    DI,                 // DI                   disable interrupts (PS1.I = 0)
    EI,                 // EI                   enable interrupts (PS1.I = 1)
    HALT,               // HALT
    STOP,               // STOP

    // Count
    TYPE_COUNT
};

/* ============================================================================
 * Instruction Structure
 * ========================================================================== */

/**
 * @brief Decoded SM85CPU instruction with all operand information.
 */
struct Instruction {
    // Location in ROM
    uint16_t address    = 0;
    uint8_t  bank       = 0;
    // Resolved target bank for CALL/JP after analysis (0xFF = unknown)
    uint8_t  resolved_target_bank = 0xFF;

    // Raw opcode byte(s)
    uint8_t opcode      = 0;    // Primary encoding byte
    uint8_t opcode2     = 0;    // Secondary byte for extended encodings

    // Decoded type
    InstructionType type = InstructionType::INVALID;

    // Size and timing
    uint8_t length      = 1;    // Instruction length in bytes (1-4)
    uint8_t cycles      = 2;    // Base cycle count
    uint8_t cycles_branch = 0;  // Extra cycles when a branch is taken

    // Operands — 8-bit registers encoded as index 0-15 (r0-r15)
    uint8_t reg8_dst    = 0;    // Destination register index
    uint8_t reg8_src    = 0;    // Source register index

    // 16-bit register pair (Reg16 enum)
    Reg16   reg16       = Reg16::RR0;

    // Condition for conditional instructions
    Condition condition = Condition::ALWAYS;

    // Bit index 0-7 for BIT/CLR/SET/BBC/BBS
    uint8_t bit_index   = 0;

    // Immediate values
    uint8_t  imm8       = 0;    // 8-bit immediate operand
    uint16_t imm16      = 0;    // 16-bit immediate (LDW address, absolute jump)
    int8_t   offset     = 0;    // Signed 8-bit offset (JR/JRC/BBC/BBS/DBNZ)

    // Control-flow classification
    bool is_jump        = false; // JP / JR variants
    bool is_call        = false; // CALL variants
    bool is_return      = false; // RET / RETC / IRET
    bool is_conditional = false; // Carries a condition code
    bool is_terminator  = false; // Unconditional end of basic block

    // Memory-access classification
    bool reads_memory   = false;
    bool writes_memory  = false;

    // PS1 flag effects
    struct {
        bool affects_c : 1;  // Carry
        bool affects_z : 1;  // Zero
        bool affects_s : 1;  // Sign
        bool affects_v : 1;  // Overflow
        bool affects_h : 1;  // Half-carry / BCD auxiliary
        bool affects_d : 1;  // Decimal mode
    } flag_effects = {};

    // MMU write hint: if this instruction writes to an MMU register
    // (0x0024-0x0028), records the MMU window index (0-4); else 0xFF.
    uint8_t mmu_write_window = 0xFF;

    std::string disassemble() const;
    std::string bytes_hex()   const;
};

// Forward declaration
class ROM;

/* ============================================================================
 * Decoder Class
 * ========================================================================== */

/**
 * @brief SM85CPU instruction decoder.
 *
 * Consult hwdocs/sm8521.pdf (instruction-set tables) to verify/extend
 * opcode byte assignments where marked TODO_OPCODE in the implementation.
 */
class Decoder {
public:
    explicit Decoder(const ROM& rom);

    /**
     * Decode instruction at a packed 32-bit address: (bank << 16) | addr.
     */
    Instruction decode(uint32_t full_addr) const;

    /**
     * Decode instruction at an explicit bank + 16-bit address.
     */
    Instruction decode(uint16_t addr, uint8_t bank) const;

private:
    void    decode_impl(Instruction& instr, uint8_t opcode,
                        uint16_t addr, uint8_t bank) const;
    uint16_t read_u16(uint16_t addr, uint8_t bank) const;
    uint8_t  read_u8 (uint16_t addr, uint8_t bank) const;

    const ROM& rom_;
};

/* ============================================================================
 * Helpers
 * ========================================================================== */

/**
 * Return the human-readable mnemonic name for an 8-bit register (r0-r15).
 */
const char* reg8_name(uint8_t idx);

/**
 * Return the human-readable mnemonic name for a 16-bit register pair.
 */
const char* reg16_name(Reg16 reg);

/**
 * Return the mnemonic for a condition code.
 */
const char* condition_name(Condition cond);

} // namespace gbrecomp

#endif // RECOMPILER_DECODER_H
