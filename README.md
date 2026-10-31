# nesrecomp1

Re-compiles NES games, given as ca65 disassemblies ([geeningwang/NES-Games-Disassembly](https://github.com/geeningwang/NES-Games-Disassembly)),
into native programs for Windows, Linux (incl. SteamOS), the web (WebAssembly) and STM32 microcontrollers.
The result is verified **bit-exactly against FCEUX 2.6.6** with the parity tooling of
[geeningwang/nes1](https://github.com/geeningwang/nes1).

Status: planning done, milestone M0 (foundations and spikes) in progress. See [doc/plan.md](doc/plan.md).

```
deps/disassembly   the 26 game disassemblies (git submodule)
deps/nes1          emulator + FCEUX oracle + comparison tools (git submodule)
tools/             Python tools: ROM rebuild, CDL/dbg readers, analysis, code generator
runtime/           portable C runtime (CPU interface, bus, PPU, APU, mappers)
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
