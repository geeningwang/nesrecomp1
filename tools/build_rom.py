#!/usr/bin/env python3
"""Stage A: rebuild a game's ROM from its disassembly and keep the debug info.

    python tools/build_rom.py <game folder name | --all> [--disasm DIR] [--out DIR]
                              [--ca65 EXE] [--ld65 EXE] [--keep-work]

For each game this repeats what the disassembly's own assemble.bat / assemble.sh do (strip the
disassembler prefix with the preparations.lua regex, run ca65 per bank, link with ld65, concatenate
header + PRG + CHR), but in plain Python so neither Lua nor a shell is needed, and with
`ca65 -g` / `ld65 --dbgfile` always on so that labels and segment spans are available for the analysis.

Output per game in <out>/<slug>/:
    game.nes        the rebuilt ROM
    game.dbg        ld65 debug file (symbols, segments, spans, lines)
    game.map        ld65 map file
    rom_info.json   iNES header fields, sizes, hashes, expected hashes, check results

Exit status: 0 = every selected game rebuilt and its size matched; 1 = a build or size problem;
a hash mismatch is reported but only counts as a failure for games whose dump is a verified one
(listed in rom_info.json as "hash_required").
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Same text-strip as preparations.lua: ".%s.%s.%s.%s.%s.%s0x......%s..:....:%s..%s..%s..." -> 3 spaces.
# Lua's "." matches any character, including a newline, so DOTALL keeps the behaviour identical.
BZK_REGEX = re.compile(r".\s.\s.\s.\s.\s.\s0x......\s..:....:\s..\s..\s...", re.DOTALL)

# README "Games list" order is by folder name; entries whose dump is not a verified [!] one.
HASH_NOT_REQUIRED = {"Double Dragon II", "Street Fighter 3"}
# Verified: the game's own assemble.bat (run with its shipped lua/ca65/ld65 on Windows) yields the same
# bytes as this script, so the SHA-1 recorded in assemble.sh does not match the upstream sources themselves.
UPSTREAM_MISMATCH = {"Nuts & Milk"}


def slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def parse_assemble_sh(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    info = {}
    for key in ("NES_OUTPUT_SIMPLE_NAME", "NES_OUTPUT_FILE_SIZE", "NES_OUTPUT_FILE_SHA1_ORIGINAL"):
        m = re.search(r"^%s=\"?([^\"\r\n]+)\"?" % key, text, re.M)
        info[key] = m.group(1).strip() if m else None
    m = re.search(r"^NES_IGNORE_COMPILE_ASM_ARRAY=\(([^)]*)\)", text, re.M)
    info["ignore"] = set(m.group(1).split()) if m else set()
    return info


def parse_lua_files(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"local\s+files_list\s*=\s*\{(.*?)\}", text, re.S)
    return re.findall(r"\"([^\"]+)\"", m.group(1)) if m else []


def parse_lua_latin(path):
    """(enabled, {char: replacement}) from preparations.lua (replace_with_latin / latin_symbols)."""
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"local\s+replace_with_latin\s*=\s*(true|false)", text)
    enabled = bool(m and m.group(1) == "true")
    table = {}
    t = re.search(r"local\s+latin_symbols\s*=\s*\{(.*?)\n\}", text, re.S)
    if t:
        for k, v in re.findall(r"\[\"([^\"]+)\"\]\s*=\s*\"([^\"]*)\"", t.group(1)):
            table[k] = v
    return enabled, table


def parse_bat_concat(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"^\s*copy\s+/B\s+(.+?)\s+%file_name%\.nes", text, re.M | re.I)
    return [t.strip() for t in m.group(1).split("+")] if m else ["header.bin", "PRG_ROM.bin", "CHR_ROM.chr"]


def find_tool(name, explicit, game_dir):
    if explicit:
        return explicit
    exe = shutil.which(name)
    if exe:
        return exe
    local = os.path.join(game_dir, name + ".exe")      # the disassembly ships Windows builds
    if os.name == "nt" and os.path.exists(local):
        return local
    sys.exit("ERROR: %s not found (install cc65, or pass --%s)" % (name, name))


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return r.returncode, r.stdout


def header_info(rom):
    h = rom[:16]
    prg, chr_ = h[4] * 16384, h[5] * 8192
    flags6, flags7 = h[6], h[7]
    return {
        "magic_ok": h[:4] == b"NES\x1a",
        "prg_size": prg,
        "chr_size": chr_,
        "mapper": (flags6 >> 4) | (flags7 & 0xF0),
        "mirroring": "four_screen" if flags6 & 8 else ("vertical" if flags6 & 1 else "horizontal"),
        "battery": bool(flags6 & 2),
        "trainer": bool(flags6 & 4),
        "flags6": flags6,
        "flags7": flags7,
    }


def readme_crcs(disasm):
    """{folder-ish game title: crc32} from the README 'Disassembled' list, in file order."""
    out = []
    try:
        text = open(os.path.join(disasm, "README.md"), encoding="utf-8").read()
    except OSError:
        return out
    section = text.split("### Disassembled")[1].split("### WIP")[0] if "### Disassembled" in text else ""
    for m in re.finditer(r"^\*\s+(.+?)\s+`([0-9A-Fa-f]{8})`\s*$", section, re.M):
        out.append((m.group(1), m.group(2).upper()))
    return out


def build_game(disasm, game, out_root, ca65x, ld65x, keep_work):
    gdir = os.path.join(disasm, game)
    sh = parse_assemble_sh(os.path.join(gdir, "assemble.sh"))
    files = parse_lua_files(os.path.join(gdir, "preparations.lua"))
    concat = parse_bat_concat(os.path.join(gdir, "assemble.bat"))
    latin_on, latin = parse_lua_latin(os.path.join(gdir, "preparations.lua"))
    work = os.path.join(out_root, slug(game), "work")
    outdir = os.path.join(out_root, slug(game))
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    for e in os.listdir(gdir):                       # everything except the tool binaries and _misc
        if e == "_misc" or e.lower().endswith((".exe", ".dll")):
            continue
        src = os.path.join(gdir, e)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(work, e))
        else:
            shutil.copy2(src, os.path.join(work, e))

    # preparations.lua: write copy_<file> with the disassembler prefix removed
    for f in files:
        with open(os.path.join(work, f), "r", encoding="utf-8", errors="surrogateescape", newline="") as fh:
            text = fh.read()
        text = BZK_REGEX.sub("   ", text)
        if latin_on:
            for k, v in latin.items():
                text = text.replace(k, v)
        with open(os.path.join(work, "copy_" + f), "w", encoding="utf-8", errors="surrogateescape", newline="") as fh:
            fh.write(text)

    ca65 = find_tool("ca65", ca65x, gdir)
    ld65 = find_tool("ld65", ld65x, gdir)
    ignore = sh["ignore"]
    objs = []
    log = []
    for f in sorted(os.listdir(work)):
        if not (f.startswith("copy_bank_") and f.endswith(".asm")) or f in ignore:
            continue
        rc, out = run([ca65, "-U", "-g", f], work)
        log.append("ca65 %s -> %d\n%s" % (f, rc, out))
        if rc != 0:
            return {"game": game, "ok": False, "error": "ca65 failed on %s" % f, "log": "\n".join(log)}
        objs.append(f[:-4] + ".o")
    dbg = os.path.join(work, "game.dbg")
    mapf = os.path.join(work, "game.map")
    rc, out = run([ld65, "-C", "ld65.cfg", "-o", "PRG_ROM.bin", "--dbgfile", "game.dbg", "-m", "game.map"] + objs, work)
    log.append("ld65 -> %d\n%s" % (rc, out))
    if rc != 0:
        return {"game": game, "ok": False, "error": "ld65 failed", "log": "\n".join(log)}

    split = os.path.join(work, "split.lua")          # Double Dragon II: PRG_ROM.bin also holds static-screen CHR
    if os.path.exists(split):
        text = open(split, encoding="utf-8", errors="replace").read()
        size_prg = int(re.search(r"size_prg\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text).group(1), 0)
        size_chr = int(re.search(r"size_chr\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text).group(1), 0)
        name_chr = re.search(r"name_chr\s*=\s*\"([^\"]+)\"", text).group(1)
        blob = open(os.path.join(work, "PRG_ROM.bin"), "rb").read()
        open(os.path.join(work, "PRG_ROM.bin"), "wb").write(blob[:size_prg])
        open(os.path.join(work, name_chr), "wb").write(blob[size_prg:size_prg + size_chr])

    rom = b""
    used = []
    for part in concat:
        p = os.path.join(work, part)
        if os.path.exists(p):
            rom += open(p, "rb").read()
            used.append(part)

    shutil.copy2(dbg, os.path.join(outdir, "game.dbg"))
    shutil.copy2(mapf, os.path.join(outdir, "game.map"))
    nes_path = os.path.join(outdir, "game.nes")
    with open(nes_path, "wb") as fh:
        fh.write(rom)

    info = header_info(rom)
    body = rom[16:]
    sha1 = hashlib.sha1(rom).hexdigest().upper()
    expect_size = int(sh["NES_OUTPUT_FILE_SIZE"]) if sh["NES_OUTPUT_FILE_SIZE"] else None
    info.update({
        "game": game,
        "slug": slug(game),
        "ok": True,
        "parts": used,
        "size": len(rom),
        "expected_size": expect_size,
        "size_ok": expect_size is None or expect_size == len(rom),
        "sha1": sha1,
        "expected_sha1": (sh["NES_OUTPUT_FILE_SHA1_ORIGINAL"] or "").upper() or None,
        "sha1_ok": (sh["NES_OUTPUT_FILE_SHA1_ORIGINAL"] or "").upper() == sha1,
        "crc32_file": "%08X" % (zlib.crc32(rom) & 0xFFFFFFFF),
        "crc32_nohdr": "%08X" % (zlib.crc32(body) & 0xFFFFFFFF),
        "hash_required": game not in HASH_NOT_REQUIRED and game not in UPSTREAM_MISMATCH,
        "upstream_mismatch": game in UPSTREAM_MISMATCH,
        "ignored_files": sorted(ignore),
        "bank_files": objs,
    })
    with open(os.path.join(outdir, "rom_info.json"), "w") as fh:
        json.dump(info, fh, indent=2, sort_keys=True)
    with open(os.path.join(outdir, "build.log"), "w") as fh:
        fh.write("\n".join(log))
    if not keep_work:
        shutil.rmtree(work)
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", nargs="?", help="folder name inside the disassembly repo")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--disasm", default=os.path.join(ROOT, "deps", "disassembly"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    ap.add_argument("--ca65")
    ap.add_argument("--ld65")
    ap.add_argument("--keep-work", action="store_true")
    a = ap.parse_args()

    if a.all:
        games = sorted(e for e in os.listdir(a.disasm)
                       if os.path.isfile(os.path.join(a.disasm, e, "assemble.sh")))
    elif a.game:
        games = [a.game]
    else:
        ap.error("give a game folder name or --all")

    crcs = [c for _, c in readme_crcs(a.disasm)]
    results, bad = [], 0
    for g in games:
        r = build_game(a.disasm, g, a.out, a.ca65, a.ld65, a.keep_work)
        results.append(r)
        if not r.get("ok"):
            print("%-40s BUILD FAILED: %s" % (g, r.get("error")))
            print(r.get("log", ""))
            bad += 1
            continue
        crc_in_readme = r["crc32_file"] in crcs or r["crc32_nohdr"] in crcs
        flag = "ok" if (r["size_ok"] and r["sha1_ok"]) else "MISMATCH"
        print("%-40s mapper %-3d PRG %6d CHR %6d size %-6s sha1 %-5s readme-crc %-5s %s" % (
            g, r["mapper"], r["prg_size"], r["chr_size"], r["size_ok"], r["sha1_ok"], crc_in_readme, flag))
        if not r["size_ok"] or (r["hash_required"] and not r["sha1_ok"]):
            bad += 1
    with open(os.path.join(a.out, "rom_summary.json"), "w") as fh:
        json.dump(results, fh, indent=2, sort_keys=True)
    print("%d game(s), %d problem(s)" % (len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
