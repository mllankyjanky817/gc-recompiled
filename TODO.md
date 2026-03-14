# TODO

## Context

This repository has been partially migrated from Game Boy assumptions to Tiger game.com assumptions.

Completed in the last session:

- Added shared game.com architecture constants in `recompiler/include/recompiler/architecture.h`.
- Switched ROM metadata/loading in `recompiler/include/recompiler/rom.h` and `recompiler/src/rom.cpp` to a game.com-oriented model based on `0x1000-0x9FFF` visible ROM windows and `0x1020` reset entry.
- Retargeted parts of the analyzer, generator, CLI, symbol seeding, and codegen away from GB header/RST/MBC defaults.
- Added `mmu[5]` to `GBContext` and replaced the top-level runtime memory map in `runtime/src/gbrt.c` with a game.com-style register/RAM/ROM/VRAM/ext-RAM layout.

Important: this is not yet a working game.com recompiler/runtime. The largest blockers are still open below.

## Session Progress Update

Completed in this session:

- Replaced `recompiler/include/recompiler/decoder.h` with an SM85CPU-oriented instruction/register model.
- Replaced `recompiler/src/decoder.cpp` with an SM85CPU opcode decoder/disassembler baseline.
- Reworked `recompiler/include/recompiler/ir/ir.h` around SM85 semantics and MMU-aware IR operands.
- Replaced `recompiler/include/recompiler/ir/ir_builder.h` and `recompiler/src/ir/ir_builder.cpp` to lower the new SM85 instruction families.
- Removed legacy GB `RST` pattern logic from `recompiler/src/analyzer.cpp` and switched analysis queue state to `known_r[16] + mmu[5]`.
- Updated jump/call analysis paths to use new SM85 instruction names (`JPC_CC_NN`, `JR_E`, `JRC_CC_E`, `DBNZ_R_E`, `JP_RR`).
- Began runtime header migration in `runtime/include/gbrt.h` by introducing `SM85Context` and game.com config/model types while keeping compatibility aliases.

Still remaining after this session:

- Deep analyzer/MMU identity work is still incomplete (function identity is still bank/address keyed, not full MMU-state keyed).
- Runtime C implementation (`runtime/src/gbrt.c`, `runtime/src/interpreter.c`) still contains substantial GB-specific behavior.
- C emitter dispatch still needs MMU-sensitive function identity and call target resolution.
- Full Ninja build/test validation still pending on a local filesystem checkout.

## Highest Priority

### 1. Replace the SM83 decoder with an SM85CPU decoder

Files:

- `recompiler/include/recompiler/decoder.h`
- `recompiler/src/decoder.cpp`

Current problem:

- The decoder is still fundamentally Game Boy / SM83 based.
- It still models GB registers (`A/B/C/D/E/H/L`), GB instruction families, CB-prefixed ops, RST instructions, and GB conditions.
- Any analysis or codegen built on top of it is still decoding the wrong ISA for game.com ROMs.

What needs to happen:

- Design a new SM85CPU instruction model from `hwdocs/sm8521.pdf` and `hwdocs/gcsts.pdf`.
- Replace the `InstructionType`, operand/register enums, decoding tables, instruction sizes, and disassembly logic.
- Confirm reset/vector/control instructions (`IRET`, `DI`, `EI`, `HALT`, `STOP`, bit branches, multiply/divide, indexed modes, etc.).

This is the real unblocker.

### 2. Change analysis state from “current bank” to full MMU window state

Files:

- `recompiler/src/analyzer.cpp`
- `recompiler/include/recompiler/analyzer.h`
- likely `recompiler/include/recompiler/bank_tracker.h`
- likely `recompiler/src/bank_tracker.cpp`

Current problem:

- The analyzer still carries a single `current_bank` / `rom_bank` style context.
- game.com uses `MMU0-MMU4` to map five simultaneous 8 KiB ROM windows across `0x1000-0x9FFF`.
- Cross-window calls/jumps are therefore not modeled correctly yet.

What needs to happen:

- Replace the single-bank context with full MMU state in the analysis work queue.
- Make function/block identity sensitive to MMU state where required.
- Track writes to MMU registers (`0x0024-0x0028`) as control-flow-relevant state transitions.
- Revisit `make_address`/label identity if bank plus address is no longer sufficient.

## Recompiler Follow-Up

### 3. Rework IR and IR builder around SM85 semantics

Files:

- `recompiler/include/recompiler/ir/ir.h`
- `recompiler/src/ir/ir_builder.cpp`
- `recompiler/include/recompiler/ir/ir_builder.h`

Current problem:

