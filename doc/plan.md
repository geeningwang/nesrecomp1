# nesrecomp1: milestone continuation plan

Status: approved continuation plan, implementation in progress. The repository exists at
`/Users/jingnw/nesrecomp1` and https://github.com/geeningwang/nesrecomp1.
M0 produced rebuild tools, analysis and code-size experiments; M1 input work has begun in nes1.
Foundation validation is complete on Windows; see [m0_report.md](m0_report.md).
Controller replay implementation and evidence are recorded in [replay.md](replay.md).

The immediate execution target
is a verified M1 runtime, followed by M2's first genuinely recompiled game, Mappy. Continue the
remaining roadmap in order only after the preceding acceptance gates pass.

---------------------------------------------------------------------------------------------------

## 0. Decisions already taken (from your answers)

| Topic | Decision |
|-------|----------|
| Translation approach | **Static recompilation 6502 -> C**, plus a **6502 interpreter fallback** for untranslated or invalidated code. Equivalence must be tested; fallback coverage is reported, never concealed. |
| Runtime language | **C++17 core with a C ABI**, approved in the continuation discussion; generated game code remains C. Reuse the verified MIT nes1 CPU/PPU/APU logic without bringing Win32 or desktop exports into the core. |
| Fidelity | **Bit-exact with FCEUX 2.6.6**. The parity contract built in `nes1` (`doc/parity_spec.md`, NESDUMP format, per-frame / per-scanline / per-instruction comparison) is the test oracle for the native games. |
| Distribution | Private for now, but **designed so a public web demo / release of freely licensed homebrew games is possible later** (see section 12). |
| Location | **Separate new GitHub repo**; `nes1` and `NES-Games-Disassembly` are dependencies. |
| STM32 | **Board not fixed.** Keep the generic port layer; select a board after complete linked runtime memory and performance evidence, not M0 object sizes alone. |
| Platforms | Windows, Linux (incl. SteamOS / Steam Deck), Web (WebAssembly), STM32. |

Both repositories exist and have been pushed. Use verified milestone commits and dependency pins.
Never put authentication values in git URLs, configuration, documentation, generated files or logs.

---------------------------------------------------------------------------------------------------

## Continuation: current evidence, execution order and acceptance gates

This section resolves the inconsistencies in the original design and takes precedence over the
earlier size-spike assumptions. The M0-M9 roadmap is retained, not restarted.

### C1. Snapshot at continuation approval

| Item | Verified state | Remaining work |
|------|----------------|----------------|
| Repository and dependencies | nesrecomp1 main is clean; dependency commits are pinned | Update the nes1 pin only after its next changes are committed and verified |
| ROM rebuild | Previously built all 26 games on Windows; 25 match the recorded SHA-1 | Add repeatable parser/build tests; preserve the explicit Nuts & Milk source-hash exception |
| CDL/debug analysis | Readers and analysis reports exist | CDL marks operands as well as opcodes; clean linear decoding alone is not proof of all execution boundaries |
| Code generator | Generated C was compiled into Thumb/Wasm objects for size measurements | No native game has yet run; generated semantics, dispatch and timing are not verified |
| STM32 sizing | Provisional generated-object measurements exist | Complete linked firmware, runtime RAM, real performance and board choice are not verified |
| nes1 input | Commit `7bf95a0` adds parsing and changes the controller model; a 300-frame no-input regression passed | Neither headless runner consumes the input yet; malformed input is not rejected strictly |
| nesrecomp1 dependency pin | Still at nes1 `be3ce16` | Deliberately do not use the WIP input commit as a completed oracle |
| Linux host | Read-only cloud check still fails reauthentication | Create no VM until authentication succeeds; continue Windows work meanwhile |

Do not describe M0 code-size experiments as executable correctness tests or as proof that a board
will meet frame-rate or RAM limits. The code-size table must identify optimization mode, object
versus linked size, and unimplemented runtime costs.

### C2. Findings that change the next implementation

1. All 26 game directories have a CDL; 25 have FM2 movies. Street Fighter 3 has no FM2 movie, so
   it needs a recorded or synthetic input fixture before claiming gameplay coverage.
2. All inventoried movies are text FM2 version 3, NTSC. Some headers omit initialization fields;
   defaults must match the pinned FCEUX version, not an invented normalization.
3. Mappy's walkthrough uses Four Score; its TAS uses ordinary controllers. Adventure Island,
   Double Dragon II, Dr. Mario and Solstice also have Four Score movies with non-neutral extra
   pads. A two-pad converter must not silently drop these records.
4. Movies contain reset and power commands. One Kunio-kun movie also contains command 8, which
   FCEUX defines as FDS disk selection. FDS remains out of scope: reject that fixture explicitly
   and select another supported movie; never silently discard the command.
5. The current `STEP`/`BSTEP` macros subtract a timer but do not implement the interrupt, APU or
   instruction-trace contract. They are scaffolding, not an exact runtime.
6. Bulk cycle charging and flag elimination can change state observed by interrupts, DMC and
   interpreter fallback. Per-instruction execution with eager flags is the initial default;
   block mode stays experimental until it passes the same parity gates.
7. The nes1 PPU includes legacy frame snapshots and `scanline_trace[240]` with RAM/NT/OAM copies.
   Port only the exercised lazy-rendering path into the new core. Do not copy these large desktop
   diagnostic buffers into the embedded machine state.

### C3. Finish the M0 validation gaps without blocking Windows work

Files: `tools/build_rom.py`, `tools/cdl.py`, `tools/dbg.py`, `tools/gen_opcodes.py`,
`tools/analyze.py`, `tools/codegen.py`, `runtime/include/nesrt_ops.h`, root `CMakeLists.txt`,
new focused tests under `tests/`, and `doc/m0_report.md`.

- Add standard-library Python unittest coverage for prefix stripping, Latin replacement, include-only
  files, linker output/split handling, debug symbol-to-bank resolution, and malformed/truncated inputs.
- Ensure a failed rebuild reports missing required assets and tool errors, rather than producing a
  success-shaped partial ROM. Check hashes against the particular game's identity, not any README entry.
- Record the known Nuts & Milk mismatch with the exact rebuilt identity. Reject a different mismatch
  instead of allowing arbitrary changed bytes just because the game name is on an exception list.
- Compile and link a small C-generated-code/C++-adapter smoke target with MSVC; exercise arithmetic,
  CPU-state exchange and C linkage. Object-file compilation by itself does not close this gate.
- Use existing Windows tools first. Install new tools only when the chosen command reports them
  missing. Restore Linux build validation when cloud authentication is available.

Gate: tests pass, C/C++ linkage works, and the M0 report distinguishes measurements from hypotheses.
Linux validation remains explicitly blocked until it has actually run.

### C4. M1-A: define deterministic replay and the observable-state contract

Files in nes1: `common/nesdump.h`, `src/cpu.{h,cpp}`, `src/nes1.cpp`,
`tool/fceux-2.6.6/src/fceux_dump.{h,cpp}`, the Windows headless runner, comparison tools,
probe generator, and directly related dump/build/parity documentation.
Files in nesrecomp1: `tools/make_input.py`, replay tests and a replay-contract document.

Replay:

- Preserve the existing unversioned `P1 P2` hex format for neutral-command, two-controller input.
  Introduce a versioned normalized form for movie metadata, up to four pads and command bits.
  The converter preserves all relevant input information; unsupported configurations fail explicitly.
- Map the FM2 `RLDUTSBA` character positions to controller bits, preserve frame order including both
  power-up frames, and validate ROM identity, NTSC mode, controller attachment and initialization.
- Implement ordinary controllers and Four Score in the runtime/oracle path, including their serial
  reports and signatures. Use the pinned FCEUX behavior for strobe edges, repeated reads and open bus.
  Preserve nes1's existing GUI input controls while changing shared controller internals.
- Support reset and power at the correct movie-frame boundary. Specify which machine state resets
  and which replay ordinal/trace counters continue. Do not approximate soft reset as power-on.
- Reject binary FM2, savestate-start movies, unsupported devices/FDS commands and contradictory metadata
  until implemented. Diagnostics identify the filename, line/frame and unsupported feature.
- Make the parser reject out-of-range bytes, garbage suffixes, invalid field counts and truncated
  lines; report read errors. Duplicate CLI options must not concatenate files accidentally.
- Retain documented neutral input past EOF for legacy input; record supplied/applied frame counts and
  EOF policy. Acceptance tests choose an explicit range within the movie and cannot pass on empty input.
