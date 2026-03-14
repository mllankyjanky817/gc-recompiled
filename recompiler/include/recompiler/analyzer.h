/**
 * @file analyzer.h
 * @brief Control flow analysis for game.com ROMs
 */

#ifndef RECOMPILER_ANALYZER_H
#define RECOMPILER_ANALYZER_H

#include "decoder.h"
#include "rom.h"
#include "bank_tracker.h"
#include <array>
#include <map>
#include <set>
#include <vector>
#include <string>

namespace gbrecomp {

/* ============================================================================
 * Basic Block
 * ========================================================================== */

/**
 * @brief A basic block - sequence of instructions without branches
 */
struct BasicBlock {
    uint64_t context_key = 0;           // Packed (bank, addr, mmu[0..4]) key
    uint16_t start_address;
    uint16_t end_address;
    uint8_t bank;                       // ROM bank this block belongs to
    std::array<uint8_t, 5> mmu_state = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    std::vector<size_t> instruction_indices;
    
    // Control flow
    std::vector<uint16_t> successors;   // Addresses of successor blocks
    std::vector<uint64_t> successor_keys;
    std::vector<uint16_t> predecessors; // Addresses of predecessor blocks
    
    // Cross-bank info
    bool has_cross_bank_successor = false;
    
    // Labels needed within this block
    std::set<uint16_t> internal_labels;
    
    // Is this block the entry to a function?
    bool is_function_entry = false;
    
    // Is this block an interrupt handler entry?
    bool is_interrupt_entry = false;
    
    // Is this block reachable from entry points?
    bool is_reachable = false;
};

/* ============================================================================
 * Function
 * ========================================================================== */

/**
 * @brief A function - collection of basic blocks with single entry
 */
struct Function {
    std::string name;
    uint64_t context_key = 0;           // Packed (bank, addr, mmu[0..4]) key
    uint16_t entry_address;
    uint8_t bank;
    std::array<uint8_t, 5> mmu_state = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    std::vector<uint64_t> block_addresses;
    
    bool is_interrupt_handler = false;
    bool crosses_banks = false;         // Calls into other banks
    bool is_called_cross_bank = false;  // Called from other banks
};

/* ============================================================================
 * Analysis Result
 * ========================================================================== */

/**
 * @brief Complete control flow analysis result
 */
struct AnalysisResult {
    using ContextKey = uint64_t;

    // ROM reference
    const ROM* rom = nullptr;
    
    // All decoded instructions
    std::vector<Instruction> instructions;
    
    // Address to instruction index map
    std::map<ContextKey, size_t> addr_to_index;  // packed context key -> index
    
    // Basic blocks indexed by packed context key
    std::map<ContextKey, BasicBlock> blocks;
    
    // Functions indexed by packed context key
    std::map<ContextKey, Function> functions;
    
    // Labels needed (jump targets)
    std::set<ContextKey> label_addresses;
    
    // Call targets (function entry points)
    std::set<ContextKey> call_targets;
    
    // Computed jump targets (JP HL, etc.)
    std::set<ContextKey> computed_jump_targets;
    
    // Bank switch points
    std::set<uint16_t> bank_switch_addresses;
    
    // Entry point
    uint16_t entry_point = arch::GC_RESET_ENTRY;
    
    // Interrupt vectors
    std::vector<uint16_t> interrupt_vectors;
    
    // Statistics
    struct {
        size_t total_instructions = 0;
        size_t total_blocks = 0;
        size_t total_functions = 0;
        size_t unreachable_instructions = 0;
        size_t cross_bank_calls = 0;
    } stats;
    
    // Bank tracker results
    BankTracker bank_tracker;
    
    // Helper to create combined address
    static uint32_t make_addr(uint8_t bank, uint16_t addr) {
        return (static_cast<uint32_t>(bank) << 16) | addr;
    }

    static ContextKey make_context_addr(uint8_t bank, uint16_t addr,
                                        const std::array<uint8_t, 5>& mmu) {
        ContextKey key = static_cast<ContextKey>(bank);
        key = (key << 16) | static_cast<ContextKey>(addr);
        key = (key << 8) | static_cast<ContextKey>(mmu[0]);
        key = (key << 8) | static_cast<ContextKey>(mmu[1]);
        key = (key << 8) | static_cast<ContextKey>(mmu[2]);
        key = (key << 8) | static_cast<ContextKey>(mmu[3]);
        key = (key << 8) | static_cast<ContextKey>(mmu[4]);
        return key;
    }

