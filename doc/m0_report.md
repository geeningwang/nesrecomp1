# M0 report: foundations and spikes

Status of milestone M0 of [plan.md](plan.md). Object sizes below are experimental measurements,
not evidence of instruction correctness, real-time performance or complete firmware fit.

## Continuation validation

- Windows MSVC 19.44 now compiles **C** operation code and links it with a **C++17** bus adapter.
  CTest executes stack/memory/arithmetic and C ABI checks successfully.
- CTest also runs 16 Python unit tests for metadata, source preparation, game-specific identity,
  CHR-RAM/embedded-CHR/split output, missing assets, CDL and debug readers.
- All 26 ROMs were rebuilt again with strict size and game-specific SHA-1/CRC checks: 25 match,
  and only the exact known Nuts & Milk hash pair is accepted as an upstream exception.
- Linux execution and complete-runtime validation remain open. No compiled native game has run yet.

Reproduce on the Windows host:

```
cmake -S . -B build -A x64 -DPython3_EXECUTABLE=C:\work\python\python.exe
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools/build_rom.py --all --ca65 <ca65.exe> --ld65 <ld65.exe>
```

## 1. Stage A: all 26 disassemblies rebuild

`tools/build_rom.py --all` (ca65/ld65 from the disassembly repo, run on the Windows VM; plain Python replaces `preparations.lua`
and the final `copy /B`) rebuilds every game.

| Result | Games |
|--------|-------|
| Byte-identical to the SHA-1 recorded by the disassembly | 25 of 26 |
| Not identical | **Nuts & Milk**: the disassembly's *own* `assemble.bat` (run with its shipped lua/ca65/ld65) produces the same bytes as our script, so the recorded SHA-1/CRC does not match the upstream sources themselves. Recorded as `upstream_mismatch` in `rom_info.json`; the oracle always uses our rebuilt ROM, so this does not block anything. Worth a look in the disassembly repo. |

Things the disassemblies do that the tool had to learn: `replace_with_latin` (Kunio-kun uses Cyrillic identifiers),
`split.lua` (Double Dragon II keeps static-screen CHR inside the PRG link and splits it afterwards), CHR segments linked
into `PRG_ROM.bin` (Ice Climber), games with no CHR file (CHR-RAM), `NES_IGNORE_COMPILE_ASM_ARRAY` (include-only files).
Mapper/size table of section 1.2 of the plan is confirmed by the headers of the rebuilt ROMs.

## 2. Stage B spike: how good is the discovery?

`tools/analyze.py` (decoder table generated from nes1's parity-verified `cpu.cpp`, FCEUX CDL, ld65 debug info):

| Game | CDL code runs | decode cleanly | instructions (CDL) | reached by descent from labels | CDL-only (computed flow) |
|------|--------------|----------------|--------------------|--------------------------------|---------------------------|
| Mappy | 66 | **66** | 5286 | 4812 (91 %) | 474 |
| Super C | 424 | **424** | 19909 | 15524 (78 %) | 4385 |
| The Legend of Zelda | 261 | **261** | 16893 | 15176 (90 %) | 1717 |

* Every tested CDL run decodes onto its end, but CODE flags also mark operands. This is supporting
  evidence, not proof of all instruction starts. M2 must reconcile assembler metadata and executed-PC
  evidence, including ambiguous/overlapping code.
* What descent cannot reach is flow through jump tables / pointers (flagged `IND_CODE` in the CDL, used as extra entry points)
  and the data-vs-code ambiguity at the edges. The interpreter fallback covers anything missed, and the miss log of the
  dispatcher will close the rest (plan 5.2 step 5).
* The `RTS` trick pattern detector found nothing in these three games with the first, rough pattern; to be refined in M2 with the
  jump-table work.

## 3. Spike S1: generated code feasibility

`runtime/include/nesrt_ops.h` (all official + undocumented opcodes as macros, flag mask chosen at generation time) and
`tools/codegen.py` (one C function per ld65 bank segment, `goto` inside the segment, return to the dispatcher on leaving it,
backward flag-liveness) produce C that compiles warning-free with Apple clang for Thumb-2 (Cortex-M7 and M4) and
wasm32 (compile-only on the Mac; MSVC and GCC are checked in M2 on the VMs). The code uses no GNU extensions, only
`static inline`, so MSVC should accept it.

Measured on Mappy (5370 instructions), `-Os`, Cortex-M7:

| Variant | Bytes per 6502 instruction |
|---------|---------------------------|
| per-instruction deadline check (`--hook insn`) | 29.6 |
| all flags computed (no liveness), per-instruction check | 34.7 |
| **block-level check (`--hook block`, default)** | **13.7** |
| no hook at all (lower bound) | 9.5 |
| `-O2` instead of `-Os` | 14.2 |

### Experimental optimization: block-level deadline, interpreter near events

The following describes the size experiment, not a verified timing implementation. The approved
continuation restores eager, per-instruction execution as the correctness baseline. Block charging,
event deadlines and flag elimination require parity tests before becoming production defaults.

The per-instruction hook of the plan (section 6.3) cost 20 of 29.6 bytes per instruction. Replaced by:

* **Exact mode = one deadline check per basic block** against the block's *worst-case* cost (base cycles plus every possible
  page-cross and taken-branch extra). If the deadline is farther away, the whole block runs and its base cycles are charged
  at once; bus I/O inside the block gets the exact time through `c->tadj`.
