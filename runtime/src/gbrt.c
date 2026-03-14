#include "gbrt.h"
#include "ppu.h"
#include "audio.h"
#include "audio_stats.h"
#include "platform_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gbrt_debug.h"

/* ============================================================================
 * Definitions
 * ========================================================================== */

#define GC_REGISTER_FILE_SIZE 0x80
#define GC_INTERNAL_RAM_SIZE  0x380
#define GC_ROM_WINDOW_START   0x1000
#define GC_ROM_WINDOW_SIZE    0x2000
#define GC_ROM_WINDOW_COUNT   5
#define GC_ROM_WINDOW_END     0xA000
#define GC_VRAM_SIZE          0x4000
#define GC_EXT_RAM_SIZE       0x2000

/* ============================================================================
 * Globals
 * ========================================================================== */

bool gbrt_trace_enabled = false;
uint64_t gbrt_instruction_count = 0;
uint64_t gbrt_instruction_limit = 0;

static char* gbrt_trace_filename = NULL;


/* ============================================================================
 * Context Management
 * ========================================================================== */

GBContext* gb_context_create(const GBConfig* config) {
    GBContext* ctx = (GBContext*)calloc(1, sizeof(GBContext));
    if (!ctx) return NULL;
    
    ctx->wram = (uint8_t*)calloc(1, GC_INTERNAL_RAM_SIZE);
    ctx->vram = (uint8_t*)calloc(1, GC_VRAM_SIZE);
    ctx->oam = (uint8_t*)calloc(1, 1);
    ctx->hram = (uint8_t*)calloc(1, 1);
    ctx->io = (uint8_t*)calloc(1, GC_REGISTER_FILE_SIZE);
    ctx->eram = (uint8_t*)calloc(1, GC_EXT_RAM_SIZE);
    ctx->eram_size = GC_EXT_RAM_SIZE;
    
    if (!ctx->wram || !ctx->vram || !ctx->oam || !ctx->hram || !ctx->io) {
        gb_context_destroy(ctx);
        return NULL;
    }
    
    GBPPU* ppu = (GBPPU*)calloc(1, sizeof(GBPPU));
    if (ppu) {
        ppu_init(ppu);
        ctx->ppu = ppu;
    }
    
    ctx->apu = gb_audio_create();
    audio_stats_init();
    gb_context_reset(ctx, true);
    (void)config;

    if (gbrt_trace_filename) {
        ctx->trace_file = fopen(gbrt_trace_filename, "w");
        if (ctx->trace_file) {
            ctx->trace_entries_enabled = true;
            fprintf(stderr, "[GBRT] Tracing entry points to %s\n", gbrt_trace_filename);
        }
    }

    return ctx;
}

void gb_context_destroy(GBContext* ctx) {
    if (!ctx) return;
    
    /* Save RAM before destroying if available */
    if (ctx->eram && ctx->ram_enabled && ctx->callbacks.save_battery_ram) {
        gb_context_save_ram(ctx);
    }
    
    if (ctx->trace_file) fclose((FILE*)ctx->trace_file);
    free(ctx->wram);
    free(ctx->vram);
    free(ctx->oam);
    free(ctx->hram);
    free(ctx->io);
    
    if (ctx->eram) free(ctx->eram);
    
    if (ctx->ppu) free(ctx->ppu);
    if (ctx->apu) gb_audio_destroy(ctx->apu);
    if (ctx->rom) free(ctx->rom);
    free(ctx);
}

