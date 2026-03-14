/**
 * @file ir_builder.h
 * @brief Build IR from decoded SM85CPU (game.com / SM8521) instructions
 */

#ifndef RECOMPILER_IR_BUILDER_H
#define RECOMPILER_IR_BUILDER_H

#include "ir.h"
#include "../decoder.h"
#include "../analyzer.h"
#include <vector>

namespace gbrecomp {
namespace ir {

/**
 * @brief Options for IR building
 */
struct BuilderOptions {
    bool emit_source_locations  = true;
    bool emit_comments          = true;
    bool preserve_flags_exactly = true;
};

/**
 * @brief Builds IR from analyzed SM85CPU instructions.
 */
class IRBuilder {
public:
    explicit IRBuilder(const BuilderOptions& options = {});

    /**
     * Build an IR Program from an AnalysisResult.
     */
    Program build(const AnalysisResult& analysis, const std::string& rom_name);

    /**
     * Lower a single decoded SM85CPU instruction into an IR BasicBlock.
     */
    void lower_instruction(const Instruction& instr, BasicBlock& block);

private:
    BuilderOptions options_;

    // --- SM85CPU instruction group lowering helpers ---

    // 8-bit data transfer
    void lower_ld_r_r   (const Instruction&, BasicBlock&);
    void lower_ld_r_imm (const Instruction&, BasicBlock&);
    void lower_ld_r_ind (const Instruction&, BasicBlock&);
    void lower_ld_ind_r (const Instruction&, BasicBlock&);
    void lower_ld_r_idx (const Instruction&, BasicBlock&);
    void lower_ld_idx_r (const Instruction&, BasicBlock&);
    void lower_ld_r_dir (const Instruction&, BasicBlock&);
    void lower_ld_dir_r (const Instruction&, BasicBlock&);

    // 16-bit data transfer
    void lower_ldw      (const Instruction&, BasicBlock&);
    void lower_push_pop (const Instruction&, BasicBlock&);

    // 8-bit ALU
    void lower_alu8_rr  (const Instruction&, BasicBlock&);
    void lower_alu8_imm (const Instruction&, BasicBlock&);
    void lower_inc_dec  (const Instruction&, BasicBlock&);
    void lower_misc8    (const Instruction&, BasicBlock&);  // CPL, DAA, SWAP

    // 16-bit ALU
    void lower_inc_dec16(const Instruction&, BasicBlock&);
    void lower_add_sub16(const Instruction&, BasicBlock&);

    // Multiply / divide
    void lower_mul_div  (const Instruction&, BasicBlock&);

    // Shift / rotate
    void lower_shift_rot(const Instruction&, BasicBlock&);

    // Bit operations (BIT / CLR / SET / BBC / BBS)
    void lower_bit_op   (const Instruction&, BasicBlock&);

    // Control flow
    void lower_jump     (const Instruction&, BasicBlock&, Program&);
    void lower_call     (const Instruction&, BasicBlock&, Program&);
    void lower_ret      (const Instruction&, BasicBlock&);
    void lower_misc_ctrl(const Instruction&, BasicBlock&);  // DI, EI, HALT, STOP, NOP

    // Emit helper — attaches source location and appends to block
    void emit(BasicBlock& block, IRInstruction instr, const Instruction& src);
};

} // namespace ir
} // namespace gbrecomp

#endif // RECOMPILER_IR_BUILDER_H
