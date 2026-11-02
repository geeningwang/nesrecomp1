#!/usr/bin/env python3
"""Generate per-instruction exact-mode C code for one NROM game (or any mapper-0 .nes file).

    python tools/gen_insn.py --game "Mappy" [--out DIR] [--gen DIR] [--seeds FILE]
    python tools/gen_insn.py --rom file.nes --name slug [--gen DIR] [--seeds FILE]

Writes <gen>/<slug>/nrt_game_code.c   one function per instruction, a PRG-offset table and the nrt_code_module
                    nrt_game_data.c   the PRG/CHR images and cart descriptor, so a linked executable needs no ROM file
                    codegen_report.json

How it works. Every instruction the code map finds becomes a C function that executes exactly what the
interpreter's Cpu::step() executes for that opcode, with opcode, operands, addressing mode and cycle counts as
constants. The function bodies are not rewritten by hand: the operand fetch, the operation and the store of each
instruction are cut out of runtime/src/nrt_cpu.cpp (the interpreter) and pasted into the function, so the two
cannot drift apart; the C compiler folds the constants. The runtime dispatches by the PC it actually has, so the
code map only decides coverage: an address without a function runs in the interpreter. Correctness is checked
against FCEUX and the nestest log, not against the interpreter.

Code map = recursive descent from the vectors (and ld65 code labels, CDL runs, extra seed files when available).
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import analyze                    # noqa: E402
from opcodes import OPCODES       # noqa: E402

GENERATOR = "gen_insn.py 1"
CPU_SRC = os.path.join(ROOT, "runtime", "src", "nrt_cpu.cpp")


# ---------------------------------------------------------------- the interpreter as the source of semantics

def read_interpreter():
    return open(CPU_SRC, encoding="utf-8").read()


def parse_switch(text, start):
    """Cases of the switch whose opening brace is the first '{' at or after `start`.
    Returns {label: body text}; labels that fall through to the next share its body."""
    i = text.index("{", start)
    depth, pos = 0, i
    cases, labels, body_start = {}, [], None
    pending = []
    j = i
    token = re.compile(r"\bcase\s+(\w+)\s*:|\bdefault\s*:|[{}]")
    for m in token.finditer(text, i):
        t = m.group(0)
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
            if depth == 0:
                if pending:
                    body = text[body_start:m.start()]
                    for lab in pending:
                        cases[lab] = body
                return cases
        elif depth == 1:
            label = m.group(1) if m.group(1) else "default"
            if pending and text[body_start:m.start()].strip() == "":
                pending.append(label)           # grouped labels
            else:
                if pending:
                    body = text[body_start:m.start()]
                    for lab in pending:
                        cases[lab] = body
                pending = [label]
            body_start = m.end()
    raise ValueError("unterminated switch")


class Interpreter:
    def __init__(self):
        s = read_interpreter()
        self.src = s
        self.defs = s[s.index("#define UNK 0"):s.index("struct opcode")]
        self.branch_defs = "\n".join(l for l in s.splitlines() if l.startswith("#define BRANCH_"))
        self.macros = self._macros(s)
        table_text = s[s.index("opcode_items[256]"):]
        table_text = table_text[:table_text.index("\n};")]
        self.table = [(m[0], int(m[1]), m[2], int(m[3]), int(m[4])) for m in
                      re.findall(r"\{ (\w+), (\d+), (\w+), (\d+), (\d+) \}", table_text)]
        assert len(self.table) == 256, len(self.table)
        step = s[s.index("bool Cpu::step()"):]
        # operand fetch
        p0 = step.index("\tswitch (addressing_mode)\n\t{\n\tcase ADDR_NONE:")
        self.fetch = parse_switch(step, p0)
        # statements between the end of the fetch switch and the operation switch
        o0 = step.index("\tswitch (instruction)\n\t{\n\tcase ADC:")
        self.ops = parse_switch(step, o0)
        fetch_end = self._switch_end(step, p0)
        self.between = step[fetch_end:step.index("\t// Operation")]
        s0 = step.index("\tif (store)\n\t{\n\t\tswitch (addressing_mode)")
        self.store = parse_switch(step, step.index("switch (addressing_mode)", s0))
        # statements after the store block: bus fixups and the cycle count
        tail0 = step.index("\t// What is left on the data bus after the instruction")
        tail1 = step.index("\tpending_instruction_cycles = 0;", tail0) + len("\tpending_instruction_cycles = 0;")
        self.tail = step[tail0:tail1]
        # declarations of the flags computed from the instruction
        d0 = step.index("\tbool store_only =")
        d1 = step.index("\tunsigned char rmw_orig = 0;")
        flags = step[d0:d1]
        so = flags[:flags.index("const bool rmw")]
        rm = flags[flags.index("const bool rmw"):flags.index("const bool write_type")]
        self.store_only = set(re.findall(r"instruction == (\w+)", so))
        self.rmw = set(re.findall(r"instruction == (\w+)", rm))
        assert "!= ADDR_ACCUMULATOR" in rm and "!store_only" in rm

    @staticmethod
    def _macros(s):
        a = s.index("#define VAR_ZERO_PAGE(operand)")
        b = s.index("#define PAGE_CROSSED")
        b = s.index("\n", b)
        return s[a:b]

    @staticmethod
    def _switch_end(text, start):
        i = text.index("{", start)
        depth = 0
        for k in range(i, len(text)):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    return k + 1
        raise ValueError


def strip(text):
    """Comments and blank lines out: the pasted interpreter text is commented there, not in 5000 copies."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    lines = [l.rstrip() for l in text.splitlines()]
    return "\n".join(l for l in lines if l.strip()) + "\n"