void gb_context_reset(GBContext* ctx, bool skip_bootrom) {
    if (ctx->apu) {
        gb_audio_reset(ctx->apu);
    }

    /* Reset DMA state */
    ctx->dma.active = 0;
    ctx->dma.source_high = 0;
    ctx->dma.progress = 0;
    ctx->dma.cycles_remaining = 0;
    
    /* Reset HALT bug state */
    ctx->halt_bug = 0;
    
    /* Reset interrupt state */
    ctx->ime = 0;
    ctx->ime_pending = 0;
    ctx->halted = 0;
    ctx->stopped = 0;
    
    /* Reset RTC state */
    ctx->rtc.s = 0;
    ctx->rtc.m = 0;
    ctx->rtc.h = 0;
    ctx->rtc.dl = 0;
    ctx->rtc.dh = 0;
    ctx->rtc.latched_s = 0;
    ctx->rtc.latched_m = 0;
    ctx->rtc.latched_h = 0;
    ctx->rtc.latched_dl = 0;
    ctx->rtc.latched_dh = 0;
    ctx->rtc.latch_state = 0;
    ctx->rtc.last_time = 0;
    ctx->rtc.active = true;  /* RTC oscillator active by default */
    
    /* Reset mapper/MMU state */
    ctx->rtc_mode = 0;
    ctx->rtc_reg = 0;
    ctx->ram_enabled = 0;
    ctx->mbc_mode = 0;
    ctx->rom_bank_upper = 0;
    ctx->mmu[0] = 0;
    ctx->mmu[1] = 1;
    ctx->mmu[2] = 2;
    ctx->mmu[3] = 3;
    ctx->mmu[4] = 4;
    
    if (skip_bootrom) {
        ctx->pc = 0x1020;
        ctx->sp = 0x03FF;
        ctx->af = 0x0000;
        ctx->bc = 0x0000;
        ctx->de = 0x0000;
        ctx->hl = 0x0000;
        gb_unpack_flags(ctx);
        ctx->rom_bank = 0;
        memset(ctx->io, 0, GC_REGISTER_FILE_SIZE);
    }
}

bool gb_context_load_rom(GBContext* ctx, const uint8_t* data, size_t size) {
    if (ctx->rom) free(ctx->rom);
    ctx->rom = (uint8_t*)malloc(size);
    if (!ctx->rom) return false;
    memcpy(ctx->rom, data, size);
    ctx->rom_size = size;

    if (!ctx->eram) {
        ctx->eram = (uint8_t*)calloc(1, GC_EXT_RAM_SIZE);
        ctx->eram_size = GC_EXT_RAM_SIZE;
    }
    
    return true;
}

bool gb_context_save_ram(GBContext* ctx) {
    if (!ctx || !ctx->eram || !ctx->eram_size || !ctx->callbacks.save_battery_ram) {
        return false;
    }
    
    /* Get ROM title for filename */
    char title[17] = {0};
    if (ctx->rom_size > 0x143) {
        memcpy(title, &ctx->rom[0x134], 16);
        for(int i=0; i<16; i++) {
            if(title[i] == 0 || title[i] < 32 || title[i] > 126) title[i] = 0;
        }
    }
    if(title[0] == 0) strcpy(title, "UNKNOWN_GAME");
    
    bool result = ctx->callbacks.save_battery_ram(ctx, title, ctx->eram, ctx->eram_size);
    if (result) {
        printf("[GBRT] Saved battery RAM for '%s'\n", title);
    } else {
        printf("[GBRT] Failed to save battery RAM for '%s'\n", title);
    }
    return result;
}

/* ============================================================================
 * Memory Access
 * ========================================================================== */

uint8_t gb_read8(GBContext* ctx, uint16_t addr) {
    if (addr < GC_REGISTER_FILE_SIZE) {
        return ctx->io[addr];
    }
    if (addr < GC_ROM_WINDOW_START) {
        size_t offset = addr - GC_REGISTER_FILE_SIZE;
        return offset < GC_INTERNAL_RAM_SIZE ? ctx->wram[offset] : 0xFF;
    }
    if (addr < GC_ROM_WINDOW_END) {
        uint8_t window = (uint8_t)((addr - GC_ROM_WINDOW_START) / GC_ROM_WINDOW_SIZE);
        uint16_t within = (uint16_t)((addr - GC_ROM_WINDOW_START) % GC_ROM_WINDOW_SIZE);
        uint8_t bank = ctx->mmu[window];
        size_t rom_addr = (size_t)bank * GC_ROM_WINDOW_SIZE + within;
        ctx->rom_bank = bank;
        return (ctx->rom && rom_addr < ctx->rom_size) ? ctx->rom[rom_addr] : 0xFF;
    }
    if (addr < 0xE000) {
        return ctx->vram[addr - 0xA000];
    }
    if (addr <= 0xFFFF) {
        size_t offset = addr - 0xE000;
        return (ctx->eram && offset < ctx->eram_size) ? ctx->eram[offset] : 0xFF;
    }
    return 0xFF;
}

