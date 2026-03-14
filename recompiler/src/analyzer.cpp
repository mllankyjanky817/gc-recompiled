/**
 * @file analyzer.cpp
 * @brief Control flow analyzer implementation (stub for MVP)
 */

#include "recompiler/analyzer.h"
#include <algorithm>
#include <queue>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <set>
#include <map>
#include <fstream>

namespace gbrecomp {

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static uint32_t make_address(uint8_t bank, uint16_t addr) {
    return (static_cast<uint32_t>(bank) << 16) | addr;
}

static uint8_t get_bank(uint32_t addr) {
    return static_cast<uint8_t>(addr >> 16);
}

static uint16_t get_offset(uint32_t addr) {
    return static_cast<uint16_t>(addr & 0xFFFF);
}

static std::array<uint8_t, 5> to_mmu_array(const uint8_t mmu[5]) {
    return {mmu[0], mmu[1], mmu[2], mmu[3], mmu[4]};
}

static AnalysisResult::ContextKey make_context_key(uint8_t bank, uint16_t addr, const uint8_t mmu[5]) {
    return AnalysisResult::make_context_addr(bank, addr, to_mmu_array(mmu));
}

/* ============================================================================
 * AnalysisResult Implementation
 * ========================================================================== */

const Instruction* AnalysisResult::get_instruction(uint8_t bank, uint16_t addr,
                                                   const std::array<uint8_t, 5>& mmu) const {
    ContextKey full_addr = make_context_addr(bank, addr, mmu);
    auto it = addr_to_index.find(full_addr);
    if (it != addr_to_index.end() && it->second < instructions.size()) {
        return &instructions[it->second];
    }
    return nullptr;
}

const BasicBlock* AnalysisResult::get_block(uint8_t bank, uint16_t addr,
                                            const std::array<uint8_t, 5>& mmu) const {
    ContextKey full_addr = make_context_addr(bank, addr, mmu);
    auto it = blocks.find(full_addr);
    if (it != blocks.end()) {
        return &it->second;
    }
    return nullptr;
}

const Function* AnalysisResult::get_function(uint8_t bank, uint16_t addr,
                                             const std::array<uint8_t, 5>& mmu) const {
    ContextKey full_addr = make_context_addr(bank, addr, mmu);
    auto it = functions.find(full_addr);
    if (it != functions.end()) {
        return &it->second;
    }
    return nullptr;
}

/* ============================================================================
 * RST Pattern Detection
 * ========================================================================== */

/* ============================================================================
 * Internal State Tracking
 * ========================================================================== */

// Track addresses to explore: (addr, known_a, ..., current_bank_context)
// Track addresses to explore with per-path register constant state
struct AnalysisState {
    uint32_t addr;
    int known_r[16];   // known value of r0-r15 (-1 = unknown)
    uint8_t mmu[5];    // MMU window state (windows 0-4 map 0x1000-0x9FFF)
};

static AnalysisState make_state(uint32_t addr) {
    AnalysisState s;
    s.addr = addr;
    for (int i = 0; i < 16; i++) s.known_r[i] = -1;
    for (int i = 0; i < 5;  i++) s.mmu[i] = static_cast<uint8_t>(i); // identity mapping
    return s;
}

static AnalysisState copy_state_with_addr(const AnalysisState& src, uint32_t addr) {
    AnalysisState s = src;
    s.addr = addr;
    return s;
}

/* ============================================================================
 * Bank Switch Detection
 * ========================================================================== */

/**
 * @brief Detect immediate bank values from common patterns
 * 
 * Looks for patterns like:
 *   LD A, n      ; n is bank number
 *   LD (2000), A ; or LD (2100), A, etc.
  *   LD r0, n      ; n is bank number
  *   LD (0024), r0 ; or through any MMU window register 0x0024-0x0028
 */
static std::set<uint8_t> detect_bank_values(const ROM& rom) {
    std::set<uint8_t> banks;
    // Enumerate banks that appear as literal LD_R_IMM / LD_DIR_R pairs targeting
    // MMU window registers 0x0024-0x0028.  Fallback: all banks present in ROM.
    const size_t bank_size = arch::GC_ROM_WINDOW_SIZE;
    const size_t nbanks = (rom.size() + bank_size - 1) / bank_size;
    for (size_t i = 0; i < nbanks && i < 256; i++) {
        banks.insert(static_cast<uint8_t>(i));
    }
    return banks;
}

/**
 * @brief Calculate Shannon entropy of a memory region
 */
static double calculate_entropy(const ROM& rom, uint8_t bank, uint16_t addr, size_t len) {
    if (!rom.is_visible_rom_address(addr) || addr + len > rom.visible_rom_end()) return 0.0;
    
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < len; i++) {
        counts[rom.read_banked(bank, addr + i)]++;
    }
    
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            double p = (double)counts[i] / len;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

/**
 * @brief Heuristic check if an address looks like valid code start
 * 
 * Checks for:
 * 1. Shannon Entropy (filtering tile data / PCM)
 * 2. Repetitive byte patterns
 * 3. Illegal address access/jumps
 * 4. Instruction density (loads vs math/control flow)
 */
static int is_likely_valid_code(const ROM& rom, uint8_t bank, uint16_t addr) {
    // 1. Shannon Entropy Check
    // Typical code has moderate entropy (3.0-6.0). 
    // Data like tilemaps is very low (< 2.0). PCM is very high (> 7.5).
    double entropy = calculate_entropy(rom, bank, addr, 48);
    if (entropy < 1.8 || entropy > 7.6) return 0;

    // 2. Check for repetitive patterns (e.g. tile data)
    const int PATTERN_CHECK_LEN = 128;
    if (addr + PATTERN_CHECK_LEN < rom.visible_rom_end()) {
        for (int period = 1; period <= 8; period++) {
            const int REQUIRED_REPEATS = 16;
            const int REQUIRED_LEN = period * REQUIRED_REPEATS;
            
            bool matches = true;
            for (int i = 0; i < REQUIRED_LEN; i++) {
                if (rom.read_banked(bank, addr + i) != rom.read_banked(bank, addr + i + period)) {
                    matches = false;
                    break;
                }
            }
            if (matches) return 0;
        }
    }

    // 3. Decode instructions
    Decoder decoder(rom);
    uint16_t curr = addr;
    int instructions_checked = 0;
    const int MAX_CHECK = 64;
    int nop_count = 0;
    int ld_count = 0;
    int control_flow_count = 0;
    int math_count = 0;

    while (instructions_checked < MAX_CHECK) {
        Instruction instr = decoder.decode(curr, bank);
        
        if (instr.type == InstructionType::UNDEFINED || instr.type == InstructionType::INVALID) return 0;
        
        if (instr.type == InstructionType::NOP) {
            nop_count++;
            if (nop_count > 4) return 0; // Too many NOPs
        }
        
        // 4. Illegal address check
        if (instr.type == InstructionType::LD_R_DIR || instr.type == InstructionType::LD_DIR_R ||
            instr.type == InstructionType::LDW_RR_NN ||
            instr.is_call || instr.is_jump) {
            
            uint16_t imm = (instr.type == InstructionType::JR_E || instr.type == InstructionType::JRC_CC_E) ? 0 : instr.imm16;
            if (imm != 0) {
                if (imm >= arch::GC_VRAM_START && imm < arch::GC_EXT_RAM_END) return 0;
            }
        }

        if (instr.reads_memory || instr.writes_memory) ld_count++;
        if (instr.is_call || instr.is_jump || instr.is_return) control_flow_count++;
        
            // Math/Logic (SM85CPU ALU opcodes 0x80-0xBF, shifts 0xB0-0xB6)
            if ((instr.opcode >= 0x80 && instr.opcode <= 0xBF)) math_count++;

        // Reject rare/data-like opcodes if too frequent at start
        // No SM85CPU-specific opcode heuristics needed here

        // Terminator Check
        if (instr.is_return && !instr.is_conditional) {
            if (instructions_checked < 2) return 0; 
            // Avoid load-only functions discovered via scanning
            if (ld_count >= instructions_checked && instructions_checked > 2) return 0;
            return (curr + instr.length - addr);
        }
        
        if (instr.is_jump && !instr.is_conditional) {
             if (instructions_checked >= 3) return (curr + instr.length - addr);
             return 0;
        }
        
        curr += instr.length;
        if (curr >= rom.visible_rom_end()) return 0;
        instructions_checked++;
        
        // High density of loads (indicative of data or large tables)
        if (instructions_checked >= 15 && ld_count == instructions_checked) return 0;
    }

    return 0;
}

/**
 * @brief Scan for 16-bit pointers that likely lead to code
 */
static void find_pointer_entry_points(const ROM& rom, AnalysisResult& result,
                                      std::queue<AnalysisState>& work_queue) {
    for (uint16_t addr = arch::GC_RESET_ENTRY; addr + 1 < rom.visible_rom_end(); addr++) {
        uint8_t lo = rom.read_banked(0, addr);
        uint8_t hi = rom.read_banked(0, addr + 1);
        uint16_t target = lo | (hi << 8);

        if (rom.is_visible_rom_address(target)) {
            uint8_t tbank = 0;
            if (is_likely_valid_code(rom, tbank, target)) {
                const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
                auto full_addr = make_context_key(tbank, target, identity_mmu);
                if (result.call_targets.find(full_addr) == result.call_targets.end()) {
                    result.call_targets.insert(full_addr);
                    work_queue.push(make_state(make_address(tbank, target)));
                }
            }
        }
    }
}

/* ============================================================================
 * Analysis Implementation
 * ========================================================================== */

/**
 * @brief Load entry points from a runtime trace file
 */
static void load_trace_entry_points(const std::string& path,
                                    std::set<AnalysisResult::ContextKey>& call_targets) {
    if (path.empty()) return;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open trace file: " << path << "\n";
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            try {
                int bank = std::stoi(line.substr(0, colon));
                int addr = std::stoi(line.substr(colon + 1), nullptr, 16);
                const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
                call_targets.insert(make_context_key(static_cast<uint8_t>(bank),
                                                     static_cast<uint16_t>(addr),
                                                     identity_mmu));
                count++;
            } catch (...) {
                continue;
            }
        }
    }
    std::cout << "Loaded " << count << " entry points from trace file: " << path << "\n";
}

