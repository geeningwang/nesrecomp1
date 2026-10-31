#!/usr/bin/env python3
"""Stage C: generate C from the 6502 code of a rebuilt game (exact mode).

    python tools/codegen.py <game folder name> [--out DIR] [--gen DIR] [--eager-flags] [--no-step]

Writes <gen>/<slug>/page_<SEGMENT>.c (one function per ld65 bank segment that holds code), pages.h and
codegen_report.json.  The instruction set is CDL code runs plus recursive descent from the vectors and the ld65
code labels (see analyze.py).  Per instruction one C statement group using the macros of
runtime/include/nesrt_ops.h; a backward flag-liveness analysis chooses which flags are computed.

Control flow inside a segment uses goto; leaving the segment (or any computed target) returns the target address
to the runtime's dispatcher, which finds the generated page for the current bank mapping or falls back to the
interpreter.  See doc/plan.md section 6.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze                    # noqa: E402
from opcodes import OPCODES       # noqa: E402

ROOT = os.path.dirname(HERE)
BR = {"BCC": ("fc", 0), "BCS": ("fc", 1), "BEQ": ("fz", 1), "BNE": ("fz", 0),
      "BMI": ("fn", 1), "BPL": ("fn", 0), "BVC": ("fv", 0), "BVS": ("fv", 1)}
BR_FLAG = {"BCC": 0x01, "BCS": 0x01, "BEQ": 0x02, "BNE": 0x02, "BMI": 0x80, "BPL": 0x80, "BVC": 0x40, "BVS": 0x40}
N, V, D, I, Z, C = 0x80, 0x40, 0x08, 0x04, 0x02, 0x01
ALL = 0xCF

# mnemonic -> (flags read, flags written) ; I is treated as always live (the runtime samples it every instruction)
FLAGS = {
    "ADC": (C, N | V | Z | C), "SBC": (C, N | V | Z | C), "*SBC": (C, N | V | Z | C),
    "AND": (0, N | Z), "ORA": (0, N | Z), "EOR": (0, N | Z), "LDA": (0, N | Z), "LDX": (0, N | Z), "LDY": (0, N | Z),
    "TAX": (0, N | Z), "TAY": (0, N | Z), "TXA": (0, N | Z), "TYA": (0, N | Z), "TSX": (0, N | Z), "PLA": (0, N | Z),
    "INX": (0, N | Z), "INY": (0, N | Z), "DEX": (0, N | Z), "DEY": (0, N | Z), "INC": (0, N | Z), "DEC": (0, N | Z),
    "CMP": (0, N | Z | C), "CPX": (0, N | Z | C), "CPY": (0, N | Z | C), "BIT": (0, N | V | Z),
    "ASL": (0, N | Z | C), "LSR": (0, N | Z | C), "ROL": (C, N | Z | C), "ROR": (C, N | Z | C),
    "CLC": (0, C), "SEC": (0, C), "CLI": (0, I), "SEI": (0, I), "CLD": (0, D), "SED": (0, D), "CLV": (0, V),
    "PHP": (ALL, 0), "PLP": (0, ALL), "RTI": (0, ALL), "BRK": (ALL, I),
    "*LAX": (0, N | Z), "*SAX": (0, 0), "*DCP": (0, N | Z | C), "*ISB": (C, N | V | Z | C), "*SLO": (0, N | Z | C),
    "*RLA": (C, N | Z | C), "*SRE": (0, N | Z | C), "*RRA": (C, N | V | Z | C), "*ANC": (0, N | Z | C),
    "*ALR": (0, N | Z | C), "*ARR": (C, N | V | Z | C), "*XAA": (0, N | Z), "*ATX": (0, N | Z), "*AXS": (0, N | Z | C),
    "*LAS": (0, N | Z), "*AHX": (0, 0), "*TAS": (0, 0), "*SHY": (0, 0), "*SHX": (0, 0), "*NOP": (0, 0), "NOP": (0, 0),
}
READ_OPS = {"ADC", "SBC", "*SBC", "AND", "ORA", "EOR", "CMP", "CPX", "CPY", "BIT", "LDA", "LDX", "LDY", "*LAX",
            "*ANC", "*ALR", "*ARR", "*XAA", "*ATX", "*AXS", "*NOP"}
WRITE_OPS = {"STA", "STX", "STY", "*SAX"}
RMW_OPS = {"ASL", "LSR", "ROL", "ROR", "INC", "DEC", "*DCP", "*ISB", "*SLO", "*RLA", "*SRE", "*RRA", "*LAS"}
UNSTABLE = {"*AHX": 0, "*TAS": 1, "*SHY": 2, "*SHX": 3}
SIMPLE = {"TAX", "TAY", "TXA", "TYA", "TSX", "TXS", "INX", "INY", "DEX", "DEY", "CLC", "SEC", "CLI", "SEI", "CLD", "SED",
          "CLV", "PHA", "PHP", "PLA", "PLP", "NOP"}
ENDS = {"JMP", "RTS", "RTI", "BRK", "*KIL"}


def max_cost(ins):
    mn, mode, size, operand, op = ins
    base, extra = OPCODES[op][3], OPCODES[op][4]
    return base + extra if mn != "*KIL" else 2


def may_touch_io(ins):
    """True when the instruction can reach a PPU/APU/mapper register or ROM (needs the exact bus time)."""
    mn, mode, size, operand, op = ins
    if mode in ("absolute",):
        return not (operand < 0x2000)
    return mode in ("absolute_x", "absolute_y", "indirect_x", "indirect_y")


def hx(v, w=4):
    return "0x%0*X" % (w, v)


def ea_read(mode, op):
    if mode == "immediate": return hx(op, 2)
    if mode == "zero_page": return "RDZP(%s)" % hx(op, 2)
    if mode == "zero_page_x": return "RDZP(%s + X)" % hx(op, 2)
    if mode == "zero_page_y": return "RDZP(%s + Y)" % hx(op, 2)
    if mode == "absolute": return "RD(%s)" % hx(op)
    if mode == "absolute_x": return "nrt_rd_idx(c, %s, X)" % hx(op)
    if mode == "absolute_y": return "nrt_rd_idx(c, %s, Y)" % hx(op)
    if mode == "indirect_x": return "RD(nrt_ptr_zp(c, (uint8_t)(%s + X)))" % hx(op, 2)
    if mode == "indirect_y": return "nrt_rd_idx(c, nrt_ptr_zp(c, %s), Y)" % hx(op, 2)
    raise ValueError(mode)


def ea_addr(mode, op):
    """(statement computing _e, is_zero_page) for stores / read-modify-write."""
    if mode == "zero_page": return "uint8_t _e = %s;" % hx(op, 2), True
    if mode == "zero_page_x": return "uint8_t _e = (uint8_t)(%s + X);" % hx(op, 2), True
    if mode == "zero_page_y": return "uint8_t _e = (uint8_t)(%s + Y);" % hx(op, 2), True
    if mode == "absolute": return "uint16_t _e = %s;" % hx(op), False
    if mode == "absolute_x": return "uint16_t _e = nrt_ea_idx_w(c, %s, X);" % hx(op), False
    if mode == "absolute_y": return "uint16_t _e = nrt_ea_idx_w(c, %s, Y);" % hx(op), False
    if mode == "indirect_x": return "uint16_t _e = nrt_ptr_zp(c, (uint8_t)(%s + X));" % hx(op, 2), False
    if mode == "indirect_y": return "uint16_t _e = nrt_ea_idx_w(c, nrt_ptr_zp(c, %s), Y);" % hx(op, 2), False
    raise ValueError(mode)


class Region:
    def __init__(self, r):
        self.name, self.start, self.size, self.off = r["name"], r["start"], r["size"], r["off"]
        self.insns = {}       # offset -> (ins, cpu addr)
        self.entries = set()  # cpu addresses that must be dispatchable
        self.labels = set()   # cpu addresses that need a C label


def build(g):
    seeds, entries_by_off = set(), set()
    vec_targets = set()
    for r in g.regions:
        if r["name"].startswith("VECTORS") and r["size"] >= 6:
            for k in range(3):
                v = g.prg[r["off"] + 2 * k] | (g.prg[r["off"] + 2 * k + 1] << 8)
                o = g.cpu_to_off(v, r["off"])
                if o is not None:
                    seeds.add(o)
                    vec_targets.add(o)
    for name, addr, seg in g.dbg.labels():
        if name.startswith(("sub_", "loc_", "bra_", "vec_")) and seg:
            for r in g.regions:
                if r["name"].startswith(seg) and r["start"] <= addr < r["start"] + r["size"]:
                    seeds.add(r["off"] + addr - r["start"])
                    if not name.startswith("bra_"):
                        entries_by_off.add(r["off"] + addr - r["start"])
                    break
    insn = set()
    if g.cdl:
        clean, broken, starts = analyze.cdl_runs_check(g)
        insn |= starts
        for a, b in g.cdl.runs():
            seeds.add(a)
            entries_by_off.add(a)
        from cdl import IND_CODE
        for o in starts:
            if g.cdl.prg[o] & IND_CODE:
                entries_by_off.add(o)
    d_seen, _, _ = analyze.descend(g, seeds | insn)
    insn |= d_seen
    entries_by_off |= vec_targets
    return insn, entries_by_off


def liveness(region, g, force_all):
    """live-out flag mask per instruction offset (backward dataflow over the region's instructions)."""
    offs = sorted(region.insns)
    succ = {}
    for o in offs:
        ins, cpu = region.insns[o]
        mn, mode, size, operand, op = ins
        s, unknown = [], False
        nxt = o + (size if mn != "*KIL" else 0)
        if mn in analyze.BRANCHES:
            t = (cpu + 2 + (operand - 256 if operand >= 128 else operand)) & 0xFFFF
            to = o + 2 + (operand - 256 if operand >= 128 else operand)
            if to in region.insns: s.append(to)
            else: unknown = True
            if nxt in region.insns: s.append(nxt)
            else: unknown = True
        elif mn == "JMP" and mode == "jmp_absolute":
            to = g.cpu_to_off(operand, o)
            if to is not None and to in region.insns and region.off <= to < region.off + region.size: s.append(to)
            else: unknown = True
        elif mn in ("JMP", "RTS", "RTI", "BRK", "*KIL"):
            unknown = True
        else:
            if mn == "JSR": unknown = True       # the callee may read anything
            if nxt in region.insns: s.append(nxt)
            else: unknown = True
        succ[o] = (s, unknown)
    live_in = {o: ALL for o in offs}
    live_out = {o: ALL for o in offs}
    if force_all:
        return live_out
    changed = True
    while changed:
        changed = False
        for o in reversed(offs):
            ins = region.insns[o][0]
            use, dfn = FLAGS.get(ins[0], (0, 0))
            if ins[0] in analyze.BRANCHES:
                use |= BR_FLAG[ins[0]]
            s, unknown = succ[o]
            lo = ALL if unknown else 0
            for t in s:
                lo |= live_in[t]
            li = (lo & ~dfn) | use
            if lo != live_out[o] or li != live_in[o]:
                live_out[o], live_in[o] = lo, li
                changed = True
    return live_out


def emit_insn(g, region, o, ins, F, out, labels_of):
    mn, mode, size, operand, op = ins
    cpu = region.start + (o - region.off)
    Fs = hx(F & 0xFF, 2)
    base = OPCODES[op][3]
    p = out.append
    nxt = cpu + size

    def target(t, extra=None):
        """C for transferring control to cpu address t: goto inside the region, else return."""
        to = g.cpu_to_off(t, o)
        if to is not None and to in region.insns and region.off <= to < region.off + region.size:
            return "goto L_%04X;" % t
        return "return %s;" % hx(t)

    if mn == "*KIL":
        p("c->jammed = 1; c->pc = %s; return %s;" % (hx(cpu), hx(cpu)))
        return
    if mn in analyze.BRANCHES:
        t = (cpu + 2 + (operand - 256 if operand >= 128 else operand)) & 0xFFFF
        fld, val = BR[mn]
        extra = 1 + (1 if (nxt & 0xFF00) != (t & 0xFF00) else 0)
        p("if (c->%s == %d) { CYC(%d); %s }" % (fld, val, extra, target(t)))
        return
    if mn == "JMP":
        if mode == "jmp_absolute":
            p(target(operand))
        else:
            p("{ uint16_t _p = %s; uint16_t _lo = RD(_p); uint16_t _hi = RD((uint16_t)((_p & 0xFF00) | ((_p + 1) & 0xFF))); "
              "return (uint16_t)(_lo | (_hi << 8)); }" % hx(operand))
        return
    if mn == "JSR":
        r = (cpu + 2) & 0xFFFF
        p("PUSH(%s); PUSH(%s);" % (hx(r >> 8, 2), hx(r & 0xFF, 2)))
        p(target(operand))
        return
    if mn == "RTS":
        p("{ uint16_t _l = POP(); uint16_t _h = POP(); return (uint16_t)((_l | (_h << 8)) + 1); }")
        return
    if mn == "RTI":
        p("{ UNPACK_P(POP()); { uint16_t _l = POP(); uint16_t _h = POP(); return (uint16_t)(_l | (_h << 8)); } }")
        return
    if mn == "BRK":
        p("return nrt_brk(c, %s);" % hx((cpu + 2) & 0xFFFF))
        return

    if mn in SIMPLE:
        f = {"PHA": "PHA()", "PHP": "PHP()", "PLP": "PLP()", "TXS": "TXS()", "NOP": ""}.get(mn)
        if f is None:
            f = "%s(%s)" % (mn, Fs) if mn in ("TAX", "TAY", "TXA", "TYA", "TSX", "INX", "INY", "DEX", "DEY", "PLA") else "%s()" % mn
        if f:
            p(f + ";")
        return
    if mn in READ_OPS:
        if mn == "*NOP":
            if mode in ("immediate", "implicit"):
                return
            p("(void)%s;" % ea_read(mode, operand))
            return
        macro = {"*SBC": "SBC", "*LAX": "LAX", "*ANC": "ANC", "*ALR": "ALR", "*ARR": "ARR", "*XAA": "XAA", "*ATX": "LXA",
                 "*AXS": "AXS"}.get(mn, mn)
        p("%s(%s, %s);" % (macro, ea_read(mode, operand), Fs))
        return
    if mn in WRITE_OPS:
        val = {"STA": "A", "STX": "X", "STY": "Y", "*SAX": "(uint8_t)(A & X)"}[mn]
        if mode.startswith("zero_page"):
            e = {"zero_page": hx(operand, 2), "zero_page_x": "%s + X" % hx(operand, 2), "zero_page_y": "%s + Y" % hx(operand, 2)}[mode]
            p("WRZP(%s, %s);" % (e, val))
        else:
            decl, _ = ea_addr(mode, operand)
            p("{ %s WR(_e, %s); }" % (decl, val))
        return
    if mn in RMW_OPS:
        if mode == "accumulator":
            p("%s(A, %s);" % (mn.lstrip("*"), Fs))
            return
        decl, zp = ea_addr(mode, operand)
        macro = mn.lstrip("*")
        if zp:
            p("{ %s uint8_t _m = RDZP(_e); %s(_m, %s); WRZP(_e, _m); }" % (decl, macro, Fs))
        else:
            p("{ %s uint8_t _m = RD(_e); WR(_e, _m); %s(_m, %s); WR(_e, _m); }" % (decl, macro, Fs))
        return
    if mn in UNSTABLE:
        k = UNSTABLE[mn]
        if mode == "indirect_y":
            p("nrt_unstable_store(c, %d, nrt_ptr_zp(c, %s), Y);" % (k, hx(operand, 2)))
        else:
            idx = "X" if mode == "absolute_x" else "Y"
            p("nrt_unstable_store(c, %d, %s, %s);" % (k, hx(operand), idx))
        return
    raise ValueError("unhandled %s %s" % (mn, mode))


def generate(g, outdir, eager, hook):
    os.makedirs(outdir, exist_ok=True)
    insn, entry_offs = build(g)
    report = {"regions": {}, "instructions": 0}
    regions = []
    for r in g.regions:
        reg = Region(r)
        for o in sorted(insn):
            if r["off"] <= o < r["off"] + r["size"]:
                ins = analyze.decode(g, o)
                if ins is not None:
                    reg.insns[o] = (ins, r["start"] + o - r["off"])
        if reg.insns:
            regions.append(reg)
    for reg in regions:
        for o, (ins, cpu) in reg.insns.items():
            mn, mode, size, operand, op = ins
            if mn in analyze.BRANCHES:
                reg.labels.add((cpu + 2 + (operand - 256 if operand >= 128 else operand)) & 0xFFFF)
            elif mn == "JMP" and mode == "jmp_absolute":
                reg.labels.add(operand)
            elif mn == "JSR":
                reg.labels.add(operand)
                reg.entries.add((cpu + 2) & 0xFFFF)       # the return address: reached through RTS
                reg.labels.add((cpu + 2) & 0xFFFF)
                reg.entries.add(operand)
            if o in entry_offs:
                reg.entries.add(cpu)
                reg.labels.add(cpu)
    hdr = ['/* GENERATED by tools/codegen.py - do not edit, do not commit. */', '#include "nesrt_ops.h"', ""]
    total = 0
    for reg in regions:
        live = liveness(reg, g, eager)
        lines = ['/* GENERATED by tools/codegen.py - do not edit, do not commit. */', '#include "nesrt_ops.h"', ""]
        fname = "nrt_page_%s" % re.sub(r"\W", "_", reg.name)
        lines.append("uint16_t %s(nrt_cpu *c, uint16_t pc)" % fname)
        lines.append("{")
        lines.append("    switch (pc) {")
        for e in sorted(reg.entries):
            if any(reg.off + e - reg.start == o for o in reg.insns):
                lines.append("    case %s: goto L_%04X;" % (hx(e), e))
        lines.append("    default: c->miss = 1; return pc;")
        lines.append("    }")
        offs = sorted(reg.insns)
        # basic blocks: a block starts at the first instruction, at every label and after every flow instruction
        bstart = set()
        for i, o in enumerate(offs):
            ins, cpu = reg.insns[o]
            if i == 0 or cpu in reg.labels or offs[i - 1] + reg.insns[offs[i - 1]][0][2] != o:
                bstart.add(o)
            if ins[0] in analyze.BRANCHES or ins[0] in ("JSR",) or ins[0] in ENDS:
                nx = o + (ins[2] if ins[0] != "*KIL" else 0)
                if nx in reg.insns:
                    bstart.add(nx)
        block_of, blocks = {}, {}
        cur = None
        for o in offs:
            if o in bstart or cur is None:
                cur = o
                blocks[cur] = []
            blocks[cur].append(o)
            block_of[o] = cur
        for i, o in enumerate(offs):
            ins, cpu = reg.insns[o]
            F = live[o]
            body = []
            if cpu in reg.labels:
                lines.append("L_%04X:" % cpu)
            if hook == "insn":
                cyc = OPCODES[ins[4]][3] if ins[0] != "*KIL" else 2
                lines.append("    STEP(%s, %d);" % (hx(cpu), cyc))
            elif hook == "block":
                if o in blocks:
                    tot = sum(max_cost(reg.insns[x][0]) for x in blocks[o])
                    base = sum(OPCODES[reg.insns[x][0][4]][3] if reg.insns[x][0][0] != "*KIL" else 2 for x in blocks[o])
                    lines.append("    BSTEP(%s, %d, %d);" % (hx(cpu), tot, base))
                if may_touch_io(ins):
                    rest = 0
                    seen = False
                    for x in blocks[block_of[o]]:
                        if seen:
                            rest += OPCODES[reg.insns[x][0][4]][3]
                        if x == o:
                            seen = True
                    lines.append("    c->tadj = %d;" % rest)
            emit_insn(g, reg, o, ins, F, body, None)
            for b in body:
                lines.append("    " + b)
            # a fall-through into a byte that is not part of the instruction set leaves the page
            mn, size = ins[0], ins[2]
            if mn not in ENDS and (o + size) not in reg.insns:
                lines.append("    return %s;" % hx((cpu + size) & 0xFFFF))
        lines.append("}")
        with open(os.path.join(outdir, "page_%s.c" % reg.name), "w") as fh:
            fh.write("\n".join(lines) + "\n")
        hdr.append("uint16_t %s(nrt_cpu *c, uint16_t pc);" % fname)
        report["regions"][reg.name] = {"instructions": len(reg.insns), "entries": len(reg.entries), "labels": len(reg.labels)}
        total += len(reg.insns)
    with open(os.path.join(outdir, "pages.h"), "w") as fh:
        fh.write("\n".join(hdr) + "\n")
    report["instructions"] = total
    with open(os.path.join(outdir, "codegen_report.json"), "w") as fh:
        json.dump(report, fh, indent=2, sort_keys=True)
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    ap.add_argument("--gen", default=os.path.join(ROOT, "generated"))
    ap.add_argument("--disasm", default=os.path.join(ROOT, "deps", "disassembly"))
    ap.add_argument("--eager-flags", action="store_true", help="compute every flag (trace builds, size baseline)")
    ap.add_argument("--hook", choices=("block", "insn", "none"), default="block",
                    help="block: deadline check once per basic block, interpreter near events (default); "
                         "insn: check before every instruction (trace builds); none: size experiments")
    ap.add_argument("--tag", default="", help="suffix of the output folder")
    a = ap.parse_args()
    slug = re.sub(r"[^a-z0-9]+", "_", a.game.lower()).strip("_")
    g = analyze.Game(os.path.join(a.out, slug), a.disasm, a.game)
    rep = generate(g, os.path.join(a.gen, slug + a.tag), a.eager_flags, a.hook)
    print("%s: %d instructions in %d pages" % (a.game, rep["instructions"], len(rep["regions"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