- Verify normalized playback against FCEUX's native movie playback on supported short fixtures before
  using it as the reference for the new runtime.

State-dump version 3:

- Retain all existing observable sections and add controller serial state, CPU interrupt/pending state,
  `[WRAM]` CPU-visible work RAM, and `[NTVIEW]` the four logical nametable pages.
- Define unmapped work-RAM behavior and logical mirroring explicitly; do not assume a null mapper page
  is zero-filled RAM. Capture through non-mutating debug views, not ordinary reads that alter the bus,
  acknowledge IRQs or increment PPU addresses.
- Update shared State, both emulator captures, full writer, hashes, digest fields, ROM/run manifests,
  parsers and selectors together. Include raw-byte/size tests for every added section.
- Reject mixed-version and missing-required-section comparisons. Support comparing old v2 outputs
  with other v2 outputs where practical, but never compare v2 versus v3 as if they were equivalent.
- Preserve input and oracle configuration through every divergence-finder rerun. Forward the same
  replay options in dump, digest, PC, instruction and selected-frame trace modes.
- Reject empty digest sequences and empty trace selections. Stream long comparisons; record which
  frames and how many instructions/events were actually compared.

Gate: both headless runners apply the same input at the same boundary; changing a controller byte,
reset command or newly dumped state byte is detected by the tests.

### C5. M1-B: prove the updated oracle, then advance the dependency

Validation progression:

1. Focused parser/converter tests and controller probes: all buttons, both pads, Four Score, strobe
   held high, falling edge, reads beyond eight, reset/power, open bus and DMA interactions.
2. Determinism: repeat a supported input run from clean initial state and compare its complete results.
   Disable persistent battery loading and desktop configuration overrides during oracle runs.
3. Short Mappy TAS fixture first; Four Score walkthrough next. Check initialization and a gameplay
   state showing that Start/movement input is really consumed, not merely that two idle runs match.
4. Run the existing 20-entry no-input suite for 3000 frames at frame and scanline level under v3,
   plus line-by-line traces of selected frames.
5. Supported movie-start segments for the nine NROM games: 3000 frames each, chosen scanline/trace
   checks and manifests identifying the rebuilt ROM, replay file, oracle build and configuration.

Commit the verified nes1 changes, update `deps/nes1` to that commit, and validate a clean checkout.
Do not edit the checked-in submodule worktree as a substitute for updating its upstream repository.
M1-oracle is complete only when its tests and the input-enabled evidence pass.

### C6. M1-C: implement a portable C++ core with a C ABI

Files: root/runtime CMake targets, `runtime/include/nesrt.h`, the existing CPU/ops header,
new `runtime/src/` components, and the headless test adapter. Exact filenames may be simplified
when extracting the exercised nes1 paths.

- Reuse only MIT nes1 logic with its notices; FCEUX remains a separate GPL test executable. No FCEUX
  implementation is copied or linked into the shipped runtime.
- CPU, PPU, APU, bus, mapper-0 mapping and frame scheduler belong to one machine instance.
  Remove dependence on Win32 headers, `stdafx.h`, global renderer/CPU objects and platform exports.
- Define one canonical C-compatible CPU state. C-generated functions and the C++ interpreter see
  the same registers, flags, stack memory, data bus, interrupt latency, jam and timing state.
  No whole-64-KB synchronization copy on each instruction and no unsafe struct reinterpretation.
- Use `extern "C"` guards for exported services. Keep platform-specific allocation, file I/O and
  logging in adapters; errors are explicit return values with host diagnostics.
- Provide caller-owned/static storage for the embedded path, immutable PRG/CHR views, fixed core
  buffers and bounded audio queues. Desktop dump/trace memory is separate from the core.
- Use a sufficiently wide cycle count for full movies; reset semantics match the replay contract.
- Reset/reinitialize the machine deterministically. Two interleaved machine instances and repeated
  runs must not share static scheduler, sample or diagnostic state.
- Port the interpreter and mapper-0 runtime before linking generated game code. Instrument the
  same pre-instruction observation point as nes1/FCEUX, not a proxy at the end of a block.

Exact instruction sequence:

1. Replenish/check the current PPU-dot slice budget.
2. Service due NMI/IRQ using the correct prior-I, RTI/BRK and jam rules; charge entry overhead.
3. Emit the instruction observation before fetching/executing its body.
4. Apply opcode-fetch bus behavior and the APU hook at the verified base-cycle point; DMC fetches
   steal cycles and update the bus.
5. Perform operands, dummy accesses, stores and page/branch extras at their verified times.
6. Finish the instruction, account for elapsed dots and retain overshoot for the next slice.

Gate: interpreter-only `nrt_test` matches the updated oracle on focused probes, independent nestest
log, the complete 20-entry baseline and the supported nine-NROM replay fixtures. MSVC is the first
execution host; Linux build and test status is recorded separately if still blocked.

### C7. M2-A: harden the code map and establish safe dispatch

Files: `tools/analyze.py`, `tools/cdl.py`, `tools/dbg.py`, `tools/codegen.py`,
new per-game manifests/tests and the runtime dispatcher.

- Merge assembler instruction metadata, labels, CDL and executed-PC evidence. Do not treat operand
  bytes marked CODE as independent entry points without confirming instruction boundaries.
- Key code identity by physical PRG offset/page and CPU mapping. Resolve NROM mirroring; preserve
  ambiguous bank targets instead of guessing. Full bank-specific dispatch is exercised in later waves.
- Unknown direct/indirect targets, page-spanning instructions, overlapping entry points and RAM code
  use explicit interpreter fallback until safely translated.
- Bound jump tables using validated annotations; auto-detection yields candidates for review, not
  speculative entries added until a byte merely looks invalid.
- Make each translated instruction resumable at event boundaries. Preserve JSR return PC, RTS/RTI
  stack semantics and intra-page fallthrough; test gaps and non-returning handlers explicitly.
- A fallback result must state why it yielded and whether it made progress. Never loop forever
  between the dispatcher and the same non-entry address without executing an instruction.
- Generated RAM-code execution requires byte/version guards; modifying a code page invalidates its
  compiled entries. The same principle applies to mapper writes and code identity changes later.
- Include the canonical ROM identity and generator version in artifacts. A different ROM cannot
  run stale compiled constants, even when its title/name is unchanged.

### C8. M2-B: make generated instruction execution real

The existing ops/header and page generator are prototypes and need executable tests.

- First default: per-instruction scheduling and eager architectural flags. The hook must perform the
  shared interrupt/APU/bus/trace sequence; a timer decrement alone is insufficient.
- Emit or reproduce operand-fetch bus effects, unofficial NOP behavior, dummy read/write order,
  DMC stalls, DMA access times, vector access and IRQ latency.
- Unify CPU semantics where appropriate, while retaining the independent FCEUX/nestest reference.
  Sharing helper code between interpreter and generated execution is not proof of correctness.
- Check register/data-bus/stack/pending-interrupt state after every instruction; periodically compare
  RAM, VRAM, OAM, framebuffer and APU/controller state, not just final CPU registers.
- Run the existing 242-opcode RAM and I/O sweeps with 120 variants plus dedicated control-flow
  fixtures for the opcodes excluded by that generator.
- Only optimize blocks after the eager path passes. A block must either use proven conservative
  event deadlines and materialize all needed intermediate state, or fall back before executing.
  Flags observed by interrupts, tracing, I/O or interpreter fallback cannot be silently eliminated.
- Compare optimized and eager builds on identical movies and instruction boundaries. Keep a mode
  experimental if it misses any gate; never weaken the bit-exact target for the STM32 size budget.

Gate: real compiled instructions pass single-step and opcode tests; counters confirm they ran,
with intentional fallback cases exercised and counted.

### C9. M2-C: first native game, then the remaining NROM games

1. Produce an actual linked Mappy executable embedding locally generated code and data. A ROM
   run entirely through the interpreter does not count as static recompilation.
2. Compare Mappy against interpreter and FCEUX over 3000-frame no-input and replay runs, with frame
   and scanline digests and selected-frame text traces.
3. Report generated/interpreted instruction counts, fallback reasons, unique executed code identities,
   build mode and complete linked sizes. Target at least 99.9% generated execution on declared movie
   fixtures; disclose timing-edge or RAM-code fallback rather than mislabeling 100% coverage.