def case_text(cases, label):
    body = cases.get(label)
    if body is None:
        return ""
    return "\tdo {\n" + strip(body) + "\t} while (0);\n"


# ---------------------------------------------------------------- code map

class PlainRom:
    """Mapper-0 ROM without assembler metadata: PRG offset = (CPU address - $8000) mod PRG size."""

    def __init__(self, path):
        data = open(path, "rb").read()
        assert data[:4] == b"NES\x1a", "not an iNES file"
        self.header = data[:16]
        n = data[4] * 16384
        self.prg = data[16:16 + n]
        self.chr = data[16 + n:16 + n + data[5] * 8192]
        self.regions = []
        self.cdl = None

    def off_to_cpu(self, off):
        return 0x8000 + off if len(self.prg) == 32768 else 0xC000 + off

    def cpu_to_off(self, addr, near_off):
        if addr < 0x8000:
            return None
        return (addr - 0x8000) & (len(self.prg) - 1)

    def vector_seeds(self):
        seeds = set()
        for k in range(3):
            a = len(self.prg) - 6 + 2 * k
            v = self.prg[a] | (self.prg[a + 1] << 8)
            o = self.cpu_to_off(v, 0)
            if o is not None:
                seeds.add(o)
        return seeds


def load_game(args):
    """(game object, seeds, name, header, chr)"""
    if args.rom:
        g = PlainRom(args.rom)
        return g, g.vector_seeds(), args.name or os.path.splitext(os.path.basename(args.rom))[0], g.header, g.chr
    slug = re.sub(r"[^a-z0-9]+", "_", args.game.lower()).strip("_")
    g = analyze.Game(os.path.join(args.out, slug), args.disasm, args.game)
    rom = open(os.path.join(args.out, slug, "game.nes"), "rb").read()
    seeds = set()
    for r in g.regions:
        if r["name"].startswith("VECTORS") and r["size"] >= 6:
            for k in range(3):
                v = g.prg[r["off"] + 2 * k] | (g.prg[r["off"] + 2 * k + 1] << 8)
                o = g.cpu_to_off(v, r["off"])
                if o is not None:
                    seeds.add(o)
    for name, addr, seg in g.dbg.labels():
        if name.startswith(("sub_", "loc_", "bra_", "vec_")) and seg:
            for r in g.regions:
                if r["name"].startswith(seg) and r["start"] <= addr < r["start"] + r["size"]:
                    seeds.add(r["off"] + addr - r["start"])
                    break
    n = len(g.prg)
    return g, seeds, slug, rom[:16], rom[16 + n:16 + n + 8192]


def code_map(game, seeds, extra_pcs, use_cdl=True):
    starts, unresolved, computed = analyze.descend(game, seeds)
    sources = {"descent": len(starts)}
    if use_cdl and game.cdl:
        _, _, cdl_starts = analyze.cdl_runs_check(game)
        sources["cdl_only"] = len(cdl_starts - starts)
        starts |= cdl_starts
    if extra_pcs:
        extra = set()
        for pc in extra_pcs:
            o = game.cpu_to_off(pc, 0) if not game.regions else _cpu_to_off_any(game, pc)
            if o is not None:
                extra.add(o)
        more, _, _ = analyze.descend(game, extra)
        sources["executed_pcs_only"] = len(more - starts)
        starts |= more
    return starts, unresolved, computed, sources