    static uint8_t context_bank(ContextKey key) {
        return static_cast<uint8_t>((key >> 56) & 0xFF);
    }

    static uint16_t context_offset(ContextKey key) {
        return static_cast<uint16_t>((key >> 40) & 0xFFFF);
    }

    static std::array<uint8_t, 5> context_mmu(ContextKey key) {
        std::array<uint8_t, 5> mmu{};
        mmu[0] = static_cast<uint8_t>((key >> 32) & 0xFF);
        mmu[1] = static_cast<uint8_t>((key >> 24) & 0xFF);
        mmu[2] = static_cast<uint8_t>((key >> 16) & 0xFF);
        mmu[3] = static_cast<uint8_t>((key >> 8) & 0xFF);
        mmu[4] = static_cast<uint8_t>(key & 0xFF);
        return mmu;
    }
    
    // Get instruction at bank:addr
    const Instruction* get_instruction(uint8_t bank, uint16_t addr,
                                       const std::array<uint8_t, 5>& mmu = {0, 1, 2, 3, 4}) const;
    
    // Get block at bank:addr
    const BasicBlock* get_block(uint8_t bank, uint16_t addr,
                                const std::array<uint8_t, 5>& mmu = {0, 1, 2, 3, 4}) const;
    
    // Get function at bank:addr
    const Function* get_function(uint8_t bank, uint16_t addr,
                                 const std::array<uint8_t, 5>& mmu = {0, 1, 2, 3, 4}) const;
};

/* ============================================================================
 * Analyzer Interface
 * ========================================================================== */

/**
 * @brief Analysis options
 */
struct AnalyzerOptions {
    // Map of RAM address -> ROM address (source of the code)
    // This allows analyzing code that is copied to RAM (e.g. OAM DMA routines)
    struct RamOverlay {
        uint16_t ram_addr;
        uint32_t rom_addr;
        uint16_t size;
    };
    std::vector<RamOverlay> ram_overlays;
    
    // Explicit list of entry points to analyze (in addition to standard ones)
    std::vector<uint32_t> entry_points;

    bool analyze_all_banks = true;      // Analyze all ROM banks
    bool detect_computed_jumps = true;  // Try to resolve JP HL targets
    bool track_bank_switches = true;    // Track bank switch operations
    bool mark_unreachable = true;       // Mark unreachable code
    
    // Debugging options
    bool trace_log = false;             // Print detailed execution trace
    bool verbose = false;               // Print verbose analysis info
    size_t max_instructions = 0;        // Max instructions to analyze (0 = infinite)
    size_t max_functions = 0;           // Max functions to discover (0 = infinite)
    
    // Feature flags
    bool aggressive_scan = true;        // Scan for unreferenced code (ON by default)
    std::string trace_file_path;        // Path to entry points trace file
};

/**
 * @brief Analyze a ROM
 * 
 * @param rom Loaded ROM
 * @param options Analysis options
 * @return Analysis result
 */
AnalysisResult analyze(const ROM& rom, const AnalyzerOptions& options = {});

/**
 * @brief Analyze a single bank
 * 
 * @param rom Loaded ROM
 * @param bank Bank number to analyze
 * @param options Analysis options
 * @return Partial analysis for that bank
 */
AnalysisResult analyze_bank(const ROM& rom, uint8_t bank,
                            const AnalyzerOptions& options = {});

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * @brief Generate function name for an address
 */
std::string generate_function_name(uint8_t bank, uint16_t address);
std::string generate_function_name(uint8_t bank, uint16_t address,
                                   const std::array<uint8_t, 5>& mmu_state);

/**
 * @brief Generate label name for an address
 */
std::string generate_label_name(uint8_t bank, uint16_t address);

/**
 * @brief Print analysis summary
 */
void print_analysis_summary(const AnalysisResult& result);

/**
 * @brief Check if address is likely data (not code)
 */
bool is_likely_data(const AnalysisResult& result, uint8_t bank, uint16_t address);

} // namespace gbrecomp

#endif // RECOMPILER_ANALYZER_H