4. Expand to the other eight NROM games. Only claim full-game coverage after a supported full-length
   movie passes; the first 3000 frames prove only the smoke segment.

This gate unlocks the M3 playable desktop work. Long unattended runs need bounded timeouts and
processes that demonstrably survive SSH disconnection; preserve result manifests on the VM.

### C10. Remaining milestones and their prerequisites

| Milestone | Continue after | Scope and exit gate |
|-----------|----------------|---------------------|
| M3 Desktop | M1 oracle/runtime and M2 NROM execution | SDL2 video/audio/gamepad/keyboard, deterministic core separated from wall-clock pacing; linked Windows and Linux smoke/play tests |
| M4 Web | Verified desktop core | Emscripten C/C++ link, browser input/audio lifecycle and persistence; Playwright checks the same hashes on freely licensed test content |
| M5 Mapper wave 1 | Verified M2 dispatch and replay | CNROM, UxROM, AxROM, MMC1; probe banking/mirroring/WRAM, bus conflicts by actual board behavior and RAM-code invalidation |
| M6 MMC3 | Verified mapped bus/PPU and M5 | Exact observed IRQ and CHR/PRG behavior; do not assume FCEUX's old PPU implements the hardware IRQ model literally |
| M7 STM32 | Working desktop runtime and linked memory/speed measurements | Generic HAL and cross-build first; select a board after verified budgets, then real hardware bring-up |
| M8 MMC5 / 91 | M6 and supported replay fixtures | Implement required features from observed tests; create a Street Fighter 3 fixture instead of assuming an upstream movie exists |
| M9 Quality | Prior fidelity and platform gates | Optimizations, packaging and coverage improvements without relaxing exactness |

Board choice remains open. Emulator/simulator tests establish functional behavior, not Cortex-M7
performance, cache behavior, SDRAM/QSPI latency or LCD/audio readiness. Verify those on a real
board. STM32 video conversion must handle all palette/emphasis values; a simple fixed L8 CLUT is
not automatically lossless for the dump's emphasis encoding.

### C11. Publication, persistence and stop conditions

- Keep commercial-game ROMs, generated code/data, rebuilt binaries and raw dumps out of commits,
  public pages and CI uploads. Public tests/demos use our own or appropriately licensed fixtures.
- Preserve repository boundaries: oracle instrumentation in nes1; runtime/tooling in nesrecomp1.
  Update directly related docs and make independently verified milestone commits.
- Before reporting a milestone complete, record the exact source/dependency commits, input/ROM
  identities, executable build, test command, compared event/instruction counts and outcome.
- No-op success (empty traces, skipped games, ignored input, unsupported commands or missing
  sections) must be a failure or an explicit incomplete result.
- Stop for unresolved behavioral choices or unavailable required hardware/authentication. A blocked
  Linux gate does not prevent independent Windows work, but cannot be marked passed.

---------------------------------------------------------------------------------------------------

## 1. Investigation results: what the disassembly repo gives us

Repository `geeningwang/NES-Games-Disassembly` (made with the BZK 6502 Disassembler; each source "is equal to the original ROM, compilable and editable").

### 1.1 Layout of one game folder (Mappy as the example)

| File | Meaning |
|------|---------|
| `bank_FF.asm` (+ `bank_00.asm`...) | ca65 source, one file per PRG bank. 558 KB for a 16 KB bank because every line carries a 40-character prefix such as `C D 2 - - - 0x00006A 00:C05A:` (flags + ROM offset + bank:CPU address) and the raw bytes. |
| `bank_ram.inc`, `bank_val.inc` | RAM variable names (`ram_xxxx = $xxxx`, with the ROM offsets that use them) and named constants. Comments contain game knowledge ("bzk" notes). |
| `ld65.cfg` | linker layout: one MEMORY area + SEGMENT per bank, with the CPU address where it is mapped. |
| `CHR_ROM.chr` (Ice Climber: `CHR_ROM_1/2.chr`) | CHR data. Games with CHR RAM have none (Zelda, Little Mermaid, Indy Heat, Solstice). |
| `header.bin` | 16-byte iNES header (mapper, sizes, mirroring, battery). |
| `preparations.lua`, `assemble.bat` / `assemble.sh` | strip the prefix with a regex into `copy_*.asm`, run ca65 + ld65, concatenate header + PRG + CHR, check SHA-1. Windows ships `ca65.exe`, `ld65.exe`, `lua53.exe`; other OS uses a system `cc65` through `_scripts/*.sh`. |
| `DPCM.bin`, `incbin/` | extra binary data (Zelda). |
| `_misc/` | **Very valuable for us**: an FCEUX **CDL file** (code/data log) for every game and **FM2 input movies** (walkthroughs / TAS, 0.1 to 12 MB each) for nearly every game, `debug logs.zip`, text tables. |

README states which games are done / WIP / planned. Only the 26 "Disassembled" ones are in scope.

### 1.2 The 26 games by mapper (iNES headers read from every `header.bin`)

| Mapper | Hardware | Games | PRG / CHR |
|--------|----------|-------|-----------|
| 0 | NROM | Battle City, Excitebike, Ice Climber, Mappy, Nuts & Milk, Pac-Man, Son Son, Tennis, Yie Ar Kung-Fu | 16-32 KB / 8 KB |
| 1 | MMC1 | Bugs Bunny Crazy Castle (64K/32K), Dr. Mario (32K/32K), RoboCop 3 (128K/128K), The Legend of Zelda (128K / CHR-RAM, battery) | |
| 2 | UxROM | The Little Mermaid (128K / CHR-RAM) | |
| 3 | CNROM | Adventure Island (32K / 32K) | |
| 4 | MMC3 (scanline IRQ) | Captain America, Contra Force, Double Dragon II, Felix the Cat, Kunio-kun Soccer League, Super C, Tecmo World Cup Soccer | 64-128K / 64-128K |
| 5 | MMC5 | Castlevania III (256K / 128K) | the hardest mapper |
| 7 | AxROM | Danny Sullivan's Indy Heat, Solstice (128K / CHR-RAM) | |
| 91 | unlicensed (JY-like) | Street Fighter III (9 Fighter) (128K / 512K) | has a scanline IRQ too |

The continuation inventory confirms CDL files for all 26 games and FM2 files for 25 games.
Street Fighter 3 has no FM2 file. These are candidate regression recordings; they require ROM,
device, reset and initialization validation before their gameplay coverage can be trusted.

### 1.3 What I found in the sources that shapes the design

* Plain ca65, hardware registers as raw absolute addresses (`STA $2000`, `STA $4014`), RAM named via `.inc`. No macro-heavy code seen so far.
* **Code that is not in the ROM image at its address**: Zelda's MMC1 routines are copied to SRAM and run from there (the file `bank___BF50_BFF9.asm` calls this out: "jumps from SRAM"). Other games may have RAM-resident or self-modifying code. => The interpreter fallback is a hard requirement, not a nicety.
* Bank-switched games have **bank-specific code addresses** (the same CPU address is different code in a different bank); every game has fixed bank(s) at the top of the address space.
* Data tables inside code banks (Mappy: an ASCII copyright string in the middle of `$C000`), so code/data separation must not be guessed from bytes alone: use CDL + control-flow discovery.
* The build tool-chain is ca65/ld65 (cc65). `ld65 --dbgfile` gives symbols, segments, spans and source lines, which is the clean machine-readable way to get labels. We do **not** need to parse ca65 source ourselves.

### 1.4 Environment constraints already known from the nes1 work

* The Mac kills freshly compiled binaries ("Killed: 9"). Python tools run on the Mac; **every C build and run happens on VMs**: the existing Windows VM (`nes1-win-vm`, MSVC, FCEUX 2.6.6 built) and a **new Linux VM** (GCC/Clang, Emscripten, `arm-none-eabi-gcc`, cc65, Renode). To be created in M0.
* GitHub pushes have succeeded in both repositories; use authenticated operations without persisting credentials.

---------------------------------------------------------------------------------------------------

## 2. Goals, non-goals, definition of done

### Goals
1. `nesrecomp1 build <game> --platform <p>` produces a native game for each platform from the game's disassembly (or a ROM + its metadata).
2. The native game is **frame-, scanline- and instruction-identical to FCEUX** on recorded input (movies) for the tested frames (section 9).
3. One code generator, one runtime core, thin platform layers (Windows, Linux/SteamOS, Web, STM32).
4. Recompiled coverage is measured and reported per game; anything not covered still runs (interpreter).