void gb_write8(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (addr < GC_REGISTER_FILE_SIZE) {
        ctx->io[addr] = value;
        if (addr >= 0x24 && addr <= 0x28) {
            ctx->mmu[addr - 0x24] = value;
        }
        return;
    }
    if (addr < GC_ROM_WINDOW_START) {
        size_t offset = addr - GC_REGISTER_FILE_SIZE;
        if (offset < GC_INTERNAL_RAM_SIZE) {
            ctx->wram[offset] = value;
        }
        return;
    }
    if (addr < GC_ROM_WINDOW_END) {
        return;
    }
    if (addr < 0xE000) {
        ctx->vram[addr - 0xA000] = value;
        return;
    }
    if (addr <= 0xFFFF) {
        size_t offset = addr - 0xE000;
        if (ctx->eram && offset < ctx->eram_size) {
            ctx->eram[offset] = value;
        }
        return;
    }
}

uint16_t gb_read16(GBContext* ctx, uint16_t addr) {
    return (uint16_t)gb_read8(ctx, addr) | ((uint16_t)gb_read8(ctx, addr + 1) << 8);
}

void gb_write16(GBContext* ctx, uint16_t addr, uint16_t value) {
    gb_write8(ctx, addr, value & 0xFF);
    gb_write8(ctx, addr + 1, value >> 8);
}

void gb_push16(GBContext* ctx, uint16_t value) {
    ctx->sp -= 2;
    gb_write16(ctx, ctx->sp, value);
}

uint16_t gb_pop16(GBContext* ctx) {
    uint16_t val = gb_read16(ctx, ctx->sp);
    ctx->sp += 2;
    return val;
}

/* ============================================================================
 * ALU
 * ========================================================================== */

void gb_add8(GBContext* ctx, uint8_t value) {
    uint32_t res = (uint32_t)ctx->a + value;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F)) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_adc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    uint32_t res = (uint32_t)ctx->a + value + carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F) + carry) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_sub8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
    ctx->a -= value;
}
void gb_sbc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    int res = (int)ctx->a - (int)value - carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 1;
    ctx->f_h = ((int)(ctx->a & 0x0F) - (int)(value & 0x0F) - (int)carry) < 0;
    ctx->f_c = res < 0;
    ctx->a = (uint8_t)res;
}
void gb_and8(GBContext* ctx, uint8_t value) { ctx->a &= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 1; ctx->f_c = 0; }
void gb_or8(GBContext* ctx, uint8_t value) { ctx->a |= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_xor8(GBContext* ctx, uint8_t value) { ctx->a ^= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_cp8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
}
uint8_t gb_inc8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0x0F;
    val++;
    ctx->f_z = val == 0;
    ctx->f_n = 0;
    return val;
}
uint8_t gb_dec8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0;
    val--;
    ctx->f_z = val == 0;
    ctx->f_n = 1;
    return val;
}
void gb_add16(GBContext* ctx, uint16_t val) {
    uint32_t res = (uint32_t)ctx->hl + val;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->hl & 0x0FFF) + (val & 0x0FFF)) > 0x0FFF;
    ctx->f_c = res > 0xFFFF;
    ctx->hl = (uint16_t)res;
}
void gb_add_sp(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->sp += off;
}
void gb_ld_hl_sp_n(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->hl = ctx->sp + off;
}

