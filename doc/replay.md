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

## Still required for M1

1. Implement reset/power commands and prove their frame-boundary and state semantics.
2. Add NESDUMP 3 work RAM, logical nametable, controller and pending-interrupt state, plus
   strict version/required-section comparison.
3. Finish the full replay/regression gates and update the nes1 dependency.
4. Implement the portable C++17 runtime/C ABI and prove its interpreter mode against the oracle.

Until reset/power execution is verified, commands are preserved in converted files but rejected
by the normalized runners before any emulation. They are never silently dropped.
