#!/usr/bin/env python3
"""Add the SM8635 GPU platform kit objects to the Adreno735 archive.

Runs after component:Adreno735 has built build/adreno735/libAdreno735.a from the
driver source. This script compiles the four kit sources against the board's
staged copy (kits/peridot/sm8635-gpu, which is the device repository's package/
directory) and appends them to that archive, the same two-step pattern sheng uses
with build-adreno740-kits.py.

Usage (from the build tree root):
    scripts/build-adreno735-kits.py [--tree .]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

KIT_SOURCES = ("ShengGmu.c", "ShengGmuCommandDb.c", "ShengGpuSmmu.c", "ShengGmuVotes.c", "GmuFirmwareImage.c")
HEADERS = ("ShengGmuSource.h", "ShengGpuSmmuSource.h", "ShengGmuCommandDbSource.h", "ShengGmuVotesSource.h")


def recipe_flags(recipe, source, obj):
    command, index = [], 0
    while index < len(recipe["arguments"]):
        value = recipe["arguments"][index]
        if value == recipe["file"]:
            command.append(str(source))
        elif value == "-o":
            command += ["-o", str(obj)]
            index += 1
        elif value in ("-MF", "-MJ"):
            index += 1
        elif value == "-Xclang" and recipe["arguments"][index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W") and value not in ("-MD", "-MP"):
            command.append(value)
        index += 1
    return command


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=".")
    args = ap.parse_args()

    tree = Path(args.tree).resolve()
    kit = tree / "kits" / "peridot" / "sm8635-gpu"
    out = tree / "build" / "adreno735"
    archive = out / "libAdreno735.a"
    for header in HEADERS:
        if not (kit / header).is_file():
            sys.exit("missing %s under %s" % (header, kit))
    if not archive.is_file():
        sys.exit("missing %s: component:Adreno735 must build first" % archive)

    objdir = tree / "build" / "xnu" / "ios-obj" / "DEVELOPMENT_ARM64_BCM2837"
    c_recipe = json.loads((objdir / "bsd/DEVELOPMENT/bsd_init.o.json").read_text().strip().rstrip(","))

    dep_kits = Path("/root/work/UniXNU/kits")
    extra_sources = [dep_kits / "AdrenoFirmware" / n for n in ("AdrenoHfi.c", "AdrenoHfiStartup.c", "AdrenoHfiVotes.c", "AdrenoFirmware.c")]
    extra_sources += sorted((dep_kits / "ArmSmmu4K").glob("ArmSmmu4K*.c"))
    extra_sources += [dep_kits / "AdrenoPM4" / "AdrenoPM4.c"]
    includes = ["-I", str(kit)]
    for name in ("AdrenoFirmware", "ArmSmmu4K", "AdrenoPM4"):
        includes += ["-I", str(dep_kits / name)]

    sources = [kit / name for name in KIT_SOURCES] + extra_sources
    objects = []
    for source in sources:
        obj = out / (source.stem + ".kit.o")
        command = recipe_flags(c_recipe, source, obj) + includes
        result = subprocess.run(command, cwd=c_recipe["directory"], capture_output=True, text=True)
        if result.returncode:
            lines = [line for line in result.stderr.splitlines() if "error" in line.lower()]
            sys.exit("%s:\n%s" % (source.name, "\n".join(lines[:20]) or result.stderr[-1500:]))
        objects.append(obj)
        print("  %-26s %7d bytes" % (obj.name, obj.stat().st_size))

    subprocess.run(["xcrun", "ar", "rcs", str(archive), *[str(o) for o in objects]], check=True)
    print("PASS: kit objects added to %s" % archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
