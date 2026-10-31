#!/usr/bin/env python3
"""Spike S2: generated code size of every game (Thumb Cortex-M7 and wasm32), written to doc/size_survey.md-ready JSON.

    python tools/size_survey.py [--out DIR] [--gen DIR] [--json FILE]
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze, codegen, measure_size   # noqa: E402

ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    ap.add_argument("--gen", default=os.path.join(ROOT, "generated"))
    ap.add_argument("--disasm", default=os.path.join(ROOT, "deps", "disassembly"))
    ap.add_argument("--json", default=os.path.join(ROOT, "out", "size_survey.json"))
    a = ap.parse_args()
    games = sorted(e for e in os.listdir(a.disasm) if os.path.isfile(os.path.join(a.disasm, e, "assemble.sh")))
    rows = []
    for game in games:
        slug = re.sub(r"[^a-z0-9]+", "_", game.lower()).strip("_")
        g = analyze.Game(os.path.join(a.out, slug), a.disasm, game)
        gdir = os.path.join(a.gen, slug)
        rep = codegen.generate(g, gdir, False, "block")
        res, secs, n = measure_size.measure(gdir, "-Os", ["thumb-m7", "wasm32"])
        row = {"game": game, "mapper": g.info["mapper"], "prg": len(g.prg), "chr": g.info["chr_size"],
               "instructions": rep["instructions"], "pages": n, "thumb_m7": res["thumb-m7"], "wasm32": res["wasm32"]}
        rows.append(row)
        print("%-36s m%-2d prg %6d  insn %6d  thumb %8d  wasm %8d" % (
            game, row["mapper"], row["prg"], row["instructions"], row["thumb_m7"], row["wasm32"]), flush=True)
    json.dump(rows, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