### Non-goals (explicit)
* PAL / Dendy timing (all 26 are NTSC or work as NTSC).
* Save states, rewind, cheats, netplay, other mappers and Famicom Disk System. Expansion audio is
  outside the first mapper-0 runtime, but required mapper-specific audio is assessed in the MMC5 wave.
* Gameplay changes / mods (the source being editable is a bonus, not a goal).
* Beating FCEUX where FCEUX is "wrong": we match it, quirks included (it is the oracle).

### Definition of done (per game)
1. ROM rebuilt from the disassembly has its verified source identity; preserve the narrowly
   documented Nuts & Milk upstream-record exception.
2. Recompiled build, exact mode, on the desktop test build: all frames of the chosen movie(s) identical to FCEUX at frame level; about 10 chosen frames identical at scanline and instruction-trace level.
3. Playable on Windows and Linux with video, audio and keyboard/gamepad. Web and STM32 per the milestone table.
4. Coverage report: share of executed instructions that ran recompiled (target >= 99.9 % on the movies, 100 % for all code reachable from CDL).

---------------------------------------------------------------------------------------------------

## 3. Architecture overview

```
 NES-Games-Disassembly/<game>            (dependency, pinned commit)
   *.asm  ld65.cfg  CHR  header.bin  _misc/*.cdl  *.fm2
          |
          v
 [A] tools/build_rom.py        ca65+ld65 -> game.nes  (CRC/SHA check)  + game.dbg (ld65 --dbgfile) + map
          |
          v
 [B] tools/analyze.py          ROM + dbg symbols + CDL + vectors + annotations (game.toml)
          |                      -> code map: leaders, basic blocks, jump tables, bank pages
          v
 [C] tools/codegen.py          -> generated/<game>/*.c  *.h  (one C file per PRG page) + data (ROM, CHR as arrays)
          |
          +-------------------------+----------------------------+
          v                         v                            v
   runtime/ (C++17, C ABI)    platform/desktop (SDL2)     platform/web, platform/stm32
   cpu ctx, bus, PPU, APU,    Windows / Linux / SteamOS   (Emscripten / HAL + DMA)
   mappers, interpreter,
   NESDUMP/trace hooks
          |
          v
   game executable  <---  verified against FCEUX with nes1's tool/compare_dumps.py (the oracle)
```

Key ideas:
* **The runtime owns everything except the CPU code of the game.** CPU = generated C (fast path) or the interpreter (fallback). PPU/APU/mappers/bus/timing are shared by both and by all platforms.
* **Two CPU implementations behind one interface.** The interpreter runs the same ROM the recompiler translated; running both in lockstep is the recompiler's main test (section 9.3).
* **The runtime can be brought up before the recompiler exists:** interpreter + runtime core must already match FCEUX on mapper-0 games (milestone M1). Failures are then attributable.
* **Exact mode is the only mode at first** (a per-instruction hook; section 6.7). A faster block-level mode is an optional later optimisation with its own acceptance criteria.

---------------------------------------------------------------------------------------------------

## 4. Repository layout and dependencies (new repo `nesrecomp1`)

```
nesrecomp1/
  README.md  LICENSE  CMakeLists.txt  pyproject.toml (tools)  .gitignore (generated/, out/, *.nes)
  deps/
    disassembly/        git submodule: geeningwang/NES-Games-Disassembly (pinned commit; rights restrictions apply)
    nes1/               git submodule: geeningwang/nes1  (tool/compare_dumps.py, find_divergence.py,
                        common/nesdump.h, FCEUX 2.6.6 sources with the dump instrumentation, parity docs)
  doc/                  plan.md (this file), design.md, runtime_contract.md, mappers.md, platforms/*.md,
                        fidelity_spec.md (new observations, extends nes1 doc/parity_spec.md), build_environment.md
  tools/                build_rom.py  analyze.py  codegen.py  cdl.py  dbg.py  nesrecomp1.py (CLI)
                        run_movie.py  make_input.py (FM2 -> input file)  coverage.py  oracle.ps1 / oracle.sh
  runtime/              include/nesrt.h (C ABI)  src/{cpu_ctx,bus,ppu,apu,interp,dump,trace,input}.cpp
                        src/mappers/{m000_nrom,m001_mmc1,m002_uxrom,m003_cnrom,m004_mmc3,m005_mmc5,m007_axrom,m091}.cpp
  platform/
    hal/                nesrt_hal.h   (video/audio/input/storage/time interface, section 8.1)
    desktop/            SDL2 (Windows, Linux, SteamOS)
    web/                Emscripten, HTML shell, AudioWorklet
    stm32/              HAL port, CMake, linker scripts per board, Renode scripts
  games/<game>/game.toml   per-game annotations (section 5.3)
  tests/                unit (python + C), lockstep, oracle, platform smoke
  generated/  out/      build products (never committed)
```

Why submodules: the disassembly repo is large (about 1 MB of text per bank) and pinned per milestone; `nes1` provides the oracle tools and must stay one source of truth (no copying of FCEUX or the parity docs).

Licence of new code: MIT (like nes1). The runtime is written clean-room from the public NESDev documentation and from the observed behaviour of FCEUX (same rule as nes1; FCEUX is GPLv2 and none of its code is copied). `nes1`'s own MIT PPU/APU/CPU-bus code may be ported (same owner).

---------------------------------------------------------------------------------------------------

## 5. Stage A and B: from disassembly to a code map

### 5.1 Stage A: rebuild the ROM (tools/build_rom.py)
* For game `G`: a plain-Python re-implementation of `preparations.lua`'s prefix strip (so we do not depend on Lua), then `ca65` per bank file and `ld65 -C ld65.cfg --dbgfile G.dbg -m G.map`, then `header.bin + PRG + CHR`.
* Tool-chain: Windows VM uses the shipped `ca65.exe`/`ld65.exe`; Linux VM uses distro `cc65`. Both must yield identical output.
* Checks: game-specific SHA-1 and CRC32 against the README and the
  `NES_OUTPUT_FILE_SHA1_ORIGINAL` in each `assemble.sh`. Do not relax identity checks based on a
  title's `[!]` marker; only an explicitly verified source-specific exception is allowed.
* Output: `out/<game>/game.nes`, `game.dbg`, `game.map`, and a JSON `rom_info` (mapper, PRG/CHR sizes, mirroring, battery, bank layout from `ld65.cfg`).

### 5.2 Stage B: analysis (tools/analyze.py)
Inputs: ROM, `.dbg` (labels, segments, spans), CDL, vectors, `games/<G>/game.toml`.

1. **Bank model.** From `rom_info` and the mapper: split PRG into 8 KB *pages* (the smallest switch granularity of any mapper here: MMC3 / MMC5). A translation unit = one physical PRG page. A block never crosses a page boundary (it ends with a fall-through to the dispatcher).
2. **Seed set.** Reset / NMI / IRQ vectors, confirmed instruction boundaries derived from assembler
   metadata/CDL/observed PCs, code labels in `.dbg`, validated jump-table targets and explicit
   `entry` annotations. CDL CODE and INDIRECT flags include operand bytes and are not themselves
   a complete instruction-start map.
3. **Recursive descent** from the seeds with a full 6502 decoder including the **undocumented opcodes** (the oracle matches FCEUX for all of them since the opcode sweep in the nes1 parity work). Conditional branches add both successors; `JSR` adds the target and the fall-through; `RTS`/`RTI`/`JMP (ind)`/`BRK` end a block.
4. **Jump tables / computed control flow.** Usual NES patterns, found by pattern + CDL:
   * *Pointer-table dispatch*: `ASL A; TAY; LDA tbl,Y; STA ptr; LDA tbl+1,Y; STA ptr+1; JMP (ptr)`;
   * *RTS trick*: `LDA tbl+1,Y; PHA; LDA tbl,Y; PHA; RTS` (table holds address-1);
   * `JSR` to a RAM vector.
   The analyzer proposes tables for validation. Ambiguous lengths and bank identities require
   bounded annotations or executed-PC evidence; they are not resolved by guessing until a value
   stops looking like code. Unresolved transfers remain interpreted and are reported.
