#!/usr/bin/env bash
# End-to-end ps3recomp static recompilation & Android ARM64 APK build pipeline
# for LEGO Marvel Super Heroes (USA) — BLUS-31161.
#
# Usage:
#   bash scripts/lift_and_build.sh /path/to/decrypted/EBOOT.ELF
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: bash $0 /path/to/decrypted/EBOOT.ELF" >&2
    exit 2
fi

EBOOT_ELF="$1"
if [ ! -f "$EBOOT_ELF" ]; then
    echo "ERROR: EBOOT.ELF not found: $EBOOT_ELF" >&2
    exit 1
fi

# Verify ELF magic (\x7fELF) and not encrypted SELF (SCE\0)
MAGIC="$(python3 -c 'import sys; d=open(sys.argv[1],"rb").read(4); print(d.hex())' "$EBOOT_ELF")"
if [ "$MAGIC" = "53434500" ]; then
    echo "ERROR: $EBOOT_ELF is still an encrypted PS3 SELF (SCE\\0)." >&2
    echo "       Decrypt EBOOT.BIN first (e.g., RPCS3 'Dump Executable') to obtain a plain ELF64." >&2
    exit 1
fi
if [ "$MAGIC" != "7f454c46" ]; then
    echo "ERROR: $EBOOT_ELF is not a valid ELF file (magic=0x$MAGIC)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PS3RECOMP_DIR="$(cd -- "$PROJ_DIR/../ps3recomp" && pwd)"
NINJA_BIN="$PROJ_DIR/../.tools/ninja"
if [ ! -x "$NINJA_BIN" ]; then
    NINJA_BIN="$(command -v ninja)"
fi

ANALYSIS_DIR="$PROJ_DIR/analysis"
RECOMP_DIR="$PROJ_DIR/recompiled"
SPU_RAW_DIR="$PROJ_DIR/spu_images"
SPU_LIFT_DIR="$PROJ_DIR/spu"
BUILD_DIR="$PROJ_DIR/build-game"

mkdir -p "$ANALYSIS_DIR" "$RECOMP_DIR" "$SPU_RAW_DIR" "$SPU_LIFT_DIR"

echo "=== [1/6] Analyzing decrypted EBOOT.ELF (OPD, find_functions, imports, PRX coverage) ==="
python3 "$PS3RECOMP_DIR/tools/ppu_loader.py" "$EBOOT_ELF" -o "$ANALYSIS_DIR/"
python3 "$PS3RECOMP_DIR/tools/prx_analyzer.py" "$EBOOT_ELF" --stubs > "$ANALYSIS_DIR/prx_coverage.txt" || true

OPD_JSON="$(find "$ANALYSIS_DIR" -maxdepth 1 -name '*.functions.json' ! -name '*.all_functions.json' | head -n 1)"
IMP_JSON="$(find "$ANALYSIS_DIR" -maxdepth 1 -name '*.imports.json' | head -n 1)"
if [ -z "$OPD_JSON" ] || [ -z "$IMP_JSON" ]; then
    echo "ERROR: ppu_loader.py did not produce functions/imports JSON in $ANALYSIS_DIR" >&2
    exit 1
fi
FUNC_JSON="$ANALYSIS_DIR/EBOOT.all_functions.json"
python3 "$PS3RECOMP_DIR/tools/find_functions.py" "$EBOOT_ELF" \
    --seed-json "$OPD_JSON" \
    --output "$FUNC_JSON"

# Compute executable code end (end of highest SHF_EXECINSTR section)
CODE_END="$(python3 -c '
import json, subprocess, sys
out = subprocess.check_output([sys.executable, sys.argv[1], sys.argv[2], "--sections"], text=True)
data = json.loads(out)
end = 0
for s in data.get("section_headers", []):
    flags = int(s["flags"], 16)
    if flags & 0x4:
        end = max(end, int(s["addr"], 16) + int(s["size"], 16))
print(hex(end) if end else "")
' "$PS3RECOMP_DIR/tools/elf_parser.py" "$EBOOT_ELF")"

echo "=== [2/6] Lifting PowerPC 64-bit PPU functions -> C++ ($RECOMP_DIR) ==="
CODE_END_ARGS=()
if [ -n "$CODE_END" ]; then
    CODE_END_ARGS=(--code-end "$CODE_END")
fi
python3 "$PS3RECOMP_DIR/tools/ppu_lifter.py" "$EBOOT_ELF" \
    --functions "$FUNC_JSON" \
    --hle-stubs "$IMP_JSON" \
    "${CODE_END_ARGS[@]}" \
    --chunks 128 \
    --jobs 1 \
    --output "$RECOMP_DIR/"

echo "=== [3/6] Generating full HLE NID handler table ==="
python3 "$PS3RECOMP_DIR/tools/gen_hle_nids.py" --all --out "$RECOMP_DIR/ppu_hle_nids.cpp"

echo "=== [4/6] Extracting & lifting embedded SPU workloads ($SPU_LIFT_DIR) ==="
python3 "$PS3RECOMP_DIR/tools/build_spu_workloads.py" \
    --lifted "$SPU_LIFT_DIR" \
    --out "$SPU_LIFT_DIR/spu_workloads.c" \
    --constructor \
    --merge-chunks \
    --allow-partial \
    --input "$EBOOT_ELF" || true

echo "=== [5/6] Compiling native Android ARM64 library (libps3recomp_lego_marvel.so) ==="
cmake -S "$PROJ_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_SMOKE_MODE=OFF \
    -DRECOMP_DIR="$RECOMP_DIR" \
    -DSPU_DIR="$SPU_LIFT_DIR"
cmake --build "$BUILD_DIR" -j3 --target ps3recomp_lego_marvel lego_marvel_cli

echo "=== [6/6] Packaging signed Android ARM64 APK ==="
bash "$SCRIPT_DIR/package_apk.sh" "$BUILD_DIR" "$PROJ_DIR/dist/lego-marvel-ps3-arm64.apk"

echo "Done! Install $PROJ_DIR/dist/lego-marvel-ps3-arm64.apk and extract ISO game data with:"
echo "  python3 $SCRIPT_DIR/extract_iso.py --iso \"<game.iso>\" --out /storage/emulated/0/PS3/BLUS31161"
echo "  cp \"$EBOOT_ELF\" /storage/emulated/0/PS3/BLUS31161/PS3_GAME/USRDIR/EBOOT.ELF"
