# Architecture Notes for Tiger game.com (SM8521)

This document summarizes architecture facts used by the recompiler/runtime and is derived from:

- `hwdocs/sm8521.pdf`
- `hwdocs/gcsts.pdf` (Game.com Sacred Tech Scroll)

Where these sources disagree, this document calls out both views and marks items needing validation.

## 1. System Summary

- CPU core: SM85CPU (inside SM8521), 8-bit architecture with 16-bit address bus.
- Address space: 64 KiB visible at a time.
- Execution reset entry: program counter starts at `0x1020` after hardware reset (SM8521 doc).
- Interrupt vectors: vector table occupies `0x1000-0x101F` (2-byte entries) in program ROM (SM8521 doc).

## 2. Memory Map

### 2.1 game.com-oriented map (`gcsts.pdf`)

- `0x0000-0x007F`: register file (CPU regs + control + peripheral registers)
- `0x0080-0x03FF`: RAM
- `0x0400-0x0FFF`: unmapped/reserved (per current reverse-engineering notes)
- `0x1000-0x9FFF`: ROM windows (banked)
- `0xA000-0xDFFF`: VRAM
- `0xE000-0xFFFF`: extended RAM

### 2.2 SM8521 generic map (`sm8521.pdf`)

- `0x0000-0x0FFF`: RAM region
- `0x1000-0xFFFF`: ROM region
- `0x0000-0x007F`: register file overlays low RAM

Interpretation for implementation: keep a game.com profile that follows `gcsts.pdf` first, but preserve hooks/config for unresolved areas (`0x0400-0x0FFF`, VRAM read behavior, extended RAM behavior).

## 3. CPU Register Model

### 3.1 General-purpose and paired registers

From `sm8521.pdf` / `gcsts.pdf`:

- 8-bit registers: `r0-r15`
- 16-bit pairs: `rr0, rr2, rr4, rr6, rr8, rr10, rr12, rr14`
- Program Counter: `PC` (16-bit)
- Stack Pointer: `SP` (split `SPH/SPL`)

Important nuance:

- General-purpose register storage is memory-mapped and relocatable through `PS0.RP` (register pointer).
- `RP` shifts the 16-byte general register window in 8-byte increments within low RAM/register-file space.

### 3.2 Status/control registers

- `PS0`: contains register pointer (`RP`) and interrupt mask field (`IM`).
- `PS1`: flags include `C, Z, S, V, D, H, B, I`.
- `SYS`: contains stack pointer size control (`SPC`) and memory configuration (`MCNF`).

## 4. Instruction-Set Characteristics

From `sm8521.pdf` CPU overview:

- 67 instruction families total.
- Includes 8-bit and 16-bit transfer/arithmetic/control paths.
- Includes bit-test + conditional branch operations (`BBC/BBS` style behavior).
- Includes unsigned multiply/divide support (`8x8->16`, `16/16` with remainder semantics described in docs).
- Includes `HALT`, `STOP`, `DI`, `EI`, `IRET` control instructions.

Addressing-mode coverage is broad (register, register-file, direct, indexed, indirect, relative, bit-addressed), and is central to correct lifting.

## 5. Interrupt and Exception Model

- Interrupt enable/request registers exposed in register-file range (`IE0/IE1`, `IR0/IR1`) per `gcsts.pdf`.
- `PS1.I` is the global interrupt enable flag.
- `PS0.IM` masks interrupt priority bands.
- Illegal instruction detect can raise NMI (SM8521 doc).
- `IRET` restores status/program flow from stack context.

## 6. ROM/MMU and Banking

From `gcsts.pdf`:

- ROM is managed in 8 KiB pages through `MMU0-MMU4` registers (`0x0024-0x0028`).
- These 5 MMU registers map `0x1000-0x9FFF` as five 8 KiB windows.
- Bank number is 8-bit (up to 256 banks, theoretical 2 MiB total).
- Documented split:
  - Banks `0-31`: internal system ROM region.
  - Banks `32-255`: cartridge-visible mapping behavior.

Recompiler implications:

- Static analysis must track MMU writes as control-flow-relevant state.
- Function identity should include (bank/window context, address), not raw address alone.

## 7. Display/Video and DMA Blocks

From `gcsts.pdf` register map:

- LCD control/timing registers around `0x0030-0x0032` (`LCC/LCH/LCV`).
- DMA block registers around `0x0034-0x003D` (`DMC`, source/destination coords, palette, ROM bank, VRAM page).

Runtime implications:

- DMA is not optional for correctness; it can source from banked ROM and target VRAM pages.
- Video path requires explicit VRAM/page semantics and palette handling from DMA register state.

## 8. Audio, Timers, and Peripheral Registers

From `gcsts.pdf`:

- Sound-generator control in `0x0040+` region (`SGC`, levels, time constants, waveform registers).
- Timer/control registers at `0x0050+` (`TM0C/TM0D/TM1C/TM1D/CLKT`).
- Watchdog registers (`WDT/WDTC`) in low register-file space.
- UART-related registers exist in register file (`URTT` listed).

Runtime implications:

- Audio should be modeled as SG channels with programmable wave/time constants.
- Timer and watchdog behavior can influence interrupts and system pacing.

## 9. Known Ambiguities to Validate

The docs themselves include areas marked as uncertain by reverse-engineering notes. Keep these as explicit TODOs in implementation:

- CPU readability semantics of VRAM on game.com profile.
- Exact behavior and intended use of extended RAM (`0xE000-0xFFFF`) in shipped titles.
- Some addressing-mode edge cases (especially index forms using `r0/rr0`).
- Precise external-memory configuration semantics under `SYS.MCNF` variants.

## 10. Practical Constraints for Recompiler Work

- Treat SM85CPU as distinct from SM83/Z80 when decoding and lifting.
- Prioritize complete register-file mapping first; many CPU/peripheral controls are memory-mapped there.
- Make MMU state first-class in CFG/dataflow (bank-sensitive code discovery).
- Keep architecture constants centralized so the profile can evolve as hardware validation improves.