- IR currently reflects GB/SM83 concepts such as `RST`, GB ALU helpers, and GB register assumptions.
- Even after a new decoder lands, the IR builder will still be lowering into the wrong machine model unless updated.

What needs to happen:

- Audit all opcode lowering paths.
- Replace GB-specific operations with SM85CPU equivalents.
- Introduce IR support for register-file-relative access, MMU-sensitive ROM access, and game.com control-flow instructions.

### 4. Update C emitter and generated dispatch for MMU-sensitive code selection

Files:

- `recompiler/src/codegen/c_emitter.cpp`

Current problem:

- Dispatch/init logic was partially retargeted, but function resolution is still effectively keyed to one bank plus one address.
- Generated code likely still assumes GB-style control flow and helper calls in many paths.

What needs to happen:

- Audit all emitted helper calls and address/bank dispatch logic.
- Ensure generated dispatch can distinguish code reached under different MMU mappings if needed.
- Update emitted initialization to set up any game.com-specific runtime state beyond the temporary `mmu[5]` defaults.

## Runtime Follow-Up

### 5. Finish migrating runtime state away from GB-only hardware structures

Files:

- `runtime/include/gbrt.h`
- `runtime/src/gbrt.c`
- `runtime/src/interpreter.c`

Current problem:

- `GBContext` still contains many GB-only fields: unpacked flag layout, MBC fields, CGB banking fields, OAM DMA state, joypad assumptions, RTC/MBC3 state, etc.
- `runtime/src/interpreter.c` is still a GB/SM83 interpreter fallback.
- Some of these fields are now dead or actively misleading after the memory-map change.

What needs to happen:

- Introduce an SM85/game.com CPU context model.
- Decide how to represent `r0-r15`, register pointer behavior, `PS0`, `PS1`, `SYS`, stack model, interrupt mask fields, and MMU registers.
- Remove or isolate leftover GB-only state once the new interpreter/decoder path is in place.
- Rewrite fallback interpretation for SM85CPU or delete it until replaced.

### 6. Reassess PPU/audio/platform layers

Files:

- `runtime/include/ppu.h`
- `runtime/src/ppu.c`
- `runtime/include/audio.h`
- `runtime/src/audio.c`
- `runtime/include/platform_sdl.h`
- `runtime/src/platform_sdl.cpp`

Current problem:

- These are still overtly Game Boy implementations.
- The recent runtime memory-map change does not make these subsystems correct for game.com.
- They still reference DMG/CGB timing, OAM sprites, GB joypad layout, GB palettes, GB audio registers, etc.

What needs to happen:

- Decide whether to stub these temporarily or begin proper game.com implementations.
- For video, model game.com LCD/DMA/VRAM semantics from `gcsts.pdf`.
- For audio, replace GB APU assumptions with game.com SG channel behavior.
- For input/platform, replace GB joypad naming and interrupt assumptions with game.com controls.

## Cleanup

### 7. Remove remaining Game Boy naming and dead assumptions

Examples to audit:

- `GBContext`, `GBConfig`, `gb_*` helper names
- comments/docstrings still describing Game Boy behavior
- `MBCType` naming that is now only acting as a compatibility shim
- any residual `0x4000`, `0x8000`, `0xFFxx`, `0x0100`, `0x0150`, `RST`, `DMG`, `CGB`, `SGB` assumptions

This is lower priority than correctness, but it should happen after the ISA/runtime migration to avoid confusion.

### 8. Revisit ROM abstraction once MMU modeling is real

Files:

- `recompiler/include/recompiler/rom.h`
- `recompiler/src/rom.cpp`

Current problem:

- The current ROM abstraction assumes physical 8 KiB pages and a visible-window helper, which is fine as a stopgap.
- Once analysis and runtime understand all five MMU windows, the ROM API may need explicit helpers for window-to-bank mapping and vector-table conventions.

## Validation

### 9. Rebuild and test from an actual filesystem checkout

Last session limitation:

- Source diagnostics were clean in the editor.
- Full `cmake -G Ninja -B build .; ninja -C build` validation could not be run because the terminal session was not attached to a local filesystem checkout of the workspace.

When resuming:

- Run the standard Ninja build from the repository root.
- Fix compile errors introduced by the partial migration.
- After decoder/runtime work, generate output into `output/` and validate against a real game.com ROM.

## Suggested Order For The Next Session

1. Implement the new SM85CPU decoder.
2. Update the analyzer to track full MMU state.
3. Update IR + IR builder to match the new decoder.
4. Rewrite or remove the GB fallback interpreter.
5. Reconcile codegen dispatch with MMU-sensitive function identity.
6. Rebuild with Ninja and fix compiler/runtime regressions.
7. Only then tackle accurate game.com video/audio/platform behavior.