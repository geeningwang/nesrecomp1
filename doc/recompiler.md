# Static recompilation, exact mode (M2)

`tools/gen_insn.py` turns the 6502 code of an NROM game into C that links with the runtime (`runtime/`) into a
native program. This first recompiler is deliberately simple and exact: **one C function per instruction**,
executing precisely the steps the interpreter executes, dispatched by the PC the CPU really has.

## How it works

1. **Code map.** Recursive descent from the reset/NMI/IRQ vectors, plus (when available) the ld65 code labels and
   the CDL runs of the disassembly, plus an optional list of executed PCs (`--seeds`, fed back from a run). Every
   instruction start found becomes a function.
2. **Function body.** The operand fetch, the operation and the store of the instruction are cut out of
   `runtime/src/nrt_cpu.cpp` (the interpreter, itself extracted from nes1) and pasted into a function whose opcode,
   operand bytes, addressing mode and cycle counts are constants; the C compiler folds the rest. Because the bodies
   are copied rather than rewritten, the interpreter and the generated code cannot drift apart. They are still not
   each other's proof: both are checked against FCEUX and the independent nestest log.
3. **Dispatch.** The runtime looks up `slot[PC offset]` (2 bytes per PRG byte) and calls `fns[slot - 1]`; slot 0
   runs the interpreter instead. Interrupt entry, the per-instruction observation hook and all bus, PPU and APU
   behaviour stay in the runtime, so interpreted and generated instructions can be mixed freely.
4. **Identity.** A module carries `nrt_rom_hash()` of the PRG and CHR it was generated from; attaching it to any other
   ROM is refused. The harness also refuses a ROM it has no module for.

Consequence for correctness: dispatch is by the actual PC, so a wrong guess of the code map (an operand byte
treated as an opcode, a missing entry) costs only coverage. An address with no function runs in the interpreter and
is reported as "missed". Instructions whose operand bytes would run off the end of PRG are not generated.

Not covered yet, and handled by the interpreter if it ever happens: code executed from RAM or work RAM (nothing in
the NROM set does it), mappers other than 0, and block-level optimisation (flag liveness, bulk cycle charging),
which stays experimental until it passes the same gates.

## Use

```
python tools/gen_insn.py --game "Mappy"                    # from out/<slug>/ (build_rom.py), uses CDL if present
python tools/gen_insn.py --rom x.nes --name x              # any mapper-0 image, descent from the vectors
python tools/gen_registry.py [slug ...]                    # generated/registry.c
cmake -DNRT_GAMES="mappy;tennis" ...                       # or ALL: builds nrt_native with those games linked in
nrt_native game.nes --dump OUT --frames 3000 --digest ...  # same command line as nes1/FCEUX; writes OUT/coverage.txt
python tools/coverage_report.py DIR                        # generated vs interpreted, missed PCs
```
`NRT_NO_CODE=1` runs the same executable interpreter-only. Generated sources are build artefacts under
`generated/` and are never committed.

## Evidence (Windows VM, MSVC; every row compares the generated-code executable with FCEUX 2.6.6)

| Check | Result |
|---|---|
| Independent nestest log | 8991 of 8991 instructions |
| Reset/power replay (`check_reset_replay.py`) and the 5 controller attachments (`check_input_replay.py`) | identical |
| 242-opcode sweeps, RAM and I/O modes, **120 variants each** (484 ROMs, all complete well within 80 frames), 80 frames, frame digests with the instruction-chain hash | identical, **366,209,325 generated / 0 interpreted** instructions |
| 23-entry suite (probes, color test, Mappy, nestest), 3000 frames, every scanline | identical |
| Nine NROM games (Mappy TAS + walkthrough, Pac-Man, Son Son, Tennis, Excitebike x3, Ice Climber, Yie Ar Kung-Fu, Battle City full 10,000 frames, Nuts & Milk without input), 3000 frames every scanline + line-by-line traces + native FM2 playback | identical, 770,835,469 generated / 120 interpreted = 99.99998% |
| Interpreter only (`NRT_NO_CODE`, `nrt_test`), suite, 3000 frames | identical (regression after the CPU state refactor) |

What the numbers mean.

- The 120 interpreted instructions were four addresses of computed control flow that the code map had not found:
  Battle City `$EC88` and Mappy `$F746`, `$F761`, `$F77C`. They are now recorded in `games/<slug>/executed_pcs.txt`,
  which `gen_insn.py` loads automatically (3 and 1 extra functions). This is the intended loop: run, read
  `coverage_report.py`, feed the missed PCs back.
- Plain `.nes` modules generated without assembler metadata (the suite) run partly in the interpreter by design:
  nestest executes code from RAM, `cpu_flags_brk` returned from BRK to an address descent had not followed (fixed:
  BRK now falls through to BRK+2). Parity held throughout while generated and interpreted instructions alternated,
  which is the fallback being exercised and counted as the plan requires.
- Linked sizes: the nine-game executable is about 11 MB for 49,000 generated instructions (about 80 bytes of x86-64 per
  instruction at `/O2`, plus 2 bytes of slot table per PRG byte); a single game is 0.5 MB. Compile time is about 6 ms
  per instruction function on one core. Embedded size and speed are not measured yet.

Not done: block-level optimisation, mappers other than 0, full-length movies, and generated-code speed
measurements.
