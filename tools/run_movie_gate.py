#!/usr/bin/env python3
"""Run the movie-replay oracle gate for one game.

    python tools/run_movie_gate.py --nes1-repo DIR --rom game.nes --movie movie.fm2 \
        --work OUT [--frames 3000] [--level scanline]

Steps: FM2 -> NESINPUT, nes1-vs-FCEUX divergence search, instruction traces of selected
frames, and native FM2 playback vs. the normalized replay. The exit code is nonzero unless
every step succeeds. Results are written to OUT/gate.json.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
TRACE_FRAMES = (1, 2, 3, 4, 5, 6, 7, 8, 120, 1502)


def run(command, log):
    result = subprocess.run(command, capture_output=True, text=True)
    log.write("$ %s\n%s%s\n" % (" ".join(str(c) for c in command), result.stdout, result.stderr))
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nes1-repo", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--movie", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--frames", type=int, default=3000)
    parser.add_argument("--level", choices=("frame", "scanline"), default="scanline")
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    repo = Path(args.nes1_repo)
    nes1 = repo / "src" / "x64" / "Release" / "nes1.exe"
    fceux = repo / "tool" / "fceux-2.6.6" / "vc" / "x64" / "Release" / "fceux64.exe"
    tool = repo / "tool"
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    replay = work / "replay.in"
    frames = str(args.frames)
    selected = ",".join(str(f) for f in TRACE_FRAMES if f <= args.frames) + "," + frames
    steps = [
        ("convert", [sys.executable, HERE / "make_input.py", args.movie, "--rom", args.rom,
                     "--output", replay, "--frames", frames]),
        ("divergence", [sys.executable, tool / "find_divergence.py", "--nes1", nes1, "--fceux", fceux,
                        "--rom", args.rom, "--frames", frames, "--work", work / "div", "--level", args.level,
                        "--input", replay, "--timeout", str(args.timeout)]),
        ("traces", [sys.executable, tool / "compare_traces.py", "--nes1", nes1, "--fceux", fceux,
                    "--rom", args.rom, "--frames", frames, "--work", work / "trace", "--input", replay,
                    "--trace-frames", selected, "--timeout", str(args.timeout)]),
        ("native_vs_normalized", [sys.executable, tool / "check_movie_replay.py", "--fceux", fceux,
                                  "--rom", args.rom, "--movie", args.movie, "--input", replay,
                                  "--frames", frames, "--work", work / "native", "--timeout", str(args.timeout)]),
    ]
    results = {}
    with open(work / "gate.log", "w") as log:
        for name, command in steps:
            code, out = run(command, log)
            results[name] = {"exit": code, "last": out.strip().splitlines()[-1:] or [""]}
            print("%-22s %s" % (name, "ok" if code == 0 else "FAILED (%d)" % code))
            if code:
                break
    results["passed"] = all(v["exit"] == 0 for v in results.values()) and len(results) == len(steps)
    (work / "gate.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
