#!/usr/bin/env python3
"""
Extract PS3_GAME metadata or full disc assets from a decrypted PS3 ISO9660 image
(such as LEGO Marvel Super Heroes BLUS-31161).

Usage:
    # Extract small metadata only (PARAM.SFO, ICON0.PNG, PIC1.PNG, PS3_DISC.SFB):
    python3 extract_iso.py --iso "/path/to/game.iso" --out ./assets --metadata-only

    # Extract full PS3_GAME/ directory (including GAME*.DAT ~8.3 GB) for Android runtime:
    python3 extract_iso.py --iso "/path/to/game.iso" --out /storage/emulated/0/PS3/BLUS31161
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

SECTOR_SIZE = 2048
CHUNK_SIZE = 4 * 1024 * 1024  # 4 MiB streaming copy


def read_dir_entries(f, lba: int, length: int):
    f.seek(lba * SECTOR_SIZE)
    data = f.read(length)
    pos = 0
    entries = []
    while pos < len(data):
        rlen = data[pos]
        if rlen == 0:
            next_sec = ((pos // SECTOR_SIZE) + 1) * SECTOR_SIZE
            if next_sec >= len(data):
                break
            pos = next_sec
            continue
        flba = struct.unpack_from("<I", data, pos + 2)[0]
        fsize = struct.unpack_from("<I", data, pos + 10)[0]
        flags = data[pos + 25]
        nlen = data[pos + 32]
        raw_name = data[pos + 33 : pos + 33 + nlen]
        pos += rlen
        if raw_name in (b"\x00", b"\x01"):
            continue
        name = raw_name.decode("latin1", "replace")
        if ";" in name:
            name = name.split(";", 1)[0]
        if name.endswith(".") and not (flags & 2):
            # Keep trailing dot only if filename has no extension other than dot
            name = name[:-1]
        is_dir = bool(flags & 2)
        entries.append((name, flba, fsize, is_dir))
    return entries


def extract_file(f, lba: int, size: int, dest_path: str):
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    f.seek(lba * SECTOR_SIZE)
    remaining = size
    with open(dest_path, "wb") as out:
        while remaining > 0:
            chunk = f.read(min(CHUNK_SIZE, remaining))
            if not chunk:
                raise IOError(f"Unexpected EOF while reading {dest_path}")
            out.write(chunk)
            remaining -= len(chunk)


def walk_iso(f, lba: int, length: int, rel_prefix: str, out_root: str, metadata_only: bool):
    for name, flba, fsize, is_dir in read_dir_entries(f, lba, length):
        rel = f"{rel_prefix}/{name}" if rel_prefix else name
        if is_dir:
            if name == "PS3_UPDATE":
                continue
            if metadata_only and rel in ("PS3_GAME/USRDIR", "PS3_GAME/TROPDIR"):
                continue
            os.makedirs(os.path.join(out_root, rel), exist_ok=True)
            walk_iso(f, flba, fsize, rel, out_root, metadata_only)
        else:
            if metadata_only and name not in ("PS3_DISC.SFB", "PARAM.SFO", "ICON0.PNG", "PIC1.PNG"):
                continue
            dest = os.path.join(out_root, rel)
            print(f"[extract] {rel} ({fsize:,} bytes) -> {dest}")
            extract_file(f, flba, fsize, dest)


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract PS3_GAME files from a decrypted PS3 ISO")
    ap.add_argument("--iso", required=True, help="Path to PS3 .iso image")
    ap.add_argument("--out", required=True, help="Output root directory")
    ap.add_argument("--metadata-only", action="store_true",
                    help="Only extract PS3_DISC.SFB, PARAM.SFO, ICON0.PNG, PIC1.PNG")
    args = ap.parse_args()

    with open(args.iso, "rb") as f:
        f.seek(16 * SECTOR_SIZE)
        pvd = f.read(SECTOR_SIZE)
        if pvd[0:6] != b"\x01CD001":
            print("ERROR: Not a valid ISO9660 primary volume descriptor", file=sys.stderr)
            return 1
        root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
        root_len = struct.unpack_from("<I", pvd, 156 + 10)[0]
        os.makedirs(args.out, exist_ok=True)
        walk_iso(f, root_lba, root_len, "", args.out, args.metadata_only)

    print("[extract] Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