AnalysisResult analyze(const ROM& rom, const AnalyzerOptions& options) {
    AnalysisResult result;
    result.rom = &rom;
    result.entry_point = rom.main_entry();
    
    Decoder decoder(rom);
    
    // Detect which banks are used
    std::set<uint8_t> known_banks = detect_bank_values(rom);
    
    std::queue<AnalysisState> work_queue;
    std::set<AnalysisResult::ContextKey> visited;
    // Pointer scanning pass
    find_pointer_entry_points(rom, result, work_queue);
    
    {
        const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
        result.call_targets.insert(make_context_key(0, rom.main_entry(), identity_mmu));
    }

    for (uint16_t vec_addr = arch::GC_VECTOR_TABLE_START;
         vec_addr + 1 < arch::GC_VECTOR_TABLE_END;
         vec_addr += 2) {
        uint16_t target = static_cast<uint16_t>(rom.read_banked(0, vec_addr)) |
                          (static_cast<uint16_t>(rom.read_banked(0, vec_addr + 1)) << 8);
        if (!rom.is_visible_rom_address(target)) continue;
        result.interrupt_vectors.push_back(target);
        const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
        result.call_targets.insert(make_context_key(0, target, identity_mmu));
    }

    // Load from trace if provided
    load_trace_entry_points(options.trace_file_path, result.call_targets);

    // Initial work queue seeding
    for (auto target : result.call_targets) {
        uint8_t bank = AnalysisResult::context_bank(target);
        uint16_t off = AnalysisResult::context_offset(target);
        AnalysisState state = make_state(make_address(bank, off));
        auto mmu = AnalysisResult::context_mmu(target);
        for (size_t i = 0; i < 5; i++) {
            state.mmu[i] = mmu[i];
        }
        work_queue.push(state);
    }
    
    // Manual entry points
    for (uint32_t target : options.entry_points) {
        uint8_t bank = get_bank(target);
        uint16_t off = get_offset(target);
        const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
        auto key = make_context_key(bank, off, identity_mmu);
        if (result.call_targets.find(key) == result.call_targets.end()) {
            result.call_targets.insert(key);
            work_queue.push(make_state(target));
        }
    }

    if (options.analyze_all_banks) {
        std::cerr << "Analyzing all " << known_banks.size() << " banks\n";
        for (uint8_t bank : known_banks) {
            if (is_likely_valid_code(rom, bank, arch::GC_ROM_WINDOW_START)) {
                work_queue.push(make_state(make_address(bank, arch::GC_ROM_WINDOW_START)));
                const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
                result.call_targets.insert(make_context_key(bank, arch::GC_ROM_WINDOW_START, identity_mmu));
            }
        }
    }
    
    // Add overlay entry points
    for (const auto& ov : options.ram_overlays) {
        uint32_t addr = make_address(0, ov.ram_addr);
        const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
        result.call_targets.insert(make_context_key(0, ov.ram_addr, identity_mmu));
        work_queue.push(make_state(addr));
    }

    // Add manual entry points
    for (uint32_t addr : options.entry_points) {
        uint8_t bank = get_bank(addr);
        uint16_t off = get_offset(addr);
        const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
        result.call_targets.insert(make_context_key(bank, off, identity_mmu));
        work_queue.push(make_state(addr));
    }
    
    // Multi-pass analysis
    bool scanning_pass = false;

    // Explore all reachable code
    while (true) {
        // Drain work queue
        while (!work_queue.empty()) {
            auto item = work_queue.front();
        work_queue.pop();
        
        uint32_t addr = item.addr;
        int known_r[16];
        uint8_t current_mmu[5];
        for (int i = 0; i < 16; i++) known_r[i] = item.known_r[i];
        for (int i = 0; i < 5;  i++) current_mmu[i] = item.mmu[i];
        // current_switchable_bank: derive from MMU window 0 (maps 0x1000-0x2FFF)
        uint8_t current_switchable_bank = current_mmu[0];
        
        uint8_t bank = get_bank(addr);
        uint16_t offset = get_offset(addr);
        auto state_key = make_context_key(bank, offset, current_mmu);
        if (visited.count(state_key)) continue;
        
        // Check if inside any RAM overlay
        const AnalyzerOptions::RamOverlay* overlay = nullptr;
        for (const auto& ov : options.ram_overlays) {
            if (offset >= ov.ram_addr && offset < ov.ram_addr + ov.size) {
                overlay = &ov;
                break;
            }
        }
        
        if (!rom.is_visible_rom_address(offset) && !overlay) continue;
        if (overlay && visited.count(state_key)) continue;

        // Calculate ROM offset
        size_t rom_offset;
        if (overlay) {
            uint8_t src_bank = get_bank(overlay->rom_addr);
            uint16_t src_addr = get_offset(overlay->rom_addr);
            rom_offset = arch::rom_offset_for_bank_address(src_bank, src_addr);
            rom_offset += (offset - overlay->ram_addr);
        } else {
            rom_offset = arch::rom_offset_for_bank_address(bank, offset);
        }
        if (rom_offset >= rom.size()) continue;
        
        visited.insert(state_key);
        
        // Decode instruction
        Instruction instr;
        if (overlay) {
             uint8_t src_bank = get_bank(overlay->rom_addr);
             uint16_t src_addr = get_offset(overlay->rom_addr) + (offset - overlay->ram_addr);
             instr = decoder.decode(src_addr, src_bank);
             instr.address = offset; 
             instr.bank = 0; // RAM is bank 0
        } else {
            instr = decoder.decode(offset, bank);
        }

        /* -------------------------------------------------------------
         * Constant Propagation (A and HL)
         * ------------------------------------------------------------- */

        // SM85CPU: LD rN, imm -> track known_r[N]
        if (instr.type == InstructionType::LD_R_IMM) {
            known_r[instr.reg8_dst] = instr.imm8;
        }
        // SM85CPU: LD rD, rS -> propagate source value
        else if (instr.type == InstructionType::LD_R_R) {
            known_r[instr.reg8_dst] = known_r[instr.reg8_src];
        }
        // All ALU / POP / indirect loads invalidate the destination register
        else if (instr.type == InstructionType::ADD_R_R  || instr.type == InstructionType::ADDC_R_R  ||
                 instr.type == InstructionType::SUB_R_R  || instr.type == InstructionType::SUBC_R_R  ||
                 instr.type == InstructionType::AND_R_R  || instr.type == InstructionType::OR_R_R    ||
                 instr.type == InstructionType::XOR_R_R  || instr.type == InstructionType::ADD_R_IMM ||
                 instr.type == InstructionType::ADDC_R_IMM || instr.type == InstructionType::SUB_R_IMM ||
                 instr.type == InstructionType::SUBC_R_IMM || instr.type == InstructionType::AND_R_IMM ||
                 instr.type == InstructionType::OR_R_IMM  || instr.type == InstructionType::XOR_R_IMM ||
                 instr.type == InstructionType::INC_R    || instr.type == InstructionType::DEC_R     ||
                 instr.type == InstructionType::LD_R_IND || instr.type == InstructionType::LD_R_IDX  ||
                 instr.type == InstructionType::LD_R_DIR || instr.type == InstructionType::POP_RR) {
            if (instr.reg8_dst < 16) known_r[instr.reg8_dst] = -1;
        }

        /* -------------------------------------------------------------
         * Bank Switching Detection
         * ------------------------------------------------------------- */
        // SM85CPU: LD (nn), rN where nn is 0x0024-0x0028 = MMU window write
        if (instr.type == InstructionType::LD_DIR_R &&
            instr.imm16 >= 0x0024 && instr.imm16 <= 0x0028) {
            uint8_t win     = static_cast<uint8_t>(instr.imm16 - 0x0024);
            uint8_t src_reg = instr.reg8_src;
            bool is_dynamic = (src_reg >= 16 || known_r[src_reg] == -1);
            uint8_t new_bank = is_dynamic ? 1 : static_cast<uint8_t>(known_r[src_reg]);

            result.bank_tracker.record_bank_switch(addr, new_bank, is_dynamic);

            if (!is_dynamic) {
                current_mmu[win] = new_bank;
                current_switchable_bank = current_mmu[0];
            }
        }

        // Trace logging
        if (options.trace_log) {
            std::cout << "[TRACE] " << std::hex << std::setfill('0') << std::setw(2) << (int)bank
                      << ":" << std::setw(4) << offset << " " << instr.disassemble() << std::dec << "\n";
        }
        
        // Check padding
        if (bank > 0 && instr.opcode == 0xFF) {
            bool is_padding = true;
            for (int i = 1; i < 16; i++) {
                if (rom.read_banked(bank, offset + i) != 0xFF) { is_padding = false; break; }
            }
            if (is_padding) continue;
        }

        if (instr.type == InstructionType::UNDEFINED) {
             std::cout << "[ERROR] Undefined instruction at " << std::hex << (int)bank << ":" << offset << "\n";
             continue;
        }

        if (options.max_instructions > 0 && result.instructions.size() >= options.max_instructions) {
            break;
        }
        
        size_t idx = result.instructions.size();
        result.instructions.push_back(instr);
        result.addr_to_index[state_key] = idx;

        AnalysisState base_state = item;
        for (int i = 0; i < 16; i++) base_state.known_r[i] = known_r[i];
        for (int i = 0; i < 5;  i++) base_state.mmu[i] = current_mmu[i];
        
        auto target_bank = [&](uint16_t target) -> uint8_t {
            if (!rom.is_visible_rom_address(target)) return 0;
            return current_switchable_bank;
        };

        if (instr.is_call) {
            uint16_t target = instr.imm16;
            uint8_t tbank = target_bank(target);
            instr.resolved_target_bank = tbank;
            
            if (tbank > 0 && tbank != bank) {
                if (!is_likely_valid_code(rom, tbank, target)) continue;
            }

            auto target_key = make_context_key(tbank, target, base_state.mmu);
            result.call_targets.insert(target_key);
            work_queue.push(make_state(make_address(tbank, target)));
            
            if (tbank != bank) {
                result.stats.cross_bank_calls++;
                result.bank_tracker.record_cross_bank_call(offset, target, bank, tbank);
            }
            
            uint32_t fall_through = make_address(bank, offset + instr.length);
            result.label_addresses.insert(make_context_key(bank, static_cast<uint16_t>(offset + instr.length), base_state.mmu));
            work_queue.push(copy_state_with_addr(base_state, fall_through));
        } else if (instr.is_jump) {
            if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JPC_CC_NN) {
                uint16_t target = instr.imm16;
                uint8_t tbank = target_bank(target);
                instr.resolved_target_bank = tbank;
                if (rom.is_visible_rom_address(target)) {
                    if (tbank > 0 && tbank != bank) {
                        if (!is_likely_valid_code(rom, tbank, target)) continue;
                    }
                    result.call_targets.insert(make_context_key(tbank, target, base_state.mmu));
                }
                result.label_addresses.insert(make_context_key(tbank, target, base_state.mmu));
                work_queue.push(copy_state_with_addr(base_state, make_address(tbank, target)));
            } else if (instr.type == InstructionType::JR_E || instr.type == InstructionType::JRC_CC_E ||
                       instr.type == InstructionType::DBNZ_R_E) {
                uint16_t target = offset + instr.length + instr.offset;
                result.label_addresses.insert(make_context_key(bank, target, base_state.mmu));
                work_queue.push(copy_state_with_addr(base_state, make_address(bank, target)));
            } else if (instr.type == InstructionType::JP_RR) {
                // Indirect jump target unknown at analysis time.
                result.computed_jump_targets.insert(make_context_key(bank, offset, base_state.mmu));
            }
            
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(make_context_key(bank, static_cast<uint16_t>(offset + instr.length), base_state.mmu));
                work_queue.push(copy_state_with_addr(base_state, fall_through));
            }
        } else if (instr.is_return) {
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(make_context_key(bank, static_cast<uint16_t>(offset + instr.length), base_state.mmu));
                work_queue.push(copy_state_with_addr(base_state, fall_through));
            }
        } else {
            work_queue.push(copy_state_with_addr(base_state, make_address(bank, offset + instr.length)));
        }
    } // End work_queue loop

    // Aggressive Code Scanning
    if (options.aggressive_scan && !scanning_pass) {
        scanning_pass = true; // prevent infinite loops if we find nothing new
        
        if (options.verbose) std::cout << "[ANALYSIS] Starting aggressive scan for missing code..." << std::endl;
        
        size_t found_count = 0;

        // Iterate through all known banks (and bank 0)
        std::vector<uint8_t> banks_to_scan;
        banks_to_scan.push_back(0);
        for (uint8_t b : known_banks) if (b > 0) banks_to_scan.push_back(b);

        // Track regions found by aggressive scanning to avoid overlapping detection in future passes
        // (Since operands are not marked as 'visited' by the main analysis)
        static std::set<uint32_t> aggressive_regions; 

        for (uint8_t bank : banks_to_scan) {
            uint16_t start_addr = arch::GC_ROM_WINDOW_START;
            uint16_t end_addr = static_cast<uint16_t>(rom.visible_rom_end() - 1);
            
            for (uint32_t addr = start_addr; addr <= end_addr; ) {
                const uint8_t identity_mmu[5] = {0, 1, 2, 3, 4};
                auto full_addr = make_context_key(bank, static_cast<uint16_t>(addr), identity_mmu);
                
                // If already visited by ANY means, skip
                if (visited.count(full_addr) || aggressive_regions.count(full_addr)) {
                    addr++; 
                    continue;
                }
                
                // Alignment heuristic: most functions start on some boundary? No.
                // But we can skip obvious padding (0xFF or 0x00)
                uint8_t byte = rom.read_banked(bank, addr);
                if (byte == 0xFF || byte == 0x00) {
                    addr++;
                    continue;
                }

                // Check if this looks like valid code
                int code_len = is_likely_valid_code(rom, bank, addr);
                if (code_len > 0) {
                    if (options.verbose) {
                        std::cout << "[ANALYSIS] Detected potential function at " 
                                  << std::hex << (int)bank << ":" << addr << std::dec << "\n";
                    }
                    
                    // Add as a new entry point
                    uint32_t entry = make_address(bank, addr);
                    auto entry_key = make_context_key(bank, static_cast<uint16_t>(addr), identity_mmu);
                    result.call_targets.insert(entry_key);
                    
                    // Add to queue
                    work_queue.push(make_state(entry));
                    found_count++;
                    
                    // Mark region as scanned
                    for (int i = 0; i < code_len; i++) {
                        aggressive_regions.insert(make_address(bank, addr + i));
                    }
                    
                    // Skip the block we just found to avoid overlapping detection
                    addr += code_len;
                    continue;
                } else {
                    // Not valid code, skip ahead.
                    addr++;
                }
            }
        }
        
        if (found_count > 0) {
            if (options.verbose) std::cout << "[ANALYSIS] Found " << found_count << " new entry points. Restarting analysis." << std::endl;
            scanning_pass = false; // Reset pass flag to allow further scanning after this batch is analyzed
            continue; // Go back to work_queue processing
        }
    }
    
    // If we get here, we are done
    break; 
    } // End while(true)
    
    // Build basic blocks from instruction boundaries
    std::set<AnalysisResult::ContextKey> block_starts;
    
    for (auto target : result.call_targets) {
        block_starts.insert(target);
    }
    for (auto target : result.label_addresses) {
        block_starts.insert(target);
    }
    
    // Create blocks
    for (auto start : block_starts) {
        if (!visited.count(start)) continue;
        
        BasicBlock block;
        block.context_key = start;
        block.start_address = AnalysisResult::context_offset(start);
        block.bank = AnalysisResult::context_bank(start);
        block.mmu_state = AnalysisResult::context_mmu(start);
        block.is_reachable = true;
        
        if (result.call_targets.count(start)) {
            block.is_function_entry = true;
        }
        
        // Find instructions in this block
        uint32_t curr = make_address(block.bank, block.start_address);
        while (true) {
            auto curr_key = make_context_key(block.bank, get_offset(curr), block.mmu_state.data());
            if (!visited.count(curr_key)) break;

            auto it = result.addr_to_index.find(curr_key);
            if (it == result.addr_to_index.end()) break;
            
            block.instruction_indices.push_back(it->second);
            const Instruction& instr = result.instructions[it->second];
            
            block.end_address = get_offset(curr) + instr.length;
            
            // Track successors for control flow
            if (instr.is_jump) {
                if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JPC_CC_NN) {
                    block.successors.push_back(instr.imm16);
                    uint8_t succ_bank = (instr.resolved_target_bank == 0xFF) ? block.bank : instr.resolved_target_bank;
                    block.successor_keys.push_back(make_context_key(succ_bank, instr.imm16, block.mmu_state.data()));
                } else if (instr.type == InstructionType::JR_E || instr.type == InstructionType::JRC_CC_E ||
                           instr.type == InstructionType::DBNZ_R_E) {
                    uint16_t target = get_offset(curr) + instr.length + instr.offset;
                    block.successors.push_back(target);
                    block.successor_keys.push_back(make_context_key(block.bank, target, block.mmu_state.data()));
                }
                // Conditional jumps also fall through
                if (instr.is_conditional) {
                    uint16_t fallthrough = get_offset(curr) + instr.length;
                    block.successors.push_back(fallthrough);
                    block.successor_keys.push_back(make_context_key(block.bank, fallthrough, block.mmu_state.data()));
                }
            } else if (instr.is_return && instr.is_conditional) {
                // Conditional returns fall through if condition is false
                uint16_t fallthrough = get_offset(curr) + instr.length;
                block.successors.push_back(fallthrough);
                block.successor_keys.push_back(make_context_key(block.bank, fallthrough, block.mmu_state.data()));
            }
            
            // Check if this ends the block
            if (instr.is_jump || instr.is_return || instr.is_call) {
                // CALLs fall through to next instruction after return
                if (instr.is_call) {
                    uint16_t fallthrough = get_offset(curr) + instr.length;
                    block.successors.push_back(fallthrough);
                    block.successor_keys.push_back(make_context_key(block.bank, fallthrough, block.mmu_state.data()));
                }
                break;
            }
            
            curr = make_address(block.bank, get_offset(curr) + instr.length);
            auto next_key = make_context_key(block.bank, get_offset(curr), block.mmu_state.data());
            
            // Check if next instruction starts a new block
            if (block_starts.count(next_key)) {
                // Fall through to the new block - add as successor
                block.successors.push_back(get_offset(curr));
                block.successor_keys.push_back(next_key);
                break;
            }
        }
        
        result.blocks[start] = block;
    }
    
    // Create functions from call targets with better merging logic
    std::set<AnalysisResult::ContextKey> processed_targets;
    
    // Function size threshold for merging (in instructions)
    const int MIN_FUNCTION_SIZE = 3;
    
    for (auto target : result.call_targets) {
        if (processed_targets.count(target)) continue;
        
        auto block_it = result.blocks.find(target);
        if (block_it == result.blocks.end()) continue;
        
        Function func;
        func.context_key = target;
        func.entry_address = AnalysisResult::context_offset(target);
        func.bank = AnalysisResult::context_bank(target);
        func.mmu_state = AnalysisResult::context_mmu(target);
        func.name = generate_function_name(func.bank, func.entry_address, func.mmu_state);
        func.block_addresses.push_back(target);
        
        // Add all blocks reachable from this function (simple DFS)
        std::queue<AnalysisResult::ContextKey> func_queue;
        std::set<AnalysisResult::ContextKey> func_visited;
        func_queue.push(target);
        
        while (!func_queue.empty()) {
            auto block_addr = func_queue.front();
            func_queue.pop();
            
            if (func_visited.count(block_addr)) continue;
            func_visited.insert(block_addr);
            
            auto blk = result.blocks.find(block_addr);
            if (blk == result.blocks.end()) continue;
            
            // Add this block to function if not already there
            if (block_addr != target) {
                func.block_addresses.push_back(block_addr);
            }
            
            // Mark all reachable call targets as processed to avoid creating separate functions
            if (result.call_targets.count(block_addr) && block_addr != target) {
                processed_targets.insert(block_addr);
            }
            
            // Follow successors
            for (auto succ_addr : blk->second.successor_keys) {
                if (!func_visited.count(succ_addr)) {
                    func_queue.push(succ_addr);
                }
            }
        }
        
        result.functions[target] = func;
        processed_targets.insert(target);
    }
    
    // Post-process: Merge very small functions into their callers
    // This reduces the number of single-instruction functions
    std::map<AnalysisResult::ContextKey, Function> merged_functions = result.functions;
    std::set<AnalysisResult::ContextKey> functions_to_remove;
    
    for (const auto& [func_addr, func] : result.functions) {
        if (functions_to_remove.count(func_addr)) continue;
        
        // Calculate total number of instructions in function
        int total_instrs = 0;
        for (auto block_addr : func.block_addresses) {
            auto blk_it = result.blocks.find(block_addr);
            if (blk_it != result.blocks.end()) {
                total_instrs += blk_it->second.instruction_indices.size();
            }
        }
        
        // If function is too small and not a special entry point, consider merging
        bool is_special_entry = (func.bank == 0 && (
            func.entry_address == arch::GC_RESET_ENTRY ||
            (func.entry_address >= arch::GC_VECTOR_TABLE_START &&
             func.entry_address < arch::GC_VECTOR_TABLE_END)
        ));
        
        if (total_instrs < MIN_FUNCTION_SIZE && !is_special_entry) {
            functions_to_remove.insert(func_addr);
        }
    }
    
    // Remove small functions
    for (auto func_addr : functions_to_remove) {
        merged_functions.erase(func_addr);
        // Also remove from call_targets so they don't get re-created
        result.call_targets.erase(func_addr);
    }
    
    result.functions = merged_functions;
    
    // Update stats
    result.stats.total_instructions = result.instructions.size();
    result.stats.total_blocks = result.blocks.size();
    result.stats.total_functions = result.functions.size();
    
    return result;
}

