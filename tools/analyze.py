#!/usr/bin/env python3
"""Stage B (spike version): analyse one rebuilt game and report how well the code can be discovered.

    python tools/analyze.py <game folder name> [--out DIR] [--disasm DIR] [--json]

Inputs: out/<slug>/{game.nes,game.dbg,rom_info.json} (from build_rom.py) and the CDL file from the
disassembly's _misc folder.  The PRG image is split into the ld65 bank segments (CPU address <-> PRG offset);
a "region" is one such segment.

What it does:
  1. decodes every contiguous CDL "code" run linearly and checks that the decode ends exactly on the run end
     (a clean decode proves the run starts on an instruction boundary);
  2. recursive descent from vectors + ld65 code labels, following branches, JSR/JMP, fall-through;
  3. compares the two: code the CDL saw but descent cannot reach (indirect targets) and the reverse;
  4. lists computed-control-flow sites: JMP (ind), RTS preceded by PHA/PHA (RTS trick), self-modifying stores
     into the code area and PRG-RAM code.

The full code map (basic blocks, tables, cycles) is the next stage (M2, tools/codemap.py).
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdl as cdlmod      # noqa: E402
import dbg as dbgmod      # noqa: E402
from opcodes import OPCODES   # noqa: E402

ROOT = os.path.dirname(HERE)
BRANCHES = {"BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"}


class Game:
    def __init__(self, out_dir, disasm_dir, game):
        self.info = json.load(open(os.path.join(out_dir, "rom_info.json")))
        rom = open(os.path.join(out_dir, "game.nes"), "rb").read()
        self.prg = rom[16:16 + self.info["prg_size"]]
        self.dbg = dbgmod.Dbg(os.path.join(out_dir, "game.dbg"))
        # regions: PRG offset <-> CPU address, only the code-carrying segments
        self.regions = []
        for name, start, size, off in self.dbg.prg_segments():
            if off + size <= len(self.prg):
                self.regions.append({"name": name, "start": start, "size": size, "off": off})
        self.regions.sort(key=lambda r: r["off"])
        cpath = cdlmod.find_cdl(os.path.join(disasm_dir, game))
        self.cdl = cdlmod.Cdl(cpath, len(self.prg)) if cpath else None

    def region_of_off(self, off):
        for r in self.regions:
            if r["off"] <= off < r["off"] + r["size"]:
                return r
        return None

    def off_to_cpu(self, off):
        r = self.region_of_off(off)
        return r["start"] + off - r["off"] if r else None

    def cpu_to_off(self, addr, near_off):
        """PRG offset for a CPU address as seen from code at PRG offset near_off.
        Same bank segment first; otherwise a region whose CPU range is unique (the fixed bank)."""
        r0 = self.region_of_off(near_off)
        if r0 and r0["start"] <= addr < r0["start"] + r0["size"]:
            return r0["off"] + addr - r0["start"]
        hits = [r for r in self.regions if r["start"] <= addr < r["start"] + r["size"]]
        if len(hits) == 1:
            return hits[0]["off"] + addr - hits[0]["start"]
        return None


def decode(game, off):
    """(mnemonic, mode, size, operand) at PRG offset off, or None when the bytes run off the image."""
    op = game.prg[off]
    mn, mode, size, cyc, extra = OPCODES[op]
    if mn == "*KIL":
        return mn, mode, 1, None, op
    if off + size > len(game.prg):
        return None
    operand = None
    if size == 2:
        operand = game.prg[off + 1]
    elif size == 3:
        operand = game.prg[off + 1] | (game.prg[off + 2] << 8)
    return mn, mode, size, operand, op


def successors(game, off, ins):
    """(fallthrough?, [targets as PRG offsets or ('unresolved', cpu_addr)])"""
    mn, mode, size, operand, op = ins
    cpu = game.off_to_cpu(off)
    tg = []
    if mn in BRANCHES:
        t = (cpu + 2 + (operand - 256 if operand >= 128 else operand)) & 0xFFFF
        tg.append(t)
        return True, tg
    if mn == "JMP":
        if mode == "jmp_absolute":
            return False, [operand]
        return False, []                 # JMP (ind): computed
    if mn == "JSR":
        return True, [operand]
    if mn in ("RTS", "RTI", "BRK", "*KIL"):
        return False, []
    return True, []


def descend(game, seeds):
    """Recursive descent. Returns (insn_starts set, unresolved targets set, computed sites list)."""
    seen, work, unresolved, computed = set(), list(seeds), set(), []
    while work:
        off = work.pop()
        while off is not None and off not in seen and 0 <= off < len(game.prg):
            ins = decode(game, off)
            if ins is None:
                break
            seen.add(off)
            fall, targets = successors(game, off, ins)
            mn, mode = ins[0], ins[1]
            if mn == "JMP" and mode == "jmp_indirect":
                computed.append((off, "jmp_indirect"))
            if mn == "RTS" and _rts_trick(game, off):
                computed.append((off, "rts_trick"))
            for t in targets:
                to = game.cpu_to_off(t, off)
                if to is None:
                    unresolved.add((off, t))
                else:
                    work.append(to)
            if ins[0] == "BRK":
                # BRK pushes the address of BRK + 2, so a handler normally resumes there (RTI)
                off += 2
                continue
            if not fall:
                break
            off += ins[2] if ins[0] != "*KIL" else 0
            if ins[0] == "*KIL":
                break
    return seen, unresolved, computed


def _rts_trick(game, off):
    """PHA ... PHA directly before the RTS within the last 8 bytes (rough pattern, listed for review)."""
    window = game.prg[max(0, off - 8):off]
    return window.count(0x48) >= 2


def cdl_runs_check(game):
    """Linear-decode each CDL code run; returns (clean runs, broken runs, instruction starts)."""
    clean, broken, starts = 0, [], set()
    for a, b in game.cdl.runs():
        off, ok, here = a, True, []
        while off < b:
            ins = decode(game, off)
            if ins is None:
                ok = False
                break
            here.append(off)
            off += ins[2] if ins[0] != "*KIL" else 1
        if ok and off == b:
            clean += 1
            starts.update(here)
        else:
            broken.append((a, b))
            starts.update(here)
    return clean, broken, starts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    ap.add_argument("--disasm", default=os.path.join(ROOT, "deps", "disassembly"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    import re
    slug = re.sub(r"[^a-z0-9]+", "_", a.game.lower()).strip("_")
    g = Game(os.path.join(a.out, slug), a.disasm, a.game)
    rep = {"game": a.game, "mapper": g.info["mapper"], "prg_size": len(g.prg), "regions": len(g.regions)}

    # seeds: reset/NMI/IRQ vectors of every vector segment, plus ld65 code labels
    seeds = set()
    for r in g.regions:
        if r["name"].startswith("VECTORS") and r["size"] >= 6:
            for k in range(3):
                v = g.prg[r["off"] + 2 * k] | (g.prg[r["off"] + 2 * k + 1] << 8)
                o = g.cpu_to_off(v, r["off"])
                if o is not None:
                    seeds.add(o)
    nlab = 0
    for name, addr, seg in g.dbg.labels():
        if name.startswith(("sub_", "loc_", "bra_", "vec_")) and seg:
            for r in g.regions:
                if r["name"].startswith(seg) and r["start"] <= addr < r["start"] + r["size"]:
                    seeds.add(r["off"] + addr - r["start"])
                    nlab += 1
                    break
    rep["seeds"] = len(seeds)
    rep["code_labels_used"] = nlab

    d_seen, unresolved, computed = descend(g, seeds)
    rep["descent_instructions"] = len(d_seen)
    rep["unresolved_targets"] = len(unresolved)
    rep["computed_sites"] = {"jmp_indirect": sum(1 for c in computed if c[1] == "jmp_indirect"),
                             "rts_trick": sum(1 for c in computed if c[1] == "rts_trick")}

    if g.cdl:
        clean, broken, c_starts = cdl_runs_check(g)
        rep["cdl_counts"] = g.cdl.counts()
        rep["cdl_runs"] = clean + len(broken)
        rep["cdl_runs_clean"] = clean
        rep["cdl_runs_broken"] = len(broken)
        rep["cdl_instructions"] = len(c_starts)
        rep["cdl_not_reached_by_descent"] = len(c_starts - d_seen)
        rep["descent_not_in_cdl"] = len(d_seen - c_starts)
        only_cdl = sorted(c_starts - d_seen)
        rep["cdl_only_samples"] = ["%s:%04X" % (g.region_of_off(o)["name"], g.off_to_cpu(o)) for o in only_cdl[:12]]
        rep["descent_only_samples"] = ["%s:%04X" % (g.region_of_off(o)["name"], g.off_to_cpu(o))
                                       for o in sorted(d_seen - c_starts)[:12]]
    if a.json:
        print(json.dumps(rep, indent=2))
    else:
        for k, v in rep.items():
            print("%-28s %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