def _cpu_to_off_any(game, addr):
    hits = [r for r in game.regions if r["start"] <= addr < r["start"] + r["size"]]
    return hits[0]["off"] + addr - hits[0]["start"] if len(hits) == 1 else None


# ---------------------------------------------------------------- C generation

def fnv_rom_hash(prg, chr_):
    h = 14695981039346656037
    for v in (len(prg), len(chr_)):
        for b in range(4):
            h = ((h ^ ((v >> (8 * b)) & 0xFF)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for byte in prg + chr_:
        h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def gen_function(it, prg, off, sym):
    op = prg[off]
    name, size, mode, cycles, extra = it.table[op]
    if off + (size if name != "SKIL" else 1) > len(prg):
        return None
    last = prg[off + size - 1] if size > 1 else 0
    op8 = prg[off + 1] if size >= 2 else 0
    op16 = (prg[off + 1] | (prg[off + 2] << 8)) if size >= 3 else 0
    apu_cycles = 2 if name == "SKIL" else cycles
    out = []
    out.append("static int %s_i_%04X(nrt_cpu_regs *c)\n{\n" % (sym, off))
    out.append("\tconst int instruction = %s;\n\tconst int instruction_size = %d;\n\tconst int addressing_mode = %s;\n"
               % (name, size, mode))
    out.append("\tint instruction_cycles = %d;\n\tconst int k_base_cycles = %d;\n\tconst int page_crossed_extra_cycles = %d;\n"
               % (cycles, cycles, extra))
    out.append("\tconst unsigned char k_op8 = 0x%02X;\n\tconst unsigned short k_op16 = 0x%04X;\n" % (op8, op16))
    out.append("\tunsigned char fetched_bus = nrt_x_begin(c, 0x%02X, %d);\n" % (op, apu_cycles))
    out.append("\tdata_bus = instruction_size > 1 ? 0x%02X : fetched_bus;\n" % last)
    out.append("\tunsigned char temp = 0;\n\tunsigned short temp_rpc = 0;\n\tunsigned char op8 = 0;\n\tunsigned short op16 = 0;\n")
    store_only = name in it.store_only
    rmw = (not store_only) and name in it.rmw and mode != "ADDR_ACCUMULATOR"
    out.append("\tconst int store_only = %d;\n\tconst int rmw = %d;\n\tconst int write_type = %d;\n"
               % (store_only, rmw, store_only or rmw))
    out.append("\tunsigned char rmw_orig = 0;\n\tint store = 0;\n\tunsigned char temp_out = 0;\n")
    out.append("\tpending_instruction_cycles = (unsigned char)instruction_cycles;\n")
    out.append(case_text(it.fetch, mode))
    out.append(strip(it.between))
    out.append(case_text(it.ops, name))
    out.append("\tif (store) {\n" + case_text(it.store, mode) + "\t}\n" if mode in it.store else "")
    tail = strip(it.tail.replace("opcode_items[opcode].cycles", "k_base_cycles"))
    out.append(tail + "\treturn 1;\n}\n")
    text = "".join(out)
    text = text.replace("OPERAND8", "k_op8").replace("OPERAND16", "k_op16")
    text = text.replace("bool ", "int ").replace("= true", "= 1").replace("= false", "= 0")
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--game")
    src.add_argument("--rom")
    ap.add_argument("--name")
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    ap.add_argument("--disasm", default=os.path.join(ROOT, "deps", "disassembly"))
    ap.add_argument("--gen", default=os.path.join(ROOT, "generated"))
    ap.add_argument("--seeds", help="text file of hex CPU addresses of instructions known to execute (coverage feedback)")
    ap.add_argument("--no-cdl", action="store_true", help="ignore the CDL file (descent only)")
    args = ap.parse_args()

    game, seeds, slug, header, chr_ = load_game(args)
    prg = game.prg
    if len(prg) not in (16384, 32768) or len(chr_) != 8192:
        sys.exit("only NROM images (16/32 KB PRG, 8 KB CHR) are supported")
    seeds_file = args.seeds
    if not seeds_file:
        default = os.path.join(ROOT, "games", slug, "executed_pcs.txt")
        seeds_file = default if os.path.exists(default) else None
    extra = []
    if seeds_file:
        extra = [int(l.split()[0], 16) for l in open(seeds_file) if l.strip() and not l.startswith("#")]
    starts, unresolved, computed, sources = code_map(game, seeds, extra, not args.no_cdl)

    it = Interpreter()
    sym = "nrt_" + re.sub(r"[^a-z0-9]+", "_", slug.lower()).strip("_")
    funcs, skipped = {}, []
    for off in sorted(starts):
        text = gen_function(it, prg, off, sym)
        if text is None:
            skipped.append(off)
        else:
            funcs[off] = text

    rom_hash = fnv_rom_hash(prg, chr_)
    vertical = header[6] & 1
    outdir = os.path.join(args.gen, slug)
    os.makedirs(outdir, exist_ok=True)

    code = ["/* Generated by %s from a %d-byte PRG (rom hash %016X). Do not edit. */\n" % (GENERATOR, len(prg), rom_hash),
            "#include <stdbool.h>\n#include \"nrt_exec.h\"\n\n"
            "/* instruction ids and addressing modes, taken from the interpreter */\n", it.defs, "\n", it.branch_defs, "\n\n",
            "/* register names and bus helpers: see nrt_gen.h */\n#include \"nrt_gen.h\"\n\n",
            "/* the interpreter's operand and flag macros */\n", it.macros, "\n\n"]
    for off in sorted(funcs):
        code.append(funcs[off] + "\n")
    if len(funcs) > 65535:
        sys.exit("more than 65535 instruction functions: the slot table needs a wider index")
    order = sorted(funcs)
    code.append("static const nrt_insn_fn %s_fns[%d] = {\n" % (sym, max(len(order), 1)))
    code.extend("\t%s_i_%04X,\n" % (sym, off) for off in order)
    code.append("%s};\n\nstatic const uint16_t %s_slot[%d] = {\n" % ("\t0\n" if not order else "", sym, len(prg)))
    code.extend("\t[0x%04X] = %d,\n" % (off, i + 1) for i, off in enumerate(order))
    code.append("};\n\nconst nrt_code_module %s_module = { NRT_CODE_ABI, %d, 0x%016XULL, %s_slot, %s_fns, %d, \"%s\" };\n"
                % (sym, len(prg), rom_hash, sym, sym, len(funcs), GENERATOR))
    open(os.path.join(outdir, "nrt_game_code.c"), "w", newline="\n").write("".join(code))

    def arr(name, data):
        rows = []
        for i in range(0, len(data), 16):
            rows.append("\t" + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        return "static const uint8_t %s[%d] = {\n%s\n};\n" % (name, len(data), "\n".join(rows))
    data_c = ["/* Generated by %s: the game's PRG and CHR images. Local build artefact, never committed. */\n" % GENERATOR,
              "#include \"nrt.h\"\n\n", arr(sym + "_prg", prg), "\n", arr(sym + "_chr", chr_), "\n",
              "const nrt_cart %s_cart = { %s_prg, %d, %s_chr, %d, %d };\n" % (sym, sym, len(prg), sym, len(chr_), vertical)]
    open(os.path.join(outdir, "nrt_game_data.c"), "w", newline="\n").write("".join(data_c))

    by_name = {}
    for off in funcs:
        n = it.table[prg[off]][0]
        by_name[n] = by_name.get(n, 0) + 1
    report = {"game": slug, "symbol": sym, "generator": GENERATOR, "prg_size": len(prg), "rom_hash": "%016X" % rom_hash,
              "functions": len(funcs), "skipped_end_of_prg": len(skipped), "code_map_sources": sources,
              "unresolved_targets": len(unresolved),
              "computed_sites": {"jmp_indirect": sum(1 for x in computed if x[1] == "jmp_indirect"),
                                 "rts_trick": sum(1 for x in computed if x[1] == "rts_trick")},
              "by_mnemonic": dict(sorted(by_name.items()))}
    json.dump(report, open(os.path.join(outdir, "codegen_report.json"), "w"), indent=2)
    print("%s: %d instruction functions (%s), %d skipped -> %s" % (slug, len(funcs), sources, len(skipped), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
