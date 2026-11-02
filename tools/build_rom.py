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

Exit status: 0 = every selected game rebuilt and passed its size and identity checks;
1 = a build, size or identity problem. The documented Nuts & Milk source mismatch is
accepted only for its exact known SHA-1, not for arbitrary changed game bytes.
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

# Verified: the game's own assemble.bat (run with its shipped lua/ca65/ld65 on Windows) yields the same
# bytes as this script, so the SHA-1 recorded in assemble.sh does not match the upstream sources themselves.
UPSTREAM_MISMATCH = {
    "Nuts & Milk": (
        "CCA97FBA72CA56809DD216618F5962EB6EA2F1AA",
        "8E10AA08B65F3A5FACD2368C40DCCF3AAB61341B",
    ),
}
README_TITLES = {
    "Castlevania 3": "Castlevania III - Dracula's Curse",
    "Double Dragon II": "Double Dragon II - The Revenge",
    "Solstice": "Solstice - The Quest for the Staff of Demnos",
    "Street Fighter 3": "Street Fighter III",
}


def read_text(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def require_match(pattern, text, path, field, flags=0):
    match = re.search(pattern, text, flags)
    if match is None:
        raise ValueError("%s: missing or malformed %s" % (path, field))
    return match


def slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def parse_assemble_sh(path):
    text = read_text(path)
    info = {}
    for key in ("NES_OUTPUT_SIMPLE_NAME", "NES_OUTPUT_FILE_SIZE", "NES_OUTPUT_FILE_SHA1_ORIGINAL"):
        m = require_match(r'^%s="?([^"\r\n]+)"?' % key, text, path, key, re.M)
        info[key] = m.group(1).strip()
    if not info["NES_OUTPUT_FILE_SIZE"].isdigit():
        raise ValueError("%s: invalid NES_OUTPUT_FILE_SIZE" % path)
    if not re.fullmatch(r"[0-9a-fA-F]{40}", info["NES_OUTPUT_FILE_SHA1_ORIGINAL"]):
        raise ValueError("%s: invalid NES_OUTPUT_FILE_SHA1_ORIGINAL" % path)
    m = re.search(r"^NES_IGNORE_COMPILE_ASM_ARRAY=\(([^)]*)\)", text, re.M)
    info["ignore"] = set(m.group(1).split()) if m else set()
    return info


def parse_lua_files(path):
    text = read_text(path)
    m = require_match(r"local\s+files_list\s*=\s*\{(.*?)\}", text, path, "files_list", re.S)
    files = re.findall(r"\"([^\"]+)\"", m.group(1))
    if not files:
        raise ValueError("%s: empty files_list" % path)
    return files


def parse_lua_latin(path):
    """(enabled, {char: replacement}) from preparations.lua (replace_with_latin / latin_symbols)."""
    text = read_text(path)
    m = require_match(r"local\s+replace_with_latin\s*=\s*(true|false)", text, path, "replace_with_latin")
    enabled = m.group(1) == "true"
    table = {}
    t = re.search(r"local\s+latin_symbols\s*=\s*\{(.*?)\n\}", text, re.S)
    if t:
        for k, v in re.findall(r"\[\"([^\"]+)\"\]\s*=\s*\"([^\"]*)\"", t.group(1)):
            table[k] = v
    if enabled and not table:
        raise ValueError("%s: Latin replacement enabled without a symbol table" % path)
    return enabled, table


def parse_bat_concat(path):
    text = read_text(path)
    m = require_match(r"^\s*copy\s+/B\s+(.+?)\s+%file_name%\.nes", text, path, "ROM concatenation", re.M | re.I)
    parts = [t.strip() for t in m.group(1).split("+")]
    if any(not part for part in parts):
        raise ValueError("%s: empty ROM concatenation part" % path)
    return parts


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
    if len(rom) < 16 or rom[:4] != b"NES\x1a":
        raise ValueError("invalid or truncated iNES header")
    if rom[7] & 0x0C == 0x08:
        raise ValueError("NES 2.0 headers are not supported by this builder")
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
    text = read_text(os.path.join(disasm, "README.md"))
    section = text.split("### Disassembled")[1].split("### WIP")[0] if "### Disassembled" in text else ""
    for m in re.finditer(r"^\*\s+(.+?)\s+`([0-9A-Fa-f]{8})`\s*$", section, re.M):
        out.append((m.group(1), m.group(2).upper()))
    return out


def title_key(title):
    title = re.split(r"\s+\(", title, maxsplit=1)[0]
    title = re.sub(r", The$", "", title)
    title = re.sub(r"^The ", "", title)
    return slug(title)


def expected_crc(disasm, game):
    title = README_TITLES.get(game, game)
    matches = [crc for name, crc in readme_crcs(disasm) if title_key(name) == title_key(title)]
    if len(matches) != 1:
        raise ValueError("%s: expected one matching README CRC, got %d" % (game, len(matches)))
    return matches[0]


def build_game(disasm, game, out_root, ca65x, ld65x, keep_work):
    gdir = os.path.join(disasm, game)
    sh = parse_assemble_sh(os.path.join(gdir, "assemble.sh"))
    files = parse_lua_files(os.path.join(gdir, "preparations.lua"))
    concat = parse_bat_concat(os.path.join(gdir, "assemble.bat"))
    latin_on, latin = parse_lua_latin(os.path.join(gdir, "preparations.lua"))
    crc_expected = expected_crc(disasm, game)
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
    if not objs:
        raise ValueError("%s: no assembler compilation units selected" % game)
    dbg = os.path.join(work, "game.dbg")
    mapf = os.path.join(work, "game.map")
    rc, out = run([ld65, "-C", "ld65.cfg", "-o", "PRG_ROM.bin", "--dbgfile", "game.dbg", "-m", "game.map"] + objs, work)
    log.append("ld65 -> %d\n%s" % (rc, out))
    if rc != 0:
        return {"game": game, "ok": False, "error": "ld65 failed", "log": "\n".join(log)}

    split = os.path.join(work, "split.lua")          # Double Dragon II: PRG_ROM.bin also holds static-screen CHR
    if os.path.exists(split):
        text = read_text(split)
        size_prg = int(require_match(r"size_prg\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text, split, "size_prg").group(1), 0)
        size_chr = int(require_match(r"size_chr\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text, split, "size_chr").group(1), 0)
        name_chr = require_match(r'name_chr\s*=\s*"([^"]+)"', text, split, "name_chr").group(1)
        with open(os.path.join(work, "PRG_ROM.bin"), "rb") as fh:
            blob = fh.read()
        if len(blob) != size_prg + size_chr:
            raise ValueError("%s: linked output does not match declared PRG/CHR split sizes" % game)
        with open(os.path.join(work, "PRG_ROM.bin"), "wb") as fh:
            fh.write(blob[:size_prg])
        with open(os.path.join(work, name_chr), "wb") as fh:
            fh.write(blob[size_prg:])

    rom = b""
    used = []
    with open(os.path.join(work, "header.bin"), "rb") as fh:
        declared = header_info(fh.read())
    declared_size = 16 + declared["prg_size"] + declared["chr_size"] + (512 if declared["trainer"] else 0)
    for part in concat:
        p = os.path.join(work, part)
        if not os.path.exists(p) and part == "CHR_ROM.chr" and declared["chr_size"] == 0:
            log.append("CHR_ROM.chr omitted: header declares CHR RAM")
            continue
        if not os.path.exists(p) and part == "CHR_ROM.chr" and len(rom) == declared_size:
            log.append("CHR_ROM.chr omitted: linked PRG_ROM.bin already includes the declared CHR data")
            continue
        with open(p, "rb") as fh:
            rom += fh.read()
        used.append(part)

    if len(rom) != declared_size:
        raise ValueError("%s: ROM has %d bytes, header requires %d" % (game, len(rom), declared_size))

    shutil.copy2(dbg, os.path.join(outdir, "game.dbg"))
    shutil.copy2(mapf, os.path.join(outdir, "game.map"))
    nes_path = os.path.join(outdir, "game.nes")
    with open(nes_path, "wb") as fh:
        fh.write(rom)

    info = header_info(rom)
    body = rom[16:]
    sha1 = hashlib.sha1(rom).hexdigest().upper()
    expect_size = int(sh["NES_OUTPUT_FILE_SIZE"]) if sh["NES_OUTPUT_FILE_SIZE"] else None
    sha1_expected = sh["NES_OUTPUT_FILE_SHA1_ORIGINAL"].upper()
    upstream_mismatch = UPSTREAM_MISMATCH.get(game) == (sha1_expected, sha1)
    crc_file = "%08X" % (zlib.crc32(rom) & 0xFFFFFFFF)
    crc_body = "%08X" % (zlib.crc32(body) & 0xFFFFFFFF)
    info.update({
        "game": game,
        "slug": slug(game),
        "ok": True,
        "parts": used,
        "size": len(rom),
        "expected_size": expect_size,
        "size_ok": expect_size is None or expect_size == len(rom),
        "sha1": sha1,
        "expected_sha1": sha1_expected,
        "sha1_ok": sha1_expected == sha1,
        "crc32_file": crc_file,
        "crc32_nohdr": crc_body,
        "expected_crc32": crc_expected,
        "crc32_ok": crc_expected in (crc_file, crc_body),
        "hash_required": True,
        "upstream_mismatch": upstream_mismatch,
        "identity_ok": upstream_mismatch or (sha1_expected == sha1 and crc_expected in (crc_file, crc_body)),
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

    os.makedirs(a.out, exist_ok=True)
    results, bad = [], 0
    for g in games:
        try:
            r = build_game(a.disasm, g, a.out, a.ca65, a.ld65, a.keep_work)
        except (OSError, ValueError) as err:
            r = {"game": g, "ok": False, "error": str(err)}
        results.append(r)
        if not r.get("ok"):
            print("%-40s BUILD FAILED: %s" % (g, r.get("error")))
            print(r.get("log", ""))
            bad += 1
            continue
        flag = "known upstream mismatch" if r["upstream_mismatch"] else (
            "ok" if r["size_ok"] and r["identity_ok"] else "MISMATCH")
        print("%-40s mapper %-3d PRG %6d CHR %6d size %-6s sha1 %-5s readme-crc %-5s %s" % (
            g, r["mapper"], r["prg_size"], r["chr_size"], r["size_ok"], r["sha1_ok"], r["crc32_ok"], flag))
        if not r["size_ok"] or not r["identity_ok"]:
            bad += 1
    with open(os.path.join(a.out, "rom_summary.json"), "w") as fh:
        json.dump(results, fh, indent=2, sort_keys=True)
    print("%d game(s), %d problem(s)" % (len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
