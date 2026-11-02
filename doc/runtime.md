# Runtime core (M1-C)

`runtime/` is the portable machine: a C++17 core behind a C API (`runtime/include/nrt.h`). It models an NTSC NES
with mapper 0 and is bit-for-bit equal to FCEUX 2.6.6 on everything the parity work has checked. It is the engine
that generated game code, the desktop, web and STM32 front ends will share. This milestone provides the
**interpreter only**; no generated code runs yet.

## Design

- One `nrt_machine` holds CPU, PPU, APU, bus and the frame scheduler. All state lives in a caller-supplied block
  (`nrt_machine_size()` = 14,688 bytes, 8-byte aligned). No globals, no allocation, no file or console I/O, no
  host headers; two machines can be interleaved and a block can be re-initialised.
- ROM is passed as immutable views (`nrt_cart`): 16 or 32 KB PRG, 8 KB CHR. The frame buffer is optional and
  caller-owned: 256x240 palette-index bytes (bits 0-5 NES colour, bits 6-7 an emphasis flag), written a line at a
  time. Colour conversion is the host's job.
- `nrt_hooks` give the observation points the parity tools use (before every instruction, after every visible
  line). `nrt_get_state` and the memory accessors read state without side effects.
- Cycle counts are 64-bit. Reset and power commands (`nrt_soft_reset`, `nrt_power_cycle`) follow nes1 spec O-41.

## Provenance

The code is a restructuring of the paths nes1 exercises (MIT): the lazy line renderer, the interpreter and the
CPU-visible APU. The opcode table and every instruction body in `nrt_cpu.cpp` are extracted mechanically from
nes1's `cpu.cpp`; only the memory-access macros differ (2 KB RAM + 8 KB work RAM + a read-only PRG view instead
of a flat 64 KB array). No FCEUX code is part of the runtime. FCEUX is only the oracle the tests run.

Not in this milestone: audio synthesis (the APU models only what the CPU can observe: frame counter, length
counters, DMC fetches and IRQs), mappers other than 0, battery saves.

## Verification

Harness: `tests/nrt_test.cpp` takes nes1's dump command line and writes NESDUMP 3 files, so
`tool/find_divergence.py`, `compare_traces.py` and `run_parity.ps1 -Nes1 nrt_test.exe` compare it with FCEUX
unchanged. Windows (MSVC, `/W4 /WX`) is the primary host.

| Check | Result |
|---|---|
| Independent nestest log (`nestest.log`) | 8991 of 8991 instructions |
| 23-entry suite (nestest, Mappy, color test, 19 probes), 3000 frames, every scanline, vs FCEUX | identical |
| Opcode sweep, normal and I/O mode, 10 frames per ROM, vs FCEUX | identical |
| Reset/power probe (`check_reset_replay.py`), 90 frames, 150,320 trace instructions | identical |
| Controller serial, 5 attachments (`check_input_replay.py`) | identical |
| Movies via `tools/run_movie_gate.py --emulator nrt_test.exe`: 3000 frames at every scanline + line-by-line traces + native FM2 playback (Mappy TAS and walkthrough, Pac-Man, Son Son, Tennis, Excitebike x3, Ice Climber, Yie Ar Kung-Fu) | identical |
| Battle City misc, full 10,000 frames incl. its reset | identical |
| API contract (`tests/rt_isolation_test.cpp`): repeat, interleaved machines, re-init, power cycle, bad carts/storage | passes |

Portability (secondary, recorded separately): on the Linux VM the runtime builds warning-free with GCC 11, Clang and
Clang + ASan/UBSan, the isolation and smoke tests pass, and `nrt_test` digests for 22 ROMs plus one replay at 3000
frames are byte-identical to the MSVC build's. See `build_environment.md` for the sanitizer caveat on that VM.

Size data point (compile only, nothing was run on a device): the four source files are about 14.4 KB of Thumb-2 with
`arm-none-eabi-g++ -Os` for Cortex-M4/M7, with no static data; the machine state is 14.7 KB plus the optional 60 KB
frame buffer. Speed on real hardware is unmeasured.

## Known gaps

- The old block-level prototype `runtime/include/nesrt_ops.h` still has its own `nrt_cpu` state struct. It has to be
  reconciled with the interpreter's CPU state before generated code is linked (plan C6/C8).
- 8x16 sprites, left-column clipping and the other checked corners are covered through the suite and probes, not
  through every game; games outside the NROM set need more mappers first.
- Full-length movies (up to 713,500 frames) have not been run.
