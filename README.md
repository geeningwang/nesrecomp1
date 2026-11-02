# nesrecomp1

Re-compiles NES games, given as ca65 disassemblies ([geeningwang/NES-Games-Disassembly](https://github.com/geeningwang/NES-Games-Disassembly)),
into native programs for Windows, Linux (incl. SteamOS), the web (WebAssembly) and STM32 microcontrollers.
The target is **bit-exactness against FCEUX 2.6.6**, using the parity tooling of
[geeningwang/nes1](https://github.com/geeningwang/nes1).

Status: M0 foundation tests pass on Windows; M1 deterministic input replay is in progress.
The portable runtime and a working recompiled game are not implemented yet.
See [doc/plan.md](doc/plan.md).

```
deps/disassembly   the 26 game disassemblies (git submodule)
deps/nes1          emulator + FCEUX oracle + comparison tools (git submodule)
tools/             Python tools: ROM rebuild, CDL/dbg readers, analysis, code generator
runtime/           C++17 core with C ABI; generated game code remains C
platform/          platform layers: hal/, desktop/ (SDL2), web/, stm32/
games/<game>/      per-game annotations (game.toml)
doc/               plan, design, build environment, fidelity notes
```

Nothing derived from the games (ROMs, generated C, binaries) is ever committed (`generated/`, `out/` are ignored).
Clone with `git clone --recurse-submodules`.

## Stage A: rebuild a ROM from its disassembly

```
python tools/build_rom.py "Mappy"          # needs ca65/ld65 (cc65) on PATH, or --ca65/--ld65
python tools/build_rom.py --all
```
Writes `out/<game>/game.nes`, `game.dbg`, `game.map`, `rom_info.json`. 25 of the 26 games reproduce the SHA-1 recorded
by the disassembly; Nuts & Milk does not reproduce it even with the disassembly's own script (see `doc/m0_report.md`).

## Stage B: analysis

```
python tools/analyze.py "Super C"
```

## M1: normalize movie input

```
python tools/make_input.py movie.fm2 --rom game.nes --output replay.in --frames 3000
python -m unittest discover -s tests -p "test_*.py" -v
```

The converter preserves controller attachment, four pads and reset/power bits, and validates
the FM2 ROM checksum. The current headless runner rejects nonzero reset/power commands until
their execution is verified. See [doc/replay.md](doc/replay.md) for the contract and evidence.