5. **Dynamic discovery loop (profile-guided).** When a generated game runs a movie and the dispatcher meets a PC that is not a known leader, the interpreter runs the code and writes a *miss record* (bank page, address, how it was reached) to `coverage.json`. `analyze.py --merge-misses` adds those as leaders in `game.toml`. Repeating this over all movies converges to full coverage; the numbers are the coverage metric.
6. **Output:** `out/<game>/codemap.json`: pages -> blocks (start, end, successors, instruction list with decoded operand, cycle cost, flags read/written), table list, data ranges, unresolved items, statistics.

### 5.3 Per-game annotation file `games/<G>/game.toml`
Small, hand-maintained, reviewed when the analyzer asks:
```toml
[game]  name = "mappy"  rom_crc32 = "3F0E8E0A"  mapper = 0
[[entry]]  addr = 0xC031  note = "RESET"                       # extra seeds
[[jump_table]]  at = 0xD536  kind = "ptr_lo_hi"  count = 7      # explicit when auto-detection is unsure
[[ram_code]]  addr = 0x6000  copy_from = 0xBF98  len = 0x40     # code copied to RAM (Zelda SRAM stubs)
[input]  movies = ["mappy_walkthrough.fm2"]
```
`ram_code` can enable translated RAM-resident code only with verified byte/version guards and
invalidation on writes. Anything else in writable memory goes to the interpreter.

---------------------------------------------------------------------------------------------------

## 6. Stage C: code generation (tools/codegen.py)

### 6.1 Execution model
The CPU is a state machine over **(physical page, address)**:
```c
// per generated PRG page: one function with a switch on the block entry offset and gotos between blocks of the page
uint16_t nrt_p12(nrt_cpu* c, uint16_t pc);  // returns the next PC when control leaves the page or an event needs service
```
* Inside a page, control transfer between blocks is a `goto` (compilers turn the entry `switch` into a jump table). A trampoline of one function per block is the alternative; M0 spike S1 decides by measuring code size and speed.
* Leaving the page (branch/JMP/JSR/RTS/fall-through off the end) returns the target PC to the **dispatcher** in the runtime: `pc -> window (pc>>13) -> physical page from the mapper -> generated function or interpreter`.
* `JSR` / `RTS` / `RTI` / `BRK` use the **real emulated stack in RAM** (push return address - 1, pop and dispatch). This keeps the RTS trick, stack-manipulating games (`PLA PLA`, `TXS`), nested NMI/IRQ and non-local exits **correct by construction**. "Native C call/return" function recovery is an optional later optimisation (M9), not part of correctness.
* Registers and flags initially live in the canonical C-compatible CPU state, visible at every
  instruction/interrupt/fallback boundary. Local caching is a later optimization that must
  preserve architectural P and all observers.

### 6.2 Instruction translation
* One C statement group per instruction, written as macros from `runtime/include/nesrt_ops.h` (`OP_ADC`, `OP_LDA_ABX`, ...). The generator emits only operand constants and macro names; the semantics live in one reviewed header, covered by a unit test against `nestest.log` and against the interpreter.
* **Memory access** goes through inline `RD(addr)` / `WR(addr,val)` with a fast path for work RAM (`addr < 0x2000`) and PRG ROM reads; everything else calls the bus (`nrt_bus_read/write`) which handles PPU, APU, controllers, mapper registers, PRG-RAM, open bus (spec O-8, O-14, O-18) and the data-bus latch.
* The **data-bus latch and dummy reads / dummy writes** (spec O-33: indexed dummy read, RMW double write, OAM DMA through the bus) are generated exactly as in the interpreter, so the instruction trace hash matches.
* **Illegal opcodes** (LAX, SAX, SLO, RLA, SRE, RRA, DCP, ISB, ANC, ALR, ARR, AXS, the unstable ones with FCEUX's results per O-31, all KIL variants) are in the same header; they must be present because the trace has to match even if a game rarely uses them.

### 6.3 Cycle accounting and timing (the exact contract)
All timing rules are those of `nes1` `doc/parity_spec.md`. The instruction sequence in C6 is the
initial implementation contract. The scheduler serves a slice before starting an instruction;
interrupt entry precedes the trace, and the trace precedes opcode-fetch/APU/body effects.
An illustrative hook is:
```c
/* May yield or redirect to an interrupt handler before this body runs. */
NRT_INSN_BEGIN(pc, opcode, base_cycles);
/* Operand accesses, page/branch extras, stores and side effects. */
NRT_INSN_END();
```
These names are a design sketch, not the current `STEP` macro's functionality. Exact
per-instruction APU behavior remains active even in builds with file logging disabled.
Within the body, accesses to PPU registers call `nrt_ppu_sync(cycle_now)` the same way `nes1`'s `sync_from_cpu()` does (lazy line renderer, spec O-19..O-22). The budget is kept in PPU dots and the frame is cut into the same slices as FCEUX (256 + 6 + 63 + 16 dots per line, special vblank and pre-render handling), so the runtime's frame loop is a port of `nes1`'s `run_one_frame`.

### 6.4 Interrupts
* NMI is raised by the runtime's frame loop at the vblank boundary (12-dot delay, taken at the next instruction boundary; the `$2000`-write-during-vblank rule O-38; a jammed CPU never takes it).
* IRQ line = frame IRQ | DMC IRQ | mapper IRQ (MMC3, MMC5, 91), checked by `NRT_STEP`.
* Taking an interrupt = the ordinary 6502 entry sequence into the vector, then dispatch. Nothing special in generated code, so it works inside any page.

### 6.5 Bank switching and the dispatcher
* `nrt_dispatch(c, pc)`: `window = pc >> 13`; `page = mapper_prg_page[window]` (updated by the mapper on register writes); if `page` has generated code and `pc` is a leader -> call it; else if `pc` is in the `ram_code` table -> its generated function; else -> **interpreter** until the PC reaches a leader again, logging a miss.
* Changes to executable mapping must be seen before the next opcode executes, including indirect
  stores and mapper registers outside `$8000-$FFFF`. Use mapping/version guards and explicit
  yield reasons; never continue executing stale generated constants.

### 6.6 Data and assets
* PRG as `const uint8_t prg[]` (read by `RD` for tables and data), CHR as `const uint8_t chr[]`. CHR-RAM games get a RAM array. Palette, nametable, OAM, PRG-RAM are runtime-owned RAM.
* **Asset separation (for section 12):** `generated/<g>/code/*.c` (derived from game code) is separate from `generated/<g>/assets/{prg,chr}.c` (pure data). A later "bring your own ROM" build can generate only the assets locally.

### 6.7 Build modes
| Mode | Hook | Use |
|------|------|-----|
| `exact` (default) | `NRT_STEP` before every instruction as above | all verification, all platforms at first |
| `trace` | `exact` + per-instruction `PC OP A X Y S P CYCLES SCANLINE DB PL` record and chained hash, same format as `nes1` | oracle comparison, lockstep tests |
| `block-experimental` | conservative block deadlines with interpreter fallback and complete state materialization | size/speed experiments only until it passes the same bit-exact gates; no allowed interrupt-boundary drift |

### 6.8 Flag liveness and code size (optimisation, M9)
Eager flags are the baseline. Dead-flag elimination is allowed only when every possible observer
(interrupt entry, trace, I/O, event yield and interpreter fallback) receives the correct architectural
state. Re-run parity and report linked-size improvements instead of assuming liveness is harmless.

---------------------------------------------------------------------------------------------------

## 7. The runtime (C++17 implementation, C ABI, bounded core storage, no desktop I/O in the core)

### 7.1 Public API (nesrt.h), sketch
```c
/* C-compatible opaque machine handle; storage/initialization contract finalized in M1. */
typedef struct nrt_machine nrt_machine;
int nrt_run_frame(nrt_machine* machine);
int nrt_set_input(nrt_machine* machine, int pad, uint8_t buttons);
const uint8_t* nrt_framebuffer_idx(const nrt_machine* machine);
int nrt_audio_read(nrt_machine* machine, int16_t* out, int n);
/* Separate host adapters: ROM loading, initialization, replay, dump/trace files. */
```
### 7.2 Components (each ported from nes1's parity-verified behaviour, then extended per mapper)
| Component | Source of truth | Notes |
|-----------|-----------------|-------|
| Bus / open bus / controllers / OAM DMA | parity spec O-8, O-14, O-18, O-33 | `$4016/$4017` shift registers; input from `nrt_set_input` or a movie |
| PPU | parity spec O-12 ... O-23, O-34 ... O-37 | lazy per-line renderer producing a palette-index frame buffer (nes1 format: index + emphasis bits); 8x16 sprites, clipping, sprite-0 hit, overflow |
| APU | parity spec O-25 ... O-29 + new audio synthesis | register/counter/IRQ/DMC behaviour = nes1's (bit-exact); **new**: band-limited mixing to PCM (nes1's APU already has pulse/triangle/noise sample generation to start from; DMC output added) |
| Mappers | **new**, clean-room from the NESDev wiki, verified against FCEUX | section 7.3 |
| Interpreter | MIT nes1 `cpu.cpp` logic adapted to the canonical C ABI state | exact trace; independently checked against FCEUX/nestest, then used for fallback |
| NESDUMP/trace | shared format `nes1/common/nesdump.h` | host/test adapter separate from bounded machine-state storage |

