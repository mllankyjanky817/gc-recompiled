/**
 * @file rom.h
 * @brief game.com ROM loader and MMU-oriented metadata
 */

#ifndef RECOMPILER_ROM_H
#define RECOMPILER_ROM_H

#include "recompiler/architecture.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gbrecomp {

enum class MBCType : uint8_t {
    NONE = 0x00,
    MMU = 0x80,
    UNKNOWN = 0xFF
};

/**
 * @brief Get human-readable name for MBC type
 */
const char* mbc_type_name(MBCType type);

/**
 * @brief Check if MBC type has RAM
 */
bool mbc_has_ram(MBCType type);

/**
 * @brief Check if MBC type has battery backup
 */
bool mbc_has_battery(MBCType type);

/**
 * @brief Check if MBC type has RTC (real-time clock)
 */
bool mbc_has_rtc(MBCType type);

struct ROMHeader {
    std::string title;
    MBCType mbc_type = MBCType::MMU;
    size_t rom_size_bytes = 0;
    size_t ram_size_bytes = 0;
    uint16_t rom_banks = 0;
    uint8_t ram_banks = 0;
    bool is_cgb = false;
    bool is_cgb_only = false;
    bool is_sgb = false;
    bool header_checksum_valid = true;
    bool global_checksum_valid = true;
    bool logo_valid = true;
    uint16_t reset_entry = arch::GC_RESET_ENTRY;
    uint16_t vector_table_start = arch::GC_VECTOR_TABLE_START;
    uint8_t mmu_window_count = arch::GC_ROM_WINDOW_COUNT;
    uint16_t bank_window_size = arch::GC_ROM_WINDOW_SIZE;
};

/* ============================================================================
 * ROM Class
 * ========================================================================== */

/**
 * @brief Loaded ROM with parsed header
 */
class ROM {
public:
    /**
     * @brief Load ROM from file
     * @param path Path to ROM file
     * @return Loaded ROM or nullopt on failure
     */
    static std::optional<ROM> load(const std::filesystem::path& path);
    
    /**
     * @brief Load ROM from memory buffer
     * @param data ROM data
     * @param name ROM name (for display)
     * @return Loaded ROM or nullopt on failure
     */
    static std::optional<ROM> load_from_buffer(std::vector<uint8_t> data,
                                                const std::string& name);
    
    // Access ROM data
    const uint8_t* data() const { return data_.data(); }
    size_t size() const { return data_.size(); }
    const std::vector<uint8_t>& bytes() const { return data_; }
    
    // Read at address (with optional bank for banked addresses)
    uint8_t read(uint16_t addr) const;
    uint8_t read_banked(uint8_t bank, uint16_t addr) const;
    
    // Access header
    const ROMHeader& header() const { return header_; }
    
    // ROM info
    const std::string& name() const { return name_; }
    const std::filesystem::path& path() const { return path_; }
    
    // Validation
    bool is_valid() const { return valid_; }
    const std::string& error() const { return error_; }
    
    // Helper methods
    bool has_banking() const { return header_.rom_banks > 1; }
    bool has_ram() const { return header_.ram_size_bytes > 0; }
    uint16_t bank_count() const { return header_.rom_banks; }
    uint16_t main_entry() const { return header_.reset_entry; }
    uint16_t visible_rom_start() const { return arch::GC_ROM_WINDOW_START; }
    uint16_t visible_rom_end() const { return arch::GC_ROM_WINDOW_END; }
    uint16_t bank_window_size() const { return header_.bank_window_size; }
    bool is_visible_rom_address(uint16_t addr) const { return arch::is_visible_rom_address(addr); }

private:
    ROM() = default;
    
    bool parse_header();
    bool validate();
    
    std::vector<uint8_t> data_;
    ROMHeader header_;
    std::string name_;
    std::filesystem::path path_;
    bool valid_ = false;
    std::string error_;
};

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * @brief Validate ROM file without fully loading
 */
bool validate_rom_file(const std::filesystem::path& path, std::string& error);

/**
 * @brief Print ROM info to stdout
 */
void print_rom_info(const ROM& rom);

} // namespace gbrecomp

#endif // RECOMPILER_ROM_H