* The intended slow path returns before a block and interprets near events. Its accuracy and cost
  have not been measured. Mid-block I/O, IRQs, DMC stalls and state materialization must be covered.
* Deadlines must be conservative (never later than the true event). Mapper IRQs that depend on PPU rendering position are
  computed by the PPU model as a lower bound.
* The per-instruction hook (`--hook insn`) stays for **trace builds** (lockstep tests and the NESDUMP instruction hash).

Plan sections 6.3 and 6.7 are updated accordingly.

## 4. Spike S2: code size of all 26 games (Thumb-2, Cortex-M7, `-Os`, block hook, flag liveness)

All code the analysis found is recompiled (CDL + descent), which is an upper bound for a mixed-mode build.

| Game | Mapper | PRG | CHR | Insns recompiled | Thumb-2 code | per insn | wasm32 object | Flash total (code+PRG+CHR) |
|------|-------|-----|-----|-----------------|-------------|---------|---------------|--------------------------|
| Battle City | 0 | 16K | 8K | 5085 | 53K | 10.8 | 108K | 77K |
| Excitebike | 0 | 16K | 8K | 5587 | 87K | 16.0 | 174K | 111K |
| Ice Climber | 0 | 16K | 8K | 6942 | 93K | 13.8 | 185K | 117K |
| Mappy | 0 | 16K | 8K | 5370 | 72K | 13.7 | 149K | 96K |
| Nuts & Milk | 0 | 16K | 8K | 5689 | 55K | 10.0 | 113K | 79K |
| Pac-Man | 0 | 16K | 8K | 4637 | 63K | 14.0 | 129K | 87K |
| Son Son | 0 | 32K | 8K | 8168 | 78K | 9.8 | 159K | 118K |
| Tennis | 0 | 16K | 8K | 5301 | 79K | 15.3 | 153K | 103K |
| Yie Ar Kung-Fu | 0 | 16K | 8K | 5053 | 69K | 14.2 | 138K | 93K |
| Bugs Bunny Crazy Castle | 1 | 64K | 32K | 7208 | 80K | 11.4 | 164K | 176K |
| Dr. Mario | 1 | 32K | 32K | 6145 | 91K | 15.3 | 181K | 155K |
| RoboCop 3 | 1 | 128K | 128K | 10114 | 132K | 13.4 | 262K | 388K |
| The Legend of Zelda | 1 | 128K | 0K | 18016 | 232K | 13.2 | 458K | 360K |
| The Little Mermaid | 2 | 128K | 0K | 12425 | 155K | 12.8 | 304K | 283K |
| Adventure Island | 3 | 32K | 32K | 6923 | 64K | 9.6 | 130K | 128K |
| Captain America and The Avengers | 4 | 128K | 128K | 28449 | 465K | 16.7 | 842K | 721K |
| Contra Force | 4 | 128K | 128K | 22923 | 342K | 15.3 | 676K | 598K |
| Double Dragon II | 4 | 128K | 128K | 22619 | 361K | 16.4 | 736K | 617K |
| Felix the Cat | 4 | 128K | 128K | 15264 | 255K | 17.1 | 428K | 511K |
| Kunio-kun no Nekketsu Soccer League | 4 | 128K | 128K | 29841 | 494K | 17.0 | 877K | 750K |
| Super C | 4 | 128K | 128K | 26560 | 634K | 24.5 | 1141K | 890K |
| Tecmo World Cup Soccer | 4 | 64K | 64K | 12035 | 130K | 11.1 | 255K | 258K |
| Castlevania 3 | 5 | 256K | 128K | 48478 | 619K | 13.1 | 1247K | 1003K |
| Danny Sullivan's Indy Heat | 7 | 128K | 0K | 12335 | 191K | 15.9 | 386K | 319K |
| Solstice | 7 | 128K | 0K | 12876 | 167K | 13.3 | 335K | 295K |
| Street Fighter 3 | 91 | 128K | 512K | 7280 | 119K | 16.7 | 193K | 759K |