The implementation uses portable C++17 with a C ABI and fixed-size machine buffers. Validate
actual linking with MSVC, GCC/Clang, Emscripten and ARM; C++ language choice alone is not proof
of freestanding suitability. Keep exceptions, RTTI and desktop dependencies unnecessary in the core.

### 7.3 Mapper plan
| Mapper | Games | Work | Hard parts |
|--------|-------|------|------------|
| 0 NROM | 9 | trivial | 16 KB mirror at `$8000/$C000` |
| 3 CNROM | 1 | CHR bank select | bus conflicts (written value ANDed with the ROM byte) |
| 2 UxROM | 1 | PRG 16 KB switch, CHR-RAM | bus conflicts |
| 7 AxROM | 2 | 32 KB PRG switch, single-screen mirroring select | one-screen NT mirroring |
| 1 MMC1 | 4 | 5-bit serial shift register, control/PRG/CHR regs, SRAM, mirroring | consecutive-write behaviour, reset by bit 7, PRG-RAM enable, code in SRAM (Zelda) via interpreter / `ram_code` |
| 4 MMC3 | 7 | 8 bank registers, PRG/CHR modes, mirroring, WRAM protect, **scanline IRQ from PPU A12 edges** | exact A12 clocking, reload/latch semantics, IRQ delay by a few CPU cycles; 8x8/8x16 pattern table split; needs the PPU line renderer to expose A12 events with FCEUX's timing |
| 5 MMC5 | 1 | PRG modes with RAM banks, CHR 1K/2K/4K/8K modes with two sets, ExRAM modes (extended attributes, nametable RAM, fill mode), 8x8 multiplier, vertical split, scanline IRQ, pulse x2 + PCM expansion audio | largest effort; scope decided after tracing which features Castlevania III really touches |
| 91 | 1 | register map from the NESDev wiki, PRG/CHR windows, IRQ | sparsely documented; derive from FCEUX behaviour with probe ROMs |

Mapper state is compared by the oracle **only through visible effects** (PRG view, CHR view, nametable view, WRAM, instruction trace with cycles). No FCEUX-internal mapper state needs exporting.

### 7.4 Audio
Bit-exact APU *state* is required (CPU-visible: `$4015`, frame IRQ, DMC); the *PCM output* is not part of the oracle (parity spec 1.9). Output: 44.1 or 48 kHz mono 16-bit, band-limited (blip-style) from cycle-stamped register writes. A reference mixer is specified in `doc/runtime_contract.md`; audio is judged by listening plus a spectral sanity comparison against FCEUX.

### 7.5 Save RAM (battery)
`nrt_hal.storage_read/write` for `$6000-$7FFF` when the header has a battery (Zelda): a file on desktop, IndexedDB on web, a flash sector or SD card on STM32. Off in oracle runs (clean power-on state).

---------------------------------------------------------------------------------------------------

## 8. Platform layers

### 8.1 HAL (platform/hal/nesrt_hal.h), the only thing a platform implements
```c
typedef struct {
  void     (*video_present)(const uint8_t* idx_frame, const uint32_t* palette_rgb); // per frame, or
  void     (*video_line)(int y, const uint8_t* idx_line);          // per scanline (STM32 line streaming)
  void     (*audio_write)(const int16_t* s, int n);                // or the runtime ring buffer is polled
  uint32_t (*input_poll)(int pad);                                 // button bits
  uint64_t (*time_us)(void);   void (*sleep_until_us)(uint64_t);   // frame pacing
  int      (*storage_read)(...); int (*storage_write)(...);
  void     (*log)(const char*);
} nrt_hal;
```
The runtime produces a palette-index frame and PCM; palette -> RGB (with emphasis) is in the platform layer from a shared table.

### 8.2 Windows and Linux (platform/desktop, SDL2)
* SDL2 video (streaming texture or GL), SDL audio queue, SDL_GameController + keyboard; fixed 60.0988 Hz pacing with audio-clock sync, integer scaling, optional CRT filter, fullscreen.
* Build: CMake. Windows: MSVC (VM). Linux: GCC/Clang (Linux VM).
* **SteamOS / Steam Deck:** SteamOS is Arch Linux, so the Linux build runs natively. Deliverables: an AppImage (and/or Flatpak), a `.desktop` file, a Steam-Input-friendly gamepad mapping (SDL_GameController), 1280x800 integer scaling, a launch script usable as a "Non-Steam Game" in Gaming Mode. Distribution through Steam itself is **out of scope** (copyrighted games; needs Steamworks). Verification on a real Deck is a user task; I verify in a Linux desktop session plus a headless Xvfb smoke test.

### 8.3 Web (platform/web, Emscripten)
* `emcmake cmake` to WebAssembly; canvas 2D or WebGL (256x240 palette texture + shader); input via Gamepad API + keyboard; audio through an **AudioWorklet** fed from a ring buffer (SharedArrayBuffer when cross-origin isolated, else postMessage); pacing with `requestAnimationFrame` plus an accumulator for 60.0988 Hz.
* Save RAM in IndexedDB; ROM-free assets only for homebrew builds (section 12).
* Tests: headless Chromium (Playwright) boots the page, runs N frames through an exported `nrt_run_frame`, reads the framebuffer hash and compares it with FCEUX's digest for the same frame (same oracle values as desktop).

### 8.4 STM32 (platform/stm32, generic port layer; board chosen after the spike)
**Memory budget spike (M0, no hardware needed):** compile the generated code of representative games with `arm-none-eabi-gcc -Os -mcpu=cortex-m7` and report `size` per game and mode (exact / interpreter-only / mixed). Planning figures to be verified, not trusted: NES PRG 16 KB ~ 3-6 k instructions; exact-mode code probably 20-40 bytes per instruction => NROM 16-32 KB games ~ 100-300 KB, 128 KB MMC3 games ~ 1 MB, Castlevania III (256 KB) > 2 MB.

| Resource | Need | Notes |
|----------|------|-------|
| RAM | CPU RAM 2 KB + nametable 2 KB + OAM 256 B + palette 32 B + PRG-RAM 8 KB (some games) + CHR-RAM 8 KB (some games) + frame buffer 256x240x8 bit = 60 KB (or only line buffers with line streaming) + audio ring | about 80-100 KB at the high end: fits internal SRAM on F7/H7 (F4 needs line streaming) |
| Flash | generated code + PRG + CHR | see spike; > 1 MB games need an H7 (2 MB) or external QSPI/OCTOSPI flash with XIP |
| CPU | NTSC CPU is roughly 1.79 M emulated cycles/s, not instructions/s; host cost depends on execution mix, PPU and memory | real-time performance must be measured, not inferred from object sizes |

**Candidate boards (decision gate after the spike):** STM32F746G-DISCO (LTDC LCD 480x272, SAI audio, QSPI, SDRAM; 1 MB flash), Nucleo-H743ZI (+ external LCD/audio), STM32H7B3I-DK (2 MB flash, 1.4 MB RAM, LTDC), or any F7/H7 with an RGB/8080 LCD.
**Video:** assess LTDC CLUT use versus conversion using the full palette/emphasis encoding;
a fixed L8 lookup is not automatically equivalent for all emphasis values. SPI/FMC paths use
bounded scanline conversion plus DMA.
**Audio:** timer-paced DAC/SAI DMA double buffer fed from the ring buffer. **Input:** GPIO buttons / USB HID gamepad.
**Build:** CMake + `arm-none-eabi-gcc`, no vendor IDE; startup + linker script per board; ST HAL/LL only inside `platform/stm32/board_*`.
**Test without hardware:** verify support for the chosen board/peripherals before relying on Renode.
Functional simulator tests may compare hashes; real Cortex-M7 throughput, caches, external-memory
latency, LCD and audio must be tested on hardware. Board choice remains deferred.
**Mixed mode as the size lever:** because the interpreter is always available (6502 bytes are 1 byte per byte), the generator takes `--budget-flash N` and recompiles only the hottest pages (by instruction counts from the movies), leaving the rest interpreted. Large games stay *runnable* on small parts, and the recompiled coverage is the quality metric.

