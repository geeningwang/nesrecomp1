# M1 replay status

The FM2 converter in `tools/make_input.py` is implemented with 10 unit tests. Together with
the 16 foundation tests, `python -m unittest discover -s tests -p "test_*.py"` runs 26 tests.

Supported conversion: text FM2 version 3, NTSC, two ordinary ports, Four Score and the Famicom
four-player attachment. Metadata, button positions, record order and ROM MD5 are validated.
Savestate/save-RAM starts, binary movies, unsupported devices/commands and malformed data fail.

The NESINPUT 1 contract and emulator CLI live upstream in
[nes1's input documentation](https://github.com/geeningwang/nes1/blob/main/doc/input_replay.md).
The dependency pin is advanced only after the upstream changes are verified and committed.
The verified controller-only checkpoint is nes1 `136ed04`. The pinned dependency still precedes
this work until the remaining M1 oracle contract is complete; run replay checks using an updated
separate nes1 checkout in the meantime.

## Verified execution

- C/C++ ABI smoke and 16 foundation unit tests execute on the Windows VM.
- Mappy TAS and Four Score walkthrough: normalized replay matches native FCEUX FM2 playback
  for 3000 frame digests and 11 selected text-trace frames each.
- Both emulators match on every frame and visible scanline of both 3000-frame input-enabled
  Mappy fixtures. Twelve selected text-trace frames compare 113,198 TAS instructions and
  116,389 walkthrough instructions line by line.
- Synthetic reports match under five attachment configurations: two pads, Four Score,
  Famicom four-player, both adapters and a disconnected second pad.
- Gameplay exposed a fine-X/background-pipeline mismatch in nes1; regression probes and a
  persistent two-tile pipeline correct it.
- The expanded 22-entry no-input suite passes at 3000 frames per entry (frame-level digests,
  including the instruction-chain hash). This is not a claim that the full suite's v3 or
  3000-frame scanline gate is complete.

Evidence uses the locally rebuilt Mappy ROM and explicitly selected 3000-frame prefixes.
It does not establish full-length movie coverage.

## Reset and power commands

nes1 `3f40791` executes FM2 reset and power commands in both runners at the start of their frame,
power before reset, and models FCEUX's state semantics (spec O-41). Besides the `reset_state` probe
(90 frames, four command frames, 150,320 trace instructions, identical dumps), movies with commands
equal native FCEUX FM2 playback at every scanline for 3000 frames, with selected traces compared line
by line: Excitebike TAS (power, reset at frame 1), Excitebike playthrough (resets at 528, 1055, 1611),
Ice Climber walkthrough (reset, power, reset at 1267, 1300, 1327) and Yie Ar Kung-Fu TAS. Removing
the commands from a replay changes the digests at exactly those frames, so the check is sensitive.

## NESDUMP 3 (nes1 `2ea42f3`)

The dump format now includes cartridge work RAM, the logical nametables, controller serial state
and pending-interrupt state, and the comparison tools reject anything that is not version 3
(old versions, missing or short sections, truncated files, mixed digest/full folders, manifests
that disagree with the files). Digests are compared as streams. `tool/test_dump_format.py` flips one
byte in each section and checks it is reported in both full and digest mode.

Adding the new sections immediately found one real state difference: nes1 kept the whole byte of the
last `$4016` write as its strobe state while FCEUX keeps bit 0 (no behavioural effect; fixed, O-43).

## Replay gate results (`tools/run_movie_gate.py`, NESDUMP 3)

| Tier | Games | Result |
|---|---|---|
| 3000 frames, every scanline, 10 trace frames + the last, native vs normalized | Mappy TAS and walkthrough (Four Score + Famicom adapter), Pac-Man, Son Son, Tennis, Excitebike TAS 2 and 4 and playthrough, Ice Climber walkthrough, Yie Ar Kung-Fu TAS | identical |
| 10,000 frames (the whole movie) | Battle City misc (reset at frame 9990) | identical |
| 30,000 frames, frame digests, same trace frames | Mappy walkthrough, Pac-Man, Son Son, Tennis | identical |

The no-input suite (23 entries) is identical at 3000 frames at every scanline.

Not covered: full-length movies (4,377 to 713,500 frames). The Nuts & Milk movies cannot be used: their
recorded ROM checksum belongs to the original ROM, while the disassembly rebuilds a different binary
(see `m0_report.md`), so `make_input.py` correctly refuses them. Dr. Mario and the other mapper 1/3/4
titles need their mappers first.

## Still required for M1

1. A full-length detached run (tiered policy) and an update of this table with its manifest.
2. Implement the portable C++17 runtime/C ABI and prove its interpreter mode against the oracle.

FDS commands are rejected when the replay file is read; they are never silently dropped.