Findings:

* The table covers generated object code plus ROM assets, not a linked runtime or firmware.
  It does not establish that any board fits all games; verified scheduling and runtime costs may
  increase these figures. The 40-70 KB runtime estimate remains unverified.
* NROM games (the 9 simplest) need about 80-120 KB in total: they fit even an STM32F4 with 512 KB-1 MB.
* Super C stands out at 24.5 bytes per instruction (indexed addressing everywhere); an obvious optimisation target (M9).
* wasm32 objects are about 2x the Thumb size (uncompressed; gzip will take them far down); still only 0.1-1.2 MB.
* Compile time of the biggest game (Super C, 650 KB of Thumb code) is a few seconds per page.
* No target throughput has been measured. Functional simulator results cannot establish real
  Cortex-M7 speed, external-memory/cache effects or display/audio timing.

## 5. STM32 reference board proposal (decision for you)

| Option | Flash / RAM | Display / audio | Fits | Comment |
|--------|-------------|-----------------|------|---------|
| **STM32F746G-DISCO** | 1 MB flash, 340 KB RAM, 16 MB QSPI flash, 16 MB SDRAM | 480x272 LTDC LCD with touch, SAI audio codec, USB | all NROM/MMC1/UxROM/AxROM/CNROM games from internal flash; MMC3 and CV3 from QSPI (XIP) | cheapest all-in-one; 216 MHz is tight for the PPU, may need a simplified (still FCEUX-identical) renderer path or 2x skipped line cache |
| **STM32H7B3I-DK** | 2 MB flash, 1.4 MB RAM, 64 MB OctoSPI, 16 MB SDRAM | 480x272 LTDC, audio codec, USB | everything from internal flash | 280 MHz, best headroom; costs more |
| Nucleo-H743ZI + LCD shield | 2 MB / 1 MB RAM | needs a separate display/audio | everything | most work on peripherals |

Board selection is deferred until linked flash/RAM and real performance budgets are available.
The F746/H7 boards above are candidates, not verified targets. A fixed 8-bit CLUT must also be
checked against the full palette/emphasis encoding; conversion may be necessary.

## 6. Still open from M0

* **Linux build VM not created**: `gcloud auth` for `admin@jingnw.altostrat.com` expired. Needs `gcloud auth login`. The startup
  script is ready (`doc/build_environment.md`). Until then MSVC on the Windows VM is the only place where code can be run.
* The C/C++ ABI smoke is verified with MSVC. Whole-game generated-code semantics, GCC validation
  and full runtime linking remain M1/M2 work.
* `tools/analyze.py` is a report; the full code map with jump-table recognition is M2.