---------------------------------------------------------------------------------------------------

## 9. Verification strategy (what "bit-exact with FCEUX" means in practice)

### 9.1 The oracle
FCEUX 2.6.6 with the headless dump runner already built on the Windows VM (nes1 repo). It loads every mapper in this project's scope unchanged, so **no mapper has to be implemented in nes1**; nes1 stays a mapper-0 emulator.

### 9.2 Required extensions of the oracle (workstream W0, in `nes1`, instrumentation only)
| Item | Why |
|------|-----|
| `--input FILE` for both nes1/FCEUX headless runners and the new runtime | strict legacy two-pad parsing plus versioned replay preserving device metadata, Four Score and reset/power commands; see C4 |
| `[WRAM]` section (`$6000-$7FFF`) in the dump | PRG-RAM games (Zelda, ...) |
| `[NTVIEW]` section (the four logical nametable pages as the PPU sees them) | mirroring modes, AxROM one-screen, MMC5 ExRAM / fill mode, mapper 91 |
| Controller serial state and CPU pending-interrupt state | detect delayed divergences before they affect gameplay; include in both captures and hashes |
| Compare tools accept the new sections; digest includes them | `tool/compare_dumps.py`, `find_divergence.py` |
| `NESDUMP 3` format bump, documented in `doc/state_dump.md` | |
No per-mapper internal state is dumped: banking is visible through the CPU's PRG view, the PPU's CHR and NT views, and IRQ timing through the instruction trace (cycles, PC).

### 9.3 Test layers (each has a go/no-go before the next)
| Layer | Test | Oracle |
|-------|------|--------|
| T0 | Python tools: decoder, CDL reader, dbg reader unit tests; ROM rebuild CRCs for all 26 | README CRC list |
| T1 | Interpreter vs `nestest.log` (CPU) | independent log |
| T2 | **Runtime (interpreter mode) vs FCEUX**, mapper-0 games: nestest, Mappy, color_test, probe ROMs, the 9 NROM games on their movies | FCEUX dumps (frame, scanline, trace) via `compare_dumps.py` |
| T3 | **Recompiled vs interpreter in lockstep**: both run the same ROM and input; after every instruction compare PC, A, X, Y, S, P, cycle, data bus, PPU latch (the trace line) and per frame the full state. Any difference is a recompiler bug localised to one instruction | the interpreter (itself proven by T2) |
| T4 | Recompiled vs FCEUX over full movies at frame level; selected frames at scanline and trace level | FCEUX |
| T5 | Mapper probe ROMs (small generated ROMs exercising one mapper behaviour each, e.g. MMC3 IRQ reload/latch cases, MMC1 shift-register edge cases) and per-mapper movie segments | FCEUX |
| T6 | Platform smoke: desktop headless (SDL dummy drivers), web (Playwright), STM32 (Renode) produce the same frame hashes as T4 for the first K frames | FCEUX digest values |
| T7 | The opcode-sweep ROMs from nes1 (`tool/make_opcode_sweep.py`) run through the **recompiler** (ROM -> C -> run), confirming every opcode's translation | FCEUX |

### 9.4 Movies as regression assets
`_misc/*.fm2` exist for nearly every game (walkthroughs up to 12 MB: hours of play). Policy: per game (a) a short smoke segment (first 3000 frames, always), (b) one full-length movie in nightly runs on the VMs (frame-level digest compare only; a mismatch triggers the automatic drill-down of `find_divergence.py`). Movies recorded on other emulators can desync in FCEUX itself; the first step per game is to run the movie in FCEUX alone and record a checksum of an expected game state, confirming the movie is valid for this ROM version.

### 9.5 Coverage metrics (`tools/coverage.py`, after every movie run)
* % of executed instructions that ran recompiled vs interpreted (target 100 %);
* dispatcher misses (page, PC, reached-from) with suggested `game.toml` additions;
* unresolved jump tables from analysis;
* generated code size per page.

---------------------------------------------------------------------------------------------------

## 10. Build, CI and environment

