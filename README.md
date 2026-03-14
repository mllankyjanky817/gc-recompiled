# GC Recompiled

A **static recompiler** for Tiger game.com ROMs that translates SM8521/SM85CPU machine code directly into portable, modern C code.

![Compatibility](https://img.shields.io/badge/compatibility-in%20progress-yellow)
![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)

<p align="center">
  <img src="dino.png" alt="GC Recompiled Screenshot" width="400">
</p>

---

## Downloads

Pre-built binaries are available on the [Releases](https://github.com/arcanite24/gb-recompiled/releases) page:

| Platform | Architecture | File |
|----------|--------------|------|
| **Windows** | x64 | `gb-recompiled-windows-x64.zip` |
| **Linux** | x64 | `gb-recompiled-linux-x64.tar.gz` |
| **macOS** | x64 (Intel) | `gb-recompiled-macos-x64.tar.gz` |
| **macOS** | ARM64 (Apple Silicon) | `gb-recompiled-macos-arm64.tar.gz` |

> **Note**: The recompiler (`gbrecomp`) is what you download. After recompiling a ROM, you'll still need CMake, Ninja, SDL2, and a C compiler to build the generated project.

---

## Features

- **Architecture-Driven**: Targets the SM8521-based Tiger game.com platform
- **Native Performance**: Generated C code compiles to native machine code
- **Bank-Aware Analysis**:
  - Tracks game.com MMU windows (`MMU0-MMU4`) for banked ROM mapping
  - Preserves control flow across bank/window changes
- **Hardware-Facing Runtime**:
  - Memory-mapped register file semantics
  - DMA/video register integration
  - SG audio/timer/watchdog integration points
- **SDL2 Platform Layer**: Ready-to-run with keyboard/controller input and window display
- **Debugging Tools**: Trace logging, instruction limits, and screenshot capture
- **Cross-Platform**: Works on macOS, Linux, and Windows (via CMake + Ninja)

---

## Quick Start

### Prerequisites

- **CMake** 3.15+
- **Ninja** build system
- **SDL2** development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

### Building

```bash
# Clone and enter the repository
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled

# Configure and build
cmake -G Ninja -B build .
ninja -C build
```

### Recompiling a ROM

```bash
# Generate C code from a ROM
./build/bin/gbrecomp path/to/gamecom_rom.bin -o output/gamecom_test

# Build the generated project
cmake -G Ninja -S output/game -B output/game/build
ninja -C output/game/build

# Run!
./output/game/build/game
```

---

## Quick Setup

### Automated Setup (Recommended)

**macOS/Linux:**
```bash
# Download and run the setup script
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled
chmod +x tools/setup.sh
./tools/setup.sh
```

**Windows:**
```bash
# Download and run the setup script (run as Administrator)
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled
powershell -ExecutionPolicy Bypass -File tools/setup.ps1
```

### Manual Setup

**Prerequisites:**
- CMake 3.15+
- Ninja build system
- SDL2 development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

**Building:**
```bash
# Clone and enter the repository
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled

# Configure and build
cmake -G Ninja -B build .
ninja -C build
```

## Usage

### Basic Recompilation

```bash
./build/bin/gbrecomp <rom.bin> -o <output_dir>
```

The recompiler will:

1. Load and parse the ROM header
2. Analyze control flow across all memory banks
3. Decode instructions and track bank switches
4. Generate C source files with the runtime library

### Debugging Options

| Flag | Description |
|------|-------------|
| `--trace` | Print every instruction during analysis |
| `--limit <N>` | Stop analysis after N instructions |
| `--add-entry-point b:addr` | Manually specified entry point (e.g. `1:4000`) |
| `--no-scan` | Disable aggressive code scanning (enabled by default) |
| `--verbose` | Show detailed analysis statistics |
| `--use-trace <file>` | Use runtime trace to seed entry points |

**Example:**

```bash
# Debug a problematic ROM
./build/bin/gbrecomp roms/gamecom_rom.bin -o output/gamecom_test --trace --limit 5000
```

### Advanced Usage

**Ground Truth Workflow (Recommended):**
For complex titles with computed jumps, use the MAME-based workflow described in [GROUND_TRUTH_WORKFLOW.md](GROUND_TRUTH_WORKFLOW.md).

```bash
# Generate a MAME trace, convert it to Bank:Address format, then recompile
mame gamecom -cart1 roms/gamecom_rom.bin -debug -debugscript logs/gamecom_trace.cmd
```

**Trace-Guided Recompilation (Recommended):**
Complex game.com titles often use computed jumps that static analysis cannot resolve. You can use execution traces to "seed" the analyzer with every instruction physically executed during a real emulated session.

1. **Generate a trace**: Run any recompiled version of the game with tracing enabled, or use the **[Ground Truth Capture Tool](GROUND_TRUTH_WORKFLOW.md)** with MAME's debugger and `trace` command.

   ```bash
   # Option A: Using recompiled game
   ./output/game/build/game --trace-entries game.trace --limit 1000000

  # Option B: Using MAME debugger tracing on the original game.com ROM
  mame gamecom -cart1 roms/gamecom_rom.bin -debug -debugscript logs/gamecom_trace.cmd
   ```

2. **Convert the MAME trace if needed, then recompile with grounding**: Feed the resulting `Bank:Address` trace back into the recompiler.

   ```bash
  ./build/bin/gbrecomp roms/gamecom_rom.bin -o output/gamecom_test --use-trace game.trace
   ```

For a detailed walkthrough, see **[GROUND_TRUTH_WORKFLOW.md](GROUND_TRUTH_WORKFLOW.md)**.
**Generic Indirect Jump Solver:**
The recompiler includes an advanced static solver for `JP HL` and `CALL HL` instructions. It tracks the contents of all 8-bit registers and 16-bit pairs throughout the program's control flow.

- **Register Tracking**: Accurately handles constant pointers loaded into `HL` or table bases loaded into `DE`.
- **Table Backtracking**: When a `JP HL` is encountered with an unknown `HL`, the recompiler scans back for jump table patterns (e.g., page-aligned pointers) and automatically discovers all potential branch targets.
- **Impact**: Improves code discovery for branch-heavy binaries without requiring full dynamic traces.

**Manual Entry Points:**
If you see interpreter fallback messages in the logs at specific addresses, you can manually force the recompiler to analyze them:

```bash
./build/bin/gbrecomp roms/gamecom_rom.bin -o out_dir --add-entry-point 28:602B
```

**Aggressive Scanning:**
The recompiler automatically scans memory banks for code that isn't directly reachable (e.g. unreferenced functions). This improves compatibility but may occasionally misidentify data as code. To disable it:

```bash
./build/bin/gbrecomp roms/gamecom_rom.bin -o out_dir --no-scan
```

### Runtime Options

When running a recompiled game:

| Option | Description |
|--------|-------------|
| `--input <script>` | Automate input from a script file |
| `--dump-frames <list>` | Dump specific frames as screenshots |
| `--screenshot-prefix <path>` | Set screenshot output path |
| `--trace-entries <file>` | Log all executed (Bank, PC) points to file |

### Controls

| game.com | Keyboard (Primary) | Keyboard (Alt) |
|---------|-------------------|----------------|
| **D-Pad Up** | ↑ Arrow | W |
| **D-Pad Down** | ↓ Arrow | S |
| **D-Pad Left** | ← Arrow | A |
| **D-Pad Right** | → Arrow | D |
| **A Button** | Z | J |
| **B Button** | X | K |
| **Start** | Enter | - |
| **Select** | Right Shift | Backspace |
| **Quit** | Escape | - |

---

## How It Works

### 1. Analysis Phase

The recompiler performs static control flow analysis:

- Discovers all reachable code starting from entry points (`0x1020`, interrupt vectors)
- Tracks bank switches to follow cross-bank calls and jumps
- Detects computed jumps (e.g., `JP HL`) and resolves jump tables
- Separates code from data using heuristics

### 2. Code Generation

Instructions are translated to C:

```c
// Original: LD A, [HL+]
ctx->a = gb_read8(ctx, ctx->hl++);

// Original: ADD A, B
gb_add8(ctx, ctx->b);

// Original: JP NZ, 0x1234
if (!ctx->flag_z) { func_00_1234(ctx); return; }
```

Each ROM bank becomes a separate C file with functions for reachable code blocks.

### 3. Runtime Execution

The generated code links against `libgbrt`, which provides:

- Memory-mapped I/O (`gb_read8`, `gb_write8`)
- CPU flag manipulation
- PPU scanline rendering
- Audio sample generation
- Timer and interrupt handling

---

## Compatibility

See [COMPATIBILITY.md](COMPATIBILITY.md) for test results as they are updated.
Recompilation does not guarantee full playability.

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ SUCCESS | TBD | TBD |
| ❌ RECOMPILE_FAIL | TBD | TBD |
| ⚠️ RUN_TIMEOUT | TBD | TBD |
| 🔧 EXCEPTION | TBD | TBD |

Manually confirmed examples should be updated as game.com validation progresses.

---

## Roadmap

- [x] Tools to identify entry-points (Trace-Guided Analysis)
- [ ] Tools for better graphical debugging (outputting PNGs grid instead of raw PPMs)
- [ ] Android builds
- [ ] Broaden game.com hardware coverage (MMU, DMA, LCD/audio edge cases)
- [ ] Cached interpreter
- [ ] Improve quality of generated code
- [ ] Reduce size of output binaries

---

## Tools

The `tools/` directory contains utilities for analysis and verification:

### 1. Ground Truth Capturer

Capture instruction discovery data using MAME's `gamecom` driver, debugger, and trace facilities.

```bash
mame gamecom -cart1 roms/gamecom_rom.bin -debug -debugscript logs/gamecom_trace.cmd
```

The existing `tools/capture_ground_truth.py` script is still PyBoy-based and is not suitable for game.com ROMs.

### 2. Coverage Analyzer

Audit your recompiled code against a dynamic trace to see exactly what instructions are missing.

```bash
python3 tools/compare_ground_truth.py --trace game.trace output/gamecom_test
```

---

## Development

### Project Architecture

The recompiler uses a multi-stage pipeline:

```
ROM → Decoder → IR Builder → Analyzer → C Emitter → Output
         ↓           ↓            ↓
     Opcodes   Intermediate   Control Flow
               Representation   Graph
```

Key components:

- **Decoder** (`decoder.h`): Parses raw bytes into structured opcodes
- **IR Builder** (`ir_builder.h`): Converts opcodes to intermediate representation
- **Analyzer** (`analyzer.h`): Builds control flow graph and tracks bank switches
- **C Emitter** (`c_emitter.h`): Generates C code from IR

---

## License

This project is licensed under the MIT License.

**Note**: game.com is a trademark of Tiger Electronics. This project does not include copyrighted ROM data. You must provide your own legally obtained ROM files.

---

## Acknowledgments

- `hwdocs/sm8521.pdf` - SM8521/SM85CPU hardware reference
- `hwdocs/gcsts.pdf` - game.com Sacred Tech Scroll
- [mgbdis](https://github.com/mattcurrie/mgbdis) - Included disassembly tooling
- [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) - The original recompiler that inspired this project
