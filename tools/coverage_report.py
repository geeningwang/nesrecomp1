#!/usr/bin/env python3
"""Sum the coverage.txt files that nrt_native writes into its dump folders.

    python tools/coverage_report.py DIR [--min-generated 0.999]

Reports generated vs interpreted instruction counts overall and per ROM-run folder, and lists runs whose generated
share is below the threshold with their interpreted PCs ("missed"), which can be fed back to gen_insn.py --seeds.
Exit status 1 if any run is below the threshold or no coverage file was found.
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--min-generated", type=float, default=0.999)
    ap.add_argument("--write-seeds", help="write the missed PCs of all runs to FILE (input of gen_insn.py --seeds)")
    a = ap.parse_args()
    total_g = total_i = runs = 0
    low = []
    all_missed = set()
    for root, _, files in os.walk(a.dir):
        if "coverage.txt" not in files:
            continue
        g = i = 0
        missed = []
        for line in open(os.path.join(root, "coverage.txt")):
            k, _, v = line.partition(" ")
            if k == "generated":
                g = int(v)
            elif k == "interpreted":
                i = int(v)
            elif k == "missed":
                missed.append(v.strip())
        all_missed.update(missed)
        runs += 1
        total_g += g
        total_i += i
        if g + i == 0 or g / (g + i) < a.min_generated:
            low.append((root, g, i, missed))
    if not runs:
        print("no coverage.txt found under %s" % a.dir)
        return 1
    share = total_g / max(total_g + total_i, 1)
    print("%d runs: %d generated + %d interpreted instructions = %.4f%% generated" % (runs, total_g, total_i, 100 * share))
    for root, g, i, missed in low[:20]:
        print("  below threshold: %s  generated=%d interpreted=%d  missed: %s" % (root, g, i, " ".join(missed[:12])))
    if a.write_seeds:
        with open(a.write_seeds, "w") as f:
            f.write("# executed PCs that ran in the interpreter (coverage_report.py)\n")
            f.writelines("%s\n" % pc for pc in sorted(all_missed))
        print("wrote %d PCs to %s" % (len(all_missed), a.write_seeds))
    return 1 if low else 0


if __name__ == "__main__":
    sys.exit(main())
