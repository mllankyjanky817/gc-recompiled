/**
 * @file rom.cpp
 * @brief game.com ROM loader implementation
 */

#include "recompiler/rom.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace gbrecomp {

const char* mbc_type_name(MBCType type) {
    switch (type) {
        case MBCType::NONE: return "Linear";
        case MBCType::MMU: return "game.com MMU";
        default: return "UNKNOWN";
    }
}

bool mbc_has_ram(MBCType type) {
    return type == MBCType::MMU;
}

bool mbc_has_battery(MBCType type) {
    return type == MBCType::MMU;
}

bool mbc_has_rtc(MBCType type) {
    return false;
}

std::optional<ROM> ROM::load(const std::filesystem::path& path) {
    ROM rom;
    rom.path_ = path;
    rom.name_ = path.stem().string();
    
    // Open file
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        rom.error_ = "Failed to open file";
        return rom;
    }
    
    // Get file size
    auto file_size = file.tellg();
    if (file_size < arch::GC_RESET_ENTRY + 1) {
        rom.error_ = "File too small to be a valid ROM";
        return rom;
    }
    
    // Read file
    rom.data_.resize(static_cast<size_t>(file_size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(rom.data_.data()), file_size);
    
    if (!file) {
        rom.error_ = "Failed to read file";
        return rom;
    }
    
    // Parse header
    if (!rom.parse_header()) {
        return rom;
    }
    
    // Validate
    if (!rom.validate()) {
        return rom;
    }
    
    rom.valid_ = true;
    return rom;
}

std::optional<ROM> ROM::load_from_buffer(std::vector<uint8_t> data,
                                          const std::string& name) {
    ROM rom;
    rom.data_ = std::move(data);
    rom.name_ = name;
    
    if (rom.data_.size() < arch::GC_RESET_ENTRY + 1) {
        rom.error_ = "Data too small to be a valid ROM";
        return rom;
    }
    
    if (!rom.parse_header()) {
        return rom;
    }
    
    if (!rom.validate()) {
        return rom;
    }
    
    rom.valid_ = true;
    return rom;
}

bool ROM::parse_header() {
    header_.title = name_;
    header_.mbc_type = MBCType::MMU;
    header_.rom_size_bytes = data_.size();
    header_.rom_banks = static_cast<uint16_t>((data_.size() + arch::GC_PHYSICAL_BANK_SIZE - 1) /
                                              arch::GC_PHYSICAL_BANK_SIZE);
    header_.ram_size_bytes = (arch::GC_INTERNAL_RAM_END - arch::GC_INTERNAL_RAM_START) +
                             (arch::GC_EXT_RAM_END - arch::GC_EXT_RAM_START);
    header_.ram_banks = 1;
    header_.reset_entry = arch::GC_RESET_ENTRY;
    header_.vector_table_start = arch::GC_VECTOR_TABLE_START;
    header_.mmu_window_count = arch::GC_ROM_WINDOW_COUNT;
    header_.bank_window_size = arch::GC_ROM_WINDOW_SIZE;
    return true;
}

bool ROM::validate() {
    if (data_.size() < arch::GC_PHYSICAL_BANK_SIZE) {
        error_ = "ROM does not contain a full 8 KiB page";
        return false;
    }

    if (header_.rom_banks == 0) {
        error_ = "ROM contains no addressable 8 KiB pages";
        return false;
    }

    return true;
}

uint8_t ROM::read(uint16_t addr) const {
    if (addr < data_.size()) {
        return data_[addr];
    }
    return 0xFF;
}

uint8_t ROM::read_banked(uint8_t bank, uint16_t addr) const {
    if (!arch::is_visible_rom_address(addr)) {
        return 0xFF;
    }

    size_t offset = arch::rom_offset_for_bank_address(bank, addr);
    if (offset < data_.size()) {
        return data_[offset];
    }
    return 0xFF;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

bool validate_rom_file(const std::filesystem::path& path, std::string& error) {
    auto rom = ROM::load(path);
    if (!rom || !rom->is_valid()) {
        error = rom ? rom->error() : "Failed to load ROM";
        return false;
    }
    return true;
}

void print_rom_info(const ROM& rom) {
    const auto& h = rom.header();
    
    std::cout << "\nROM Information:\n";
    std::cout << "  Title:        " << h.title << "\n";
    std::cout << "  Mapper:       " << mbc_type_name(h.mbc_type) << "\n";
    std::cout << "  ROM Size:     " << (h.rom_size_bytes / 1024) << " KB (" 
              << h.rom_banks << " banks)\n";
    std::cout << "  MMU Windows:  " << (int)h.mmu_window_count << " x "
              << h.bank_window_size / 1024 << " KB\n";
    std::cout << "  Entry Point:  0x" << std::hex << std::setw(4) << std::setfill('0')
              << h.reset_entry << std::dec << "\n";
    std::cout << "  Vector Table: 0x" << std::hex << std::setw(4) << std::setfill('0')
              << h.vector_table_start << "-0x" << std::setw(4)
              << (arch::GC_VECTOR_TABLE_END - 1) << std::dec << "\n";
}

} // namespace gbrecomp
