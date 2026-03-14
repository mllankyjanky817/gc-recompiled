---
trigger: always_on
---

# Agent Rules for gc-recompiled

## Project Overview
This project is a static recompiler for Tiger game.com software, translating SM8521/SM85CPU code into C code compliant with modern standards.

- **Root Structure**:
  - `recompiler/`: Source for the recompiler tool (`gbrecomp`).
   - `runtime/`: Runtime library linked by recompiled game.com programs.
   - `roms/`: Contains original game.com ROM dumps (for example, `.bin` cartridge images).
   - `output/`: Generated recompiled projects (git-ignored).
   - `hwdocs/`: Hardware reference documents, especially `sm8521.pdf` and `gcsts.pdf`.

## Build Standards
- **Build System**: CMake + Ninja. **ALWAYS** use Ninja.
- **Main Project**:
  - Configure: `cmake -G Ninja -B build .`
  - Build: `ninja -C build`

## Workflow: Test ROM Recompilation
To ensure the generated game.com project reflects the latest recompiler/runtime changes, follow this strictly:

1. **Rebuild Tools**:
   ```bash
   ninja -C build
   ```

2. **Regenerate C Code**:
    Run the recompiler on a game.com ROM to update generated source files under `output/`.
   ```bash
    ./build/bin/gbrecomp roms/gamecom_rom.bin -o output/gamecom_test
   ```

3. **Rebuild Test Artifact**:
   Rebuild the generated project to produce the final executable.
   ```bash
    cmake -G Ninja -S output/gamecom_test -B output/gamecom_test/build
    ninja -C output/gamecom_test/build
   ```

## Development Guidelines
- **Sync**: "Make sure the recompiled project is always up to date." If you modify `recompiler` logic or `runtime` headers, trigger the *Test ROM Recompilation* workflow.
- **Resources**:
   - Consult `ARCH.md` for game.com architecture constraints derived from `hwdocs/sm8521.pdf` and `hwdocs/gcsts.pdf`.
   - Use `GROUND_TRUTH_WORKFLOW.md` for MAME-based trace capture guidance.
   - Use `README.md` for build and usage flow.
- **Paths**: Usage of absolute paths is preferred for tool calls, but shell commands should be executed from the workspace root for clarity.
- Put all recompiled ROM output under `/output`, since that folder is git ignored.
- Put all `.log` files under `/logs`, since that folder is git ignored.

## Debugging
- **Crash/Stuck Analysis**: If the recompiler hangs or produces invalid code:
   - Use `--trace` to log every instruction analyzed: `./build/bin/gbrecomp roms/gamecom_rom.bin --trace`
   - Use `--limit <N>` to stop after N instructions to prevent infinite loops (useful for finding where analysis gets stuck): `./build/bin/gbrecomp roms/gamecom_rom.bin --limit 10000`
  - Watch for `[ERROR] Undefined instruction` logs to identify invalid code paths or data misinterpreted as code.
   - For ground-truth traces, prefer MAME's debugger with `-debug` and `-debugscript` against the `gamecom` driver instead of Game Boy-specific tools like PyBoy.
   - When behavior conflicts with assumptions, validate against `hwdocs/sm8521.pdf` and `hwdocs/gcsts.pdf` and update `ARCH.md`.