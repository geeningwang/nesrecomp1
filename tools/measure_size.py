#!/usr/bin/env python3
"""Compile the generated pages for the target architectures and report code size (never runs anything).

    python tools/measure_size.py <game folder name> [--gen DIR] [--opt -Os]

Targets (Apple clang cross back ends, compile only):
    thumb   : --target=thumbv7em-none-eabihf -mcpu=cortex-m7        (STM32 F7/H7)
    thumb-m4: --target=thumbv7em-none-eabihf -mcpu=cortex-m4        (STM32 F4)
    wasm32  : --target=wasm32                                       (web)
    arm64   : host (informational)
Sizes are the .text of each object (llvm-size, ELF/wasm) in bytes.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LLVM_SIZE = "/Library/Developer/CommandLineTools/usr/bin/llvm-size"

TARGETS = {
    "thumb-m7": ["--target=thumbv7em-none-eabihf", "-mcpu=cortex-m7", "-mthumb"],
    "thumb-m4": ["--target=thumbv7em-none-eabihf", "-mcpu=cortex-m4", "-mthumb"],
    "wasm32": ["--target=wasm32"],
}


def text_size(obj, target):
    r = subprocess.run([LLVM_SIZE, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    lines = r.stdout.strip().splitlines()
    if r.returncode != 0 or len(lines) < 2:
        return None
    cols = lines[1].split()
    return int(cols[0])


def wasm_size(path):
    return os.path.getsize(path)      # llvm-size does not read wasm: the object size is the closest honest figure


def measure(gen, opt, targets):
    pages = sorted(f for f in os.listdir(gen) if f.startswith("page_") and f.endswith(".c"))
    res = {t: 0 for t in targets}
    secs = {t: 0.0 for t in targets}
    with tempfile.TemporaryDirectory() as td:
        for t in targets:
            for p in pages:
                obj = os.path.join(td, p + "." + t + ".o")
                t0 = time.time()
                r = subprocess.run(["clang"] + TARGETS[t] + [opt, "-ffreestanding", "-fno-builtin", "-I", os.path.join(ROOT, "runtime", "include"),
                                    "-c", os.path.join(gen, p), "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   universal_newlines=True)
                secs[t] += time.time() - t0
                if r.returncode != 0:
                    print(r.stdout[:2000])
                    sys.exit("compile failed: %s for %s" % (p, t))
                res[t] += wasm_size(obj) if t == "wasm32" else text_size(obj, t)
    return res, secs, len(pages)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--gen", default=os.path.join(ROOT, "generated"))
    ap.add_argument("--tag", default="")
    ap.add_argument("--opt", default="-Os")
    ap.add_argument("--targets", default="thumb-m7,wasm32")
    a = ap.parse_args()
    slug = re.sub(r"[^a-z0-9]+", "_", a.game.lower()).strip("_")
    gen = os.path.join(a.gen, slug + a.tag)
    import json
    rep = json.load(open(os.path.join(gen, "codegen_report.json")))
    res, secs, n = measure(gen, a.opt, a.targets.split(","))
    ins = rep["instructions"]
    print("%s%s: %d instructions, %d pages, %s" % (a.game, a.tag, ins, n, a.opt))
    for t, v in res.items():
        print("  %-9s %9d bytes  %6.1f bytes/instruction   (compile %.1fs)" % (t, v, v / ins, secs[t]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