* **Where things run:** Python tools on the Mac (pure Python 3); all C compilation and running on VMs. New VM `nesrecomp1-linux-vm` (Ubuntu, e2-standard-4): build-essential, cmake, ninja, python3, cc65, emscripten, arm-none-eabi-gcc, renode, xvfb, chromium + playwright, libsdl2-dev. The Windows VM (`nes1-win-vm`) stays the MSVC build and the FCEUX oracle host. FCEUX digests for the movies are generated once per game and cached in `tests/golden/<game>/` (hashes only; never ROMs or dumps of copyrighted content).
* **Scripts:** `tools/oracle.ps1` wraps "run FCEUX headless with these frames/input"; `tools/nesrecomp1.py` CLI: `rom`, `analyze`, `generate`, `build`, `run-movie`, `compare`, `coverage`.
* **CI:** GitHub Actions for what needs neither the oracle nor ROMs (tool unit tests, mapper-logic unit tests, codegen of our own test ROMs, platform builds incl. Emscripten and ARM). Oracle-dependent jobs run on the VMs by script (no ROM content ever in Actions).
* **Docs:** `doc/build_environment.md` for both VMs (same style as nes1's) and per-platform build instructions.

---------------------------------------------------------------------------------------------------

## 11. Milestones and work breakdown

Dependencies are in the todo table; no time estimates. Each milestone ends with an exit check.

### M0 Foundations and spikes
* Repo skeleton, submodules, `.gitignore`, CMake skeleton, `doc/plan.md` (this document), `doc/build_environment.md`.
* Create the Linux VM and install the tool-chains; record in the doc.
* **Stage A for all 26 games**: `build_rom.py`; CRC table; list of games whose rebuilt ROM does not match.
* Read all 26 headers / `ld65.cfg`; confirm the mapper table (1.2); confirm absence of expansion audio and four-screen mirroring.
* CDL reader + `.dbg` reader; first `analyze.py` report for Mappy and one MMC3 game (seed coverage, jump-table candidates, unresolved items).
* **Spike S1 (feasibility):** hand-written generated code for ~20 representative instructions + the dispatcher compiled with MSVC, GCC, Emscripten, `arm-none-eabi-gcc`: size and speed per instruction; decides goto-switch vs trampolines.
* **Spike S2 (STM32 budget):** S1 numbers x instruction counts from `analyze.py` for all 26 games -> board-selection table; **ask you to pick the reference board**.
Exit: ROMs rebuilt, tool-chains documented, size table delivered, STM32 board proposal.

### M1 Oracle extension and the runtime core (interpreter mode)
* W0 in `nes1` (9.2): input playback in both headless runners, `[WRAM]`, `[NTVIEW]`, NESDUMP v3, tool updates, `make_input.py`, docs. All 20 existing suite entries must stay identical.
* Runtime core in C++17 with a C ABI: bus, PPU, APU state, controllers, OAM DMA,
  interpreter, per-instance frame loop, mapper 0 and a separate NESDUMP/trace host adapter.
* Desktop headless test executable `nrt_test <rom> --dump ...` (interpreter only).
Exit: T1 and T2: runtime in interpreter mode identical to FCEUX (3000 frames, scanline and trace level) on nestest, Mappy, color_test, the probe ROMs and the nine NROM games with movie starts.

### M2 Recompiler v1 and the first native game
* Decoder, validated instruction/code identities, CFG discovery, `game.toml`, exact per-instruction
  generated execution, safe dispatcher and invalidation-aware interpreter fallback.
* Lockstep harness (T3), opcode-sweep-through-recompiler (T7).
* Mappy and the other NROM games recompiled; coverage reports.
Exit: T3 and T4 for the nine NROM games (lockstep over their movie segments; coverage at target).

### M3 Desktop playable (Windows + Linux)
* SDL2 layer, CMake targets `game_<name>`, audio mixer, pacing, gamepad/keyboard, fullscreen/scaling, save RAM files; Linux and Windows VM builds; Xvfb/dummy-driver smoke test.
Exit: Mappy plays with sound on Windows and Linux; frame hashes in the running game equal the oracle's.

### M4 Web
* Emscripten target, HTML shell, AudioWorklet, Gamepad API, IndexedDB, Playwright test; a **homebrew/test-ROM demo page** as the public-safe artefact (section 12); commercial games' web builds stay private.
Exit: Mappy in a browser (local), Playwright digest matches.

### M5 Mapper wave 1: CNROM, UxROM, AxROM, MMC1
* Mappers 3, 2, 7, 1 with probe ROMs; per-physical-bank pages; `ram_code` for Zelda; battery save.
* Games: Adventure Island, Little Mermaid, Indy Heat, Solstice, Bugs Bunny, Dr. Mario, RoboCop 3, Zelda.
Exit: T3/T4/T5 for these eight games.

### M6 Mapper wave 2: MMC3
* Mapper 4 with the A12-driven IRQ at FCEUX's exact timing (new observations into `doc/fidelity_spec.md`, probe ROMs generated like the nes1 ones), WRAM, mirroring; PPU hooks for A12 events (mid-line pattern table switches).
* Games: Super C, Contra Force, Captain America, Double Dragon II, Felix, Kunio Soccer, Tecmo World Cup.
Exit: T3/T4/T5 for all seven (movie segments with raster effects: status bars, split scroll).

### M7 STM32
* Generic port, board support for the chosen board, firmware build, Renode tests, DWT performance budget, LCD/audio/input on board, mixed-mode budget option.
* Start with NROM games; then up to the flash limit of the chosen board.
Exit: T6 under Renode; hardware bring-up checklist for you; per-game fit table (recompiled / mixed / does not fit).

### M8 Mapper wave 3: MMC5 and mapper 91
* Castlevania III (trace the features used by the movies first; implement those plus a documented subset) and Street Fighter III (mapper 91).
Exit: T4 on their movies.

### M9 Quality and optimisation
* Function recovery, flag liveness and block optimization only when bit-exactness is preserved;
  linked-size/speed measurements, SteamOS packaging, polish and documentation.
Exit: measured size/speed requirements met on the chosen board; all 26 games pass their declared
fixtures or have a separately approved, documented scope limitation.

Game order inside a wave: easiest first (fewest features used).

---------------------------------------------------------------------------------------------------

## 12. Legal and distribution design

* The disassembly source, the ROM, the generated C and the built binaries all contain the games' copyrighted code, graphics and music. **Nothing generated is committed**; `generated/`, `out/`, ROMs, CHR and binaries are in `.gitignore`; `tests/golden/` holds only hashes.
* The disassembly repo is already a derivative work hosted on GitHub; whether it stays public is your decision, outside this project.
* For a **future public release / web demo** the pipeline is ROM-driven and the asset/code split of 6.6 allows (1) shipping only **tools + runtime + metadata (`game.toml`)** and letting each user generate the game from their own ROM locally (as many recompilation projects do), and/or (2) shipping **freely licensed homebrew** built the same way (test ROMs generated by our own `make_probe_rom.py` are ours; a few open-licence homebrew NES games can be added as demo content after a licence check).
* The web demo in M4 uses only our own probe/homebrew ROMs.
* FCEUX stays GPLv2 inside `nes1`'s `tool/`; it is a **test tool only**, never linked into a shipped binary, and no FCEUX code is copied into the runtime.

---------------------------------------------------------------------------------------------------

## 13. Risks and mitigations

| # | Risk | Mitigation |
|---|------|------------|
| R1 | Computed jumps / RTS tricks the analyzer misses | validated interpreter fallback with explicit progress and miss records; report coverage |
| R2 | Code running from RAM / self-modifying | byte/version-guarded translation with write invalidation, otherwise interpreted |
| R3 | Bit-exact timing of mapper IRQs (MMC3 A12, MMC5 scanline counter) | new probe ROMs + FCEUX trace comparison; PPU renderer exposes A12 events at FCEUX's positions; M6's main work |
| R4 | FCEUX quirks inside mappers not visible without reading FCEUX source | same clean-room method as nes1: observe with probe ROMs; document in `fidelity_spec.md`; if reading source is unavoidable, disclose it as in nes1 |
| R5 | Movies desync or were recorded for another ROM revision | per-game movie validation step; short smoke segments always available |
| R6 | STM32 size and speed | spike first; mixed mode; board with XIP flash; `fast` mode as M9 option |
| R7 | MMC5 breadth | scope by tracing what Castlevania III uses; the game is last |
| R8 | Nuts & Milk differs from its recorded original hash | pin the known rebuilt identity and disclose the exception; unrelated mismatches fail |
| R9 | Mac cannot run compiled binaries; VM turnaround | detached runs, cached digests, per-page generated files for incremental builds |
| R10 | Generated C size (hundreds of thousands of lines for big games) slows compiles | one file per page, `-O1` in tests, ccache |
| R11 | Legal exposure from published artefacts | keep game-derived outputs private; public tools/tests contain only appropriately licensed material |
| R12 | Audio quality vs bit-exact state | state is exact; PCM mixer specified separately, judged by ear/spectrum |

---------------------------------------------------------------------------------------------------

## 14. Open items and assumptions to confirm during M0

1. Pin the disassembly repo commit (HEAD `df2c8e5` when inspected).
2. Confirm required hardware features during mapper-specific execution; headers/CDL alone do not
   prove absence of expansion audio. CDL/movie inventory is complete (26/25 respectively).
3. How many movies replay cleanly in FCEUX 2.6.6 for the rebuilt ROM.
4. Whether any game uses the PAL path or reads the region; all assumed NTSC.
5. STM32 reference board (after S2).
6. Repository name and URL are established; keep copyrighted game-derived outputs out of its public history.

## Appendix A: per-game quick table (mapper, sizes, assets, from headers and `_misc` listings)

| Game | Mapper | PRG | CHR | Movies seen in `_misc` |
|------|--------|-----|-----|------------------------|
| Mappy | 0 | 16K | 8K | 2 |
| Battle City | 0 | 16K | 8K | 3 |
| Excitebike | 0 | 16K | 8K | 5 |
| Ice Climber | 0 | 16K | 8K (2 files) | 2 |
| Nuts & Milk | 0 | 16K | 8K | 2 |
| Pac-Man | 0 | 16K | 8K | 1 |
| Son Son | 0 | 32K | 8K | 1 |
| Tennis | 0 | 16K | 8K | 1 |
| Yie Ar Kung-Fu | 0 | 16K | 8K | 2 |
| Adventure Island | 3 | 32K | 32K | 2 |
| The Little Mermaid | 2 | 128K | CHR-RAM | 4 |
| Indy Heat | 7 | 128K | CHR-RAM | 1 |
| Solstice | 7 | 128K | CHR-RAM | 1 |
| Bugs Bunny | 1 | 64K | 32K | 3 |
| Dr. Mario | 1 | 32K | 32K | 2 |
| RoboCop 3 | 1 | 128K | 128K | 1 |
| The Legend of Zelda | 1 | 128K | CHR-RAM + SRAM | 4 |
| Super C | 4 | 128K | 128K | 2 |
| Contra Force | 4 | 128K | 128K | 1 |
| Captain America | 4 | 128K | 128K | 4 |
| Double Dragon II | 4 | 128K | 128K (116K raw CHR plus linked screen data) | 1 |
| Felix | 4 | 128K | 128K | 1 |
| Kunio Soccer | 4 | 128K | 128K | 3; exclude unsupported FDS-command fixture unless implemented |
| Tecmo World Cup | 4 | 64K | 64K | 3 |
| Castlevania III | 5 | 256K | 128K | 4 |
| Street Fighter III | 91 | 128K | 512K | 0; requires a new supported fixture |

## Appendix B: the daily oracle loop for a recompiled game

1. `nesrecomp1 build mappy --platform desktop --mode trace` -> `out/mappy/nrt_test`
2. `make_input.py mappy_walkthrough.fm2` -> `mappy.in`
3. FCEUX: `fceux64 mappy.nes --dump o_f --frames 3000 --input mappy.in --digest --dump-frames all`
4. Ours: `nrt_test mappy.nes --dump o_n --frames 3000 --input mappy.in --digest --dump-frames all`
5. `compare_dumps.py o_n o_f --summary`; if different, `find_divergence.py` drills down to the first frame, scanline, instruction and field (the tools already used in nes1).
6. Recompiled vs interpreter in lockstep: the same run with `--cpu interp` vs `--cpu recomp` and `compare_traces.py`.