AnalysisResult analyze_bank(const ROM& rom, uint8_t bank, const AnalyzerOptions& options) {
    (void)bank; // Unused parameter
    // For now, just analyze the whole ROM
    // TODO: Filter to specific bank
    return analyze(rom, options);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

std::string generate_function_name(uint8_t bank, uint16_t address) {
    const std::array<uint8_t, 5> identity = {0, 1, 2, 3, 4};
    return generate_function_name(bank, address, identity);
}

std::string generate_function_name(uint8_t bank, uint16_t address,
                                   const std::array<uint8_t, 5>& mmu_state) {
    std::ostringstream ss;
    
    if (bank == 0) {
        switch (address) {
            case arch::GC_RESET_ENTRY:
                if (mmu_state == std::array<uint8_t, 5>{0, 1, 2, 3, 4}) {
                    return "gc_main";
                }
                break;
        }
        if (address >= arch::GC_VECTOR_TABLE_START && address < arch::GC_VECTOR_TABLE_END) {
            ss << "irq_vector_" << std::hex << std::setfill('0') << std::setw(4) << address
               << "_m"
               << std::setw(2) << (int)mmu_state[0]
               << std::setw(2) << (int)mmu_state[1]
               << std::setw(2) << (int)mmu_state[2]
               << std::setw(2) << (int)mmu_state[3]
               << std::setw(2) << (int)mmu_state[4];
            return ss.str();
        }
    }
    
    ss << "func_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address
       << "_m"
       << std::setw(2) << (int)mmu_state[0]
       << std::setw(2) << (int)mmu_state[1]
       << std::setw(2) << (int)mmu_state[2]
       << std::setw(2) << (int)mmu_state[3]
       << std::setw(2) << (int)mmu_state[4];
    return ss.str();
}

std::string generate_label_name(uint8_t bank, uint16_t address) {
    std::ostringstream ss;
    ss << "loc_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address;
    return ss.str();
}

void print_analysis_summary(const AnalysisResult& result) {
    std::cout << "=== Analysis Summary ===" << std::endl;
    std::cout << "Total instructions: " << result.stats.total_instructions << std::endl;
    std::cout << "Total basic blocks: " << result.stats.total_blocks << std::endl;
    std::cout << "Total functions: " << result.stats.total_functions << std::endl;
    std::cout << "Call targets: " << result.call_targets.size() << std::endl;
    std::cout << "Label addresses: " << result.label_addresses.size() << std::endl;
    std::cout << "Bank switches detected: " << result.bank_tracker.switches().size() << std::endl;
    std::cout << "Cross-bank calls tracked: " << result.bank_tracker.calls().size() << std::endl;
    
    std::cout << "\nFunctions found:" << std::endl;
    for (const auto& [addr, func] : result.functions) {
        std::cout << "  " << func.name << " @ ";
        if (func.bank > 0) {
            std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)func.bank << ":";
        }
        std::cout << std::hex << std::setfill('0') << std::setw(4) << func.entry_address << std::endl;
    }
}

bool is_likely_data(const AnalysisResult& result, uint8_t bank, uint16_t address) {
    std::array<uint8_t, 5> identity = {0, 1, 2, 3, 4};
    auto full_addr = AnalysisResult::make_context_addr(bank, address, identity);
    return result.addr_to_index.find(full_addr) == result.addr_to_index.end();
}

} // namespace gbrecomp