uint8_t gb_rlc(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v = (v << 1) | ctx->f_c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rrc(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (v >> 1) | (ctx->f_c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rl(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v >> 7; v = (v << 1) | c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rr(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v & 1; v = (v >> 1) | (c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sla(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v <<= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sra(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (uint8_t)((int8_t)v >> 1); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_swap(GBContext* ctx, uint8_t v) { v = (uint8_t)((v << 4) | (v >> 4)); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; return v; }
uint8_t gb_srl(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v >>= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
void gb_bit(GBContext* ctx, uint8_t bit, uint8_t v) { ctx->f_z = !(v & (1 << bit)); ctx->f_n = 0; ctx->f_h = 1; }

void gb_rlca(GBContext* ctx) { ctx->a = gb_rlc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rrca(GBContext* ctx) { ctx->a = gb_rrc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rla(GBContext* ctx) { ctx->a = gb_rl(ctx, ctx->a); ctx->f_z = 0; }
void gb_rra(GBContext* ctx) { ctx->a = gb_rr(ctx, ctx->a); ctx->f_z = 0; }

void gb_daa(GBContext* ctx) {
   int a = ctx->a;
   if (!ctx->f_n) {
       if (ctx->f_h || (a & 0xF) > 9) a += 0x06;
       if (ctx->f_c || a > 0x9F) a += 0x60;
   } else {
       if (ctx->f_h) a = (a - 6) & 0xFF;
       if (ctx->f_c) a -= 0x60;
   }
   
   ctx->f_h = 0;
   if ((a & 0x100) == 0x100) ctx->f_c = 1;
   
   a &= 0xFF;
   ctx->f_z = (a == 0);
   ctx->a = (uint8_t)a;
}

/* ============================================================================
 * Control Flow helpers
 * ========================================================================== */

void gb_ret(GBContext* ctx) { ctx->pc = gb_pop16(ctx); }
void gbrt_jump_hl(GBContext* ctx) { ctx->pc = ctx->hl; }
void gb_rst(GBContext* ctx, uint8_t vec) { gb_push16(ctx, ctx->pc); ctx->pc = vec; }

void gbrt_set_trace_file(const char* filename) {
    if (gbrt_trace_filename) free(gbrt_trace_filename);
    if (filename) gbrt_trace_filename = strdup(filename);
    else gbrt_trace_filename = NULL;
}

void gbrt_log_trace(GBContext* ctx, uint16_t bank, uint16_t addr) {
    if (ctx->trace_entries_enabled && ctx->trace_file) {
        fprintf((FILE*)ctx->trace_file, "%d:%04x\n", (int)bank, (int)addr);
    }
}

__attribute__((weak)) void gb_dispatch(GBContext* ctx, uint16_t addr) { 
    gbrt_log_trace(ctx, (addr < 0x4000) ? 0 : ctx->rom_bank, addr);
    ctx->pc = addr; 
    gb_interpret(ctx, addr); 
}

__attribute__((weak)) void gb_dispatch_call(GBContext* ctx, uint16_t addr) { 
    gbrt_log_trace(ctx, (addr < 0x4000) ? 0 : ctx->rom_bank, addr);
    ctx->pc = addr; 
}

/* ============================================================================
 * Timing & Hardware Sync
 * ========================================================================== */

static inline void gb_sync(GBContext* ctx) {
    uint32_t current = ctx->cycles;
    uint32_t delta = current - ctx->last_sync_cycles;
    if (delta > 0) {
        ctx->last_sync_cycles = current;
        if (ctx->ppu) ppu_tick((GBPPU*)ctx->ppu, ctx, delta);
    }
}

void gb_add_cycles(GBContext* ctx, uint32_t cycles) {
    ctx->cycles += cycles;
    ctx->frame_cycles += cycles;
}



static void gb_rtc_tick(GBContext* ctx, uint32_t cycles) {
    if (!ctx->rtc.active) return;
    
    /* Update RTC time */
    ctx->rtc.last_time += cycles;
    while (ctx->rtc.last_time >= 4194304) { /* 1 second at 4.194304 MHz */
        ctx->rtc.last_time -= 4194304;
        
        ctx->rtc.s++;
        if (ctx->rtc.s >= 60) {
            ctx->rtc.s = 0;
            ctx->rtc.m++;
            if (ctx->rtc.m >= 60) {
                ctx->rtc.m = 0;
                ctx->rtc.h++;
                if (ctx->rtc.h >= 24) {
                    ctx->rtc.h = 0;
                    uint16_t d = ctx->rtc.dl | ((ctx->rtc.dh & 1) << 8);
                    d++;
                    ctx->rtc.dl = d & 0xFF;
                    if (d > 0x1FF) {
                        ctx->rtc.dh |= 0x80; /* Overflow */
                        ctx->rtc.dh &= 0xFE; /* Clear 9th bit */
                    } else {
                        ctx->rtc.dh = (ctx->rtc.dh & 0xFE) | ((d >> 8) & 1);
                    }
                }
            }
        }
    }
}

/**
 * Process OAM DMA transfer
 * DMA takes 160 M-cycles (640 T-cycles), copying 1 byte per M-cycle
 */
static void gb_dma_tick(GBContext* ctx, uint32_t cycles) {
    if (!ctx->dma.active) return;
    
    /* Process DMA cycles */
    while (cycles > 0 && ctx->dma.active) {
        /* Each byte takes 4 T-cycles (1 M-cycle) */
        uint32_t byte_cycles = (cycles >= 4) ? 4 : cycles;
        cycles -= byte_cycles;
        ctx->dma.cycles_remaining -= byte_cycles;
        
        /* Copy one byte every 4 T-cycles */
        if (ctx->dma.progress < 160 && (ctx->dma.cycles_remaining % 4) == 0) {
            uint16_t src_addr = ((uint16_t)ctx->dma.source_high << 8) | ctx->dma.progress;
            /* Directly access ROM/RAM without triggering normal restrictions */
            uint8_t byte;
            if (src_addr < 0x8000) {
                /* ROM */
                if (src_addr < 0x4000) {
                    byte = ctx->rom[src_addr];
                } else {
                    byte = ctx->rom[(ctx->rom_bank * 0x4000) + (src_addr - 0x4000)];
                }
            } else if (src_addr < 0xA000) {
                /* VRAM */
                byte = ctx->vram[src_addr - 0x8000];
            } else if (src_addr < 0xC000) {
                /* External RAM */
                byte = ctx->eram ? ctx->eram[(ctx->ram_bank * 0x2000) + (src_addr - 0xA000)] : 0xFF;
            } else if (src_addr < 0xE000) {
                /* WRAM */
                if (src_addr < 0xD000) {
                    byte = ctx->wram[src_addr - 0xC000];
                } else {
                    byte = ctx->wram[(ctx->wram_bank * 0x1000) + (src_addr - 0xD000)];
                }
            } else {
                byte = 0xFF;
            }
            ctx->oam[ctx->dma.progress] = byte;
            ctx->dma.progress++;
        }
        
        /* Check if DMA is complete */
        if (ctx->dma.progress >= 160 || ctx->dma.cycles_remaining == 0) {
            ctx->dma.active = 0;
        }
    }
}

void gb_tick(GBContext* ctx, uint32_t cycles) {
    static uint32_t last_log = 0;
    
    // Check limit
    if (gbrt_instruction_limit > 0) {
        gbrt_instruction_count++;
        if (gbrt_instruction_count >= gbrt_instruction_limit) {
            printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
            exit(0);
        }
    }

    if (gbrt_trace_enabled && ctx->cycles - last_log >= 10000) {
        last_log = ctx->cycles;
        fprintf(stderr, "[TICK] Cycles: %u, PC: 0x%04X, IME: %d, IF: 0x%02X, IE: 0x%02X\n", 
                ctx->cycles, ctx->pc, ctx->ime, ctx->io[0x0F], ctx->io[0x80]);
    }
    gb_add_cycles(ctx, cycles);
    
    /* RTC Tick */
    gb_rtc_tick(ctx, cycles);
    
    /* OAM DMA Tick */
    gb_dma_tick(ctx, cycles);

    /* Update DIV and TIMA */
    uint16_t old_div = ctx->div_counter;
    ctx->div_counter += (uint16_t)cycles;
    ctx->io[0x04] = (uint8_t)(ctx->div_counter >> 8);
    if (ctx->apu) gb_audio_div_tick(ctx->apu, old_div, ctx->div_counter);
    
    uint8_t tac = ctx->io[0x07];
    if (tac & 0x04) { /* Timer Enabled */
        uint16_t mask;
        switch (tac & 0x03) {
            case 0: mask = 1 << 9; break; /* 4096 Hz (1024 cycles) -> bit 9 */
            case 1: mask = 1 << 3; break; /* 262144 Hz (16 cycles) -> bit 3 */
            case 2: mask = 1 << 5; break; /* 65536 Hz (64 cycles) -> bit 5 */
            case 3: mask = 1 << 7; break; /* 16384 Hz (256 cycles) -> bit 7 */
            default: mask = 0; break;
        }
        
        /* Check for falling edges.
           We detect how many times the bit flipped from 1 to 0.
           The bit flips every 'mask' cycles (period is 2*mask).
           We iterate to find all falling edges in the range. 
        */
        uint16_t current = old_div;
        uint32_t cycles_left = cycles;
        
        /* Optimization: if cycles are small (common case), doing a loop is fine. */
        while (cycles_left > 0) {
            /* Next falling edge is at next multiple of (2*mask) */
            uint16_t next_fall = (current | (mask * 2 - 1)) + 1;
            
            /* Distance to next fall */
            uint32_t dist = (uint16_t)(next_fall - current);
            if (dist == 0) dist = mask * 2; /* Should happen if current is exactly on edge? */
            
            /* Check if we reach the fall */
            if (cycles_left >= dist) {
                /* Validate it is a falling edge for the selected bit?
                   next_fall is the transition 11...1 -> 00...0 for bits < bit+1.
                   Bit 'mask' definitely transitions. 
                   Wait, next multiple of 2*mask means mask bit becomes 0.
                   So yes, next_fall is a falling edge point.
                */
                if (ctx->io[0x05] == 0xFF) { 
                    ctx->io[0x05] = ctx->io[0x06]; /* Reload TMA */
                    ctx->io[0x0F] |= 0x04;         /* Request Timer Interrupt */
                } else {
                    ctx->io[0x05]++;
                }
                current += (uint16_t)dist;
                cycles_left -= dist;
            } else {
                break;
            }
        }
    }
    
    if ((ctx->cycles & 0xFF) < cycles || (ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1F))) {
        gb_sync(ctx);
        if (ctx->frame_done || (ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1F))) ctx->stopped = 1;
    }
    if (ctx->apu) gb_audio_step(ctx, cycles);
    if (ctx->ime_pending) { ctx->ime = 1; ctx->ime_pending = 0; }
}

void gb_handle_interrupts(GBContext* ctx) {
    if (!ctx->ime) return;
    uint8_t if_reg = ctx->io[0x0F];
    uint8_t ie_reg = ctx->io[0x80];
    uint8_t pending = if_reg & ie_reg & 0x1F;
    if (pending) {
        ctx->ime = 0; ctx->halted = 0;
        uint16_t vec = 0; uint8_t bit = 0;
        if (pending & 0x01) { vec = 0x0040; bit = 0x01; }
        else if (pending & 0x02) { vec = 0x0048; bit = 0x02; }
        else if (pending & 0x04) { vec = 0x0050; bit = 0x04; }
        else if (pending & 0x08) { vec = 0x0058; bit = 0x08; }
        else if (pending & 0x10) { vec = 0x0060; bit = 0x10; }
        if (vec) {
            ctx->io[0x0F] &= ~bit;
            
            /* ISR takes 5 M-cycles (20 T-cycles) as per Pan Docs:
             * - 2 M-cycles: Wait states (NOPs)
             * - 2 M-cycles: Push PC to stack (SP decremented twice, PC written)
             * - 1 M-cycle: Set PC to interrupt vector
             */
            gb_tick(ctx, 8);  /* 2 wait M-cycles */
            gb_push16(ctx, ctx->pc);
            gb_tick(ctx, 8);  /* 2 push M-cycles */
            ctx->pc = vec;
            gb_tick(ctx, 4);  /* 1 jump M-cycle */
            ctx->stopped = 1;
        }
    }
}

/* ============================================================================
 * Execution
 * ========================================================================== */

uint32_t gb_run_frame(GBContext* ctx) {
    gb_reset_frame(ctx);
    uint32_t start = ctx->cycles;

    while (!ctx->frame_done) {
        gb_handle_interrupts(ctx);
        
        /* Check for HALT exit condition (even if IME=0) */
        if (ctx->halted) {
             if (ctx->io[0x0F] & ctx->io[0x80] & 0x1F) {
                 ctx->halted = 0;
             }
        }
        
        ctx->stopped = 0;
        if (ctx->halted) gb_tick(ctx, 4);
        else gb_step(ctx);
        gb_sync(ctx);
    }
    return ctx->cycles - start;
}

uint32_t gb_step(GBContext* ctx) {
    if (gbrt_instruction_limit > 0 && ++gbrt_instruction_count >= gbrt_instruction_limit) {
        printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
        exit(0);
    }
    
    /* Handle HALT bug by falling back to interpreter for the next instruction */
    if (ctx->halt_bug) {
        gb_interpret(ctx, ctx->pc);
        return 0; /* Cycle counting handled by interpreter */
    }

    uint32_t start = ctx->cycles;
    gb_dispatch(ctx, ctx->pc);
    return ctx->cycles - start;
}

void gb_reset_frame(GBContext* ctx) {
    ctx->frame_done = 0;
    ctx->frame_cycles = 0;
    if (ctx->ppu) ppu_clear_frame_ready((GBPPU*)ctx->ppu);
}

const uint32_t* gb_get_framebuffer(GBContext* ctx) {
    if (ctx->ppu) return ppu_get_framebuffer((GBPPU*)ctx->ppu);
    return NULL;
}

void gb_halt(GBContext* ctx) { ctx->halted = 1; }
void gb_stop(GBContext* ctx) { ctx->stopped = 1; }
bool gb_frame_complete(GBContext* ctx) { return ctx->frame_done != 0; }

void gb_set_platform_callbacks(GBContext* ctx, const GBPlatformCallbacks* c) {
    if (ctx && c) {
        ctx->callbacks = *c;
    }
}

void gb_audio_callback(GBContext* ctx, int16_t l, int16_t r) {
    if (ctx && ctx->callbacks.on_audio_sample) {
        ctx->callbacks.on_audio_sample(ctx, l, r);
    }
}
