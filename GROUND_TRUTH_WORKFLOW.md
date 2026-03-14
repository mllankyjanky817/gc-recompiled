# Expert Workflow: Ground Truth Guided Recompilation

This guide describes the game.com workflow for improving code coverage with dynamic execution data captured from MAME's built-in debugger and trace facilities.

## The Strategy
Static analysis alone can struggle with computed control flow, MMU-controlled bank windows, and indirect branches. For game.com binaries, the recommended source of dynamic execution truth is MAME's `gamecom` driver, launched with `-debug` and a `-debugscript` that enables per-instruction tracing for the main SM8521 CPU.

---

## Quick Start: MAME Trace Workflow

For most cases, use MAME to generate a raw execution trace, post-process it into the recompiler's `Bank:Address` trace format, then feed it back into `gbrecomp`.

### 1. Capture a raw execution trace with MAME

Create a debugger script, for example `logs/gamecom_trace.cmd`:

```text
# Focus tracing on the main SM8521 CPU
focus maincpu

# Disable loop folding so every executed instruction is emitted
trace logs/gamecom_mame.trace,maincpu,noloop

# Run for 5 minutes of emulated time
gtime #300000

# Flush the trace and stop tracing
traceflush
trace off,maincpu
quit
```

Launch MAME with the game.com driver and your cartridge image in slot 1:

```bash
mame gamecom -cart1 roms/gamecom_rom.bin -debug -debugscript logs/gamecom_trace.cmd -window -skip_gameinfo
```

Notes:

1. The MAME short name for the console is `gamecom`.
2. `-debug` enables the integrated debugger.
3. `-debugscript` executes the trace commands immediately on startup.
4. `trace ... ,noloop` ensures loops are not condensed away, which is usually what you want for coverage extraction.

## Manual Workflow Steps

### Step 1: Capture Ground Truth (Dynamic)
Run the original game.com ROM under MAME and record the main CPU execution stream.

The raw MAME trace will include disassembly and optional extra fields, not just `(Bank, PC)` pairs. Use a post-processing step to convert it into the recompiler's trace format.

Example conversion flow:

```bash
# Example post-processing placeholder:
# 1. Parse MAME's trace output
# 2. Extract the currently mapped bank/window state and PC
# 3. Emit unique Bank:Address lines to a recompiler trace file
python3 tools/your_mame_trace_parser.py logs/gamecom_mame.trace -o logs/gamecom_ground.trace
```

- **Output**: `logs/gamecom_ground.trace` containing `Bank:Address` pairs.
- **Benefit**: This trace contains proven executed code from a game.com-aware emulator rather than Game Boy-only tooling.

Important:

1. The existing `tools/capture_ground_truth.py` is PyBoy-based and Game Boy-specific. Do not use it for game.com titles.
2. The existing `tools/run_ground_truth.py` automation is also currently PyBoy-oriented; use this manual MAME flow until a game.com-specific automation script exists.

### Step 2: Trace-Guided Recompilation
Now, we feed this trace into the recompiler. The recompiler will use these addresses as "roots" for its recursive descent analysis.

```bash
# Recompile using the MAME-derived ground truth trace
./build/bin/gbrecomp roms/gamecom_rom.bin -o output/gamecom_grounded --use-trace logs/gamecom_ground.trace
```

- **What happens**: The recompiler loads the trace and immediately marks those addresses as function entry points. It then follows every subsequent branch from those points, discovering much more code than a blind scan would.

### Step 3: Verify Coverage
Use the comparison tool to see how much of the dynamic execution was successfully recompiled.

```bash
# Compare the recompiled C code against the ground truth trace
python3 tools/compare_ground_truth.py --trace logs/gamecom_ground.trace output/gamecom_grounded
```

- **Success Metric**: Coverage should increase materially over blind static discovery, especially around MMU window changes and indirect dispatch.
- **Missing Instructions**: Remaining misses are likely tied to incomplete bank-state reconstruction, RAM overlays, or runtime-generated code/data ambiguities.

## Step 4: Refine the Trace (Optional)
If a specific menu path, minigame, or cartridge state is not covered, capture a second MAME trace with a different interaction path and merge it.

1. **Create a second MAME debug script** with different input/setup assumptions.
2. **Capture another raw MAME trace** and convert it to `Bank:Address` format.
3. **Merge or combine traces**:
   ```bash
   type logs\gamecom_alt.trace >> logs\gamecom_ground.trace
   sort logs\gamecom_ground.trace /unique > logs\gamecom_merged.trace
   ```
4. **Re-recompile** with the merged trace:
   ```bash
   ./build/bin/gbrecomp roms/gamecom_rom.bin -o output/gamecom_grounded --use-trace logs/gamecom_merged.trace
   ```

You can also generate refinement traces from the recompiled binary itself using `--trace-entries`, then merge those results back into the MAME-derived seed trace.

---

## Summary of Tools

| Tool | Purpose |
|------|---------|
| `mame gamecom -debug -debugscript <file>` | Runs the original game.com ROM in MAME and executes debugger trace commands automatically. |
| MAME `trace` / `traceover` / `tracelog` | Generates per-instruction execution logs from the SM8521 main CPU. |
| `tools/your_mame_trace_parser.py` | Placeholder for a game.com-specific converter from raw MAME trace output to `Bank:Address` lines. |
| `gbrecomp --use-trace <file>` | Loads a `.trace` file to seed function discovery. |
| `tools/compare_ground_truth.py` | Measures coverage of a recompiled project against a `.trace`. |
| `runtime --trace-entries <file>` | Logs execution from the *recompiled* binary for further refinement. |

## MAME Debugger Notes

- Use `focus maincpu` in debugger scripts to avoid tracing auxiliary devices.
- Use `trace <file>,maincpu,noloop` when you need exhaustive instruction coverage.
- Use `traceover` instead of `trace` if you want a higher-level call-oriented trace and are willing to skip subroutine bodies.
- Use `gtime #<milliseconds>` to bound scripted capture time.
- Use `traceflush` before shutdown so the trace file is flushed to disk.
