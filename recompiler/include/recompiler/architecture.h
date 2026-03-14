#ifndef RECOMPILER_ARCHITECTURE_H
#define RECOMPILER_ARCHITECTURE_H

#include <cstddef>
#include <cstdint>

namespace gbrecomp::arch {

constexpr uint16_t GC_VECTOR_TABLE_START = 0x1000;
constexpr uint16_t GC_VECTOR_TABLE_END = 0x1020;
constexpr uint16_t GC_RESET_ENTRY = 0x1020;

constexpr uint16_t GC_ROM_WINDOW_START = 0x1000;
constexpr uint16_t GC_ROM_WINDOW_SIZE = 0x2000;
constexpr uint16_t GC_ROM_WINDOW_COUNT = 5;
constexpr uint16_t GC_ROM_WINDOW_END =
    GC_ROM_WINDOW_START + GC_ROM_WINDOW_SIZE * GC_ROM_WINDOW_COUNT;

constexpr uint16_t GC_REGISTER_FILE_END = 0x0080;
constexpr uint16_t GC_INTERNAL_RAM_START = 0x0080;
constexpr uint16_t GC_INTERNAL_RAM_END = 0x0400;
constexpr uint16_t GC_VRAM_START = 0xA000;
constexpr uint16_t GC_VRAM_END = 0xE000;
constexpr uint16_t GC_EXT_RAM_START = 0xE000;
constexpr uint16_t GC_EXT_RAM_END = 0x10000;

constexpr size_t GC_PHYSICAL_BANK_SIZE = GC_ROM_WINDOW_SIZE;
constexpr uint8_t GC_DEFAULT_MMU0_BANK = 0;
constexpr uint8_t GC_DEFAULT_MMU1_BANK = 1;
constexpr uint8_t GC_DEFAULT_MMU2_BANK = 2;
constexpr uint8_t GC_DEFAULT_MMU3_BANK = 3;
constexpr uint8_t GC_DEFAULT_MMU4_BANK = 4;

inline bool is_visible_rom_address(uint16_t addr) {
    return addr >= GC_ROM_WINDOW_START && addr < GC_ROM_WINDOW_END;
}

inline uint8_t rom_window_index(uint16_t addr) {
    return static_cast<uint8_t>((addr - GC_ROM_WINDOW_START) / GC_ROM_WINDOW_SIZE);
}

inline uint16_t rom_window_offset(uint16_t addr) {
    return static_cast<uint16_t>((addr - GC_ROM_WINDOW_START) % GC_ROM_WINDOW_SIZE);
}

inline size_t rom_offset_for_bank_address(uint8_t bank, uint16_t addr) {
    return static_cast<size_t>(bank) * GC_PHYSICAL_BANK_SIZE + rom_window_offset(addr);
}

} // namespace gbrecomp::arch

#endif // RECOMPILER_ARCHITECTURE_H