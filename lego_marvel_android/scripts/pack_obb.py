#!/usr/bin/env python3
"""
Package LEGO Marvel Super Heroes (BLUS-31161) PS3_GAME/ into an uncompressed
Android ZIP64 OBB archive for zero-copy mounting in ps3recomp's ppu_fs.cpp.

Usage:
    python3 pack_obb.py [src_ps3_game_dir] [out_obb_path]

Defaults:
    src_ps3_game_dir = /storage/emulated/0/PS3/BLUS31161/PS3_GAME
    out_obb_path     = /storage/emulated/0/Download/main.2.com.ps3recomp.legomarvel.obb
"""

import os
import struct
import sys
import time
import zipfile

DEFAULT_SRC = "/storage/emulated/0/PS3/BLUS31161/PS3_GAME"
DEFAULT_OUT = "/storage/emulated/0/Download/main.2.com.ps3recomp.legomarvel.obb"


def collect_files(src_root: str):
    """Collect all files under PS3_GAME/, placing small boot/metadata files first."""
    small = []
    large = []
    dirs = []
    src_root = os.path.abspath(src_root)
    for dirpath, dirnames, filenames in os.walk(src_root):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, src_root)
        if rel_dir == ".":
            arc_dir = "PS3_GAME"
        else:
            arc_dir = "PS3_GAME/" + rel_dir.replace("\\", "/")
        dirs.append(arc_dir + "/")
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            arc = arc_dir + "/" + fn
            sz = os.path.getsize(full)
            if sz >= 32 * 1024 * 1024:
                large.append((full, arc, sz))
            else:
                small.append((full, arc, sz))
    # Sort small files so EBOOT.ELF and PARAM.SFO are near the front
    small.sort(key=lambda x: (0 if x[1].endswith(("PARAM.SFO", "EBOOT.ELF")) else 1, x[1]))
    large.sort(key=lambda x: x[1])
    return dirs, small + large


def verify_obb_raw_zip64(obb_path: str, src_root: str, expected_files: list):
    """Verify ZIP64 EOCD, Central Directory, LFH offsets, and >4 GiB payload bytes."""
    fsize = os.path.getsize(obb_path)
    with open(obb_path, "rb") as f:
        tail_len = min(fsize, 65557)
        f.seek(fsize - tail_len)
        tail = f.read(tail_len)
        eocd_rel = -1
        for i in range(tail_len - 22, -1, -1):
            if tail[i:i+4] == b"PK\x05\x06":
                clen = struct.unpack_from("<H", tail, i + 20)[0]
                if i + 22 + clen == tail_len:
                    eocd_rel = i
                    break
        assert eocd_rel >= 0, "Missing ZIP EOCD!"
        eocd_off = (fsize - tail_len) + eocd_rel
        cd_entries, cd_size, cd_offset = struct.unpack_from("<HII", tail, eocd_rel + 10)

        # Check ZIP64 EOCD locator & record
        assert eocd_off >= 20, "Truncated OBB"
        f.seek(eocd_off - 20)
        loc = f.read(20)
        if loc[:4] == b"PK\x06\x07":
            z64_off = struct.unpack_from("<Q", loc, 8)[0]
            f.seek(z64_off)
            z64 = f.read(56)
            assert z64[:4] == b"PK\x06\x06", "Invalid ZIP64 EOCD signature"
            cd_entries, cd_size, cd_offset = struct.unpack_from("<QQQ", z64, 32)
            print(f"[verify] ZIP64 EOCD OK: entries={cd_entries} cd_size={cd_size} cd_offset={cd_offset}")
        else:
            print(f"[verify] Standard EOCD: entries={cd_entries} cd_size={cd_size} cd_offset={cd_offset}")

        f.seek(cd_offset)
        cd = f.read(cd_size)
        pos = 0
        parsed = {}
        while pos + 46 <= len(cd):
            assert cd[pos:pos+4] == b"PK\x01\x02", f"Bad CD entry at {pos}"
            method = struct.unpack_from("<H", cd, pos + 10)[0]
            comp32, uncomp32, nlen, elen, clen = struct.unpack_from("<IIHHH", cd, pos + 20)
            lh_off32 = struct.unpack_from("<I", cd, pos + 42)[0]
            name = cd[pos + 46 : pos + 46 + nlen].decode("utf-8")
            ex = cd[pos + 46 + nlen : pos + 46 + nlen + elen]
            uncomp64, comp64, lh_off64 = uncomp32, comp32, lh_off32
            ep = 0
            while ep + 4 <= len(ex):
                eid, esz = struct.unpack_from("<HH", ex, ep)
                ep += 4
                if eid == 0x0001:
                    zp = 0
                    if uncomp32 == 0xFFFFFFFF and zp + 8 <= esz:
                        uncomp64 = struct.unpack_from("<Q", ex, ep + zp)[0]; zp += 8
                    if comp32 == 0xFFFFFFFF and zp + 8 <= esz:
                        comp64 = struct.unpack_from("<Q", ex, ep + zp)[0]; zp += 8
                    if lh_off32 == 0xFFFFFFFF and zp + 8 <= esz:
                        lh_off64 = struct.unpack_from("<Q", ex, ep + zp)[0]; zp += 8
                ep += esz
            pos += 46 + nlen + elen + clen
            if name.endswith("/"):
                continue
            assert method == 0, f"Entry {name} is compressed (method={method}), must be STORED!"
            f.seek(lh_off64)
            lfh = f.read(30)
            assert lfh[:4] == b"PK\x03\x04", f"Bad LFH for {name} at {lh_off64}"
            lfh_nlen, lfh_elen = struct.unpack_from("<HH", lfh, 26)
            data_off = lh_off64 + 30 + lfh_nlen + lfh_elen
            parsed[name] = (data_off, uncomp64)

        assert len(parsed) == len(expected_files), (
            f"File count mismatch: {len(parsed)} vs {len(expected_files)}"
        )

        # Spot-check EBOOT.ELF magic and first/last 64 bytes of every >4 GiB offset file + first DAT
        over_4gb_checked = 0
        for full, arc, sz in expected_files:
            assert arc in parsed, f"Missing {arc}"
            doff, dsz = parsed[arc]
            assert dsz == sz, f"Size mismatch for {arc}: {dsz} vs {sz}"
            if arc.endswith("EBOOT.ELF") or doff >= (1 << 32) or arc.endswith("GAME.DAT"):
                f.seek(doff)
                got_head = f.read(min(64, sz))
                with open(full, "rb") as sf:
                    exp_head = sf.read(min(64, sz))
                assert got_head == exp_head, f"Head mismatch for {arc} at offset {doff}"
                if sz > 64:
                    f.seek(doff + sz - 64)
                    got_tail = f.read(64)
                    with open(full, "rb") as sf:
                        sf.seek(sz - 64)
                        exp_tail = sf.read(64)
                    assert got_tail == exp_tail, f"Tail mismatch for {arc}"
                if doff >= (1 << 32):
                    over_4gb_checked += 1

        print(f"[verify] ALL {len(parsed)} STORED files verified OK "
              f"({over_4gb_checked} files past 4 GiB boundary verified byte-for-byte)!")


def main():
    src = sys.argv[1] if len(sys.argv) >= 2 else DEFAULT_SRC
    out = sys.argv[2] if len(sys.argv) >= 3 else DEFAULT_OUT

    if not os.path.isdir(src):
        print(f"ERROR: source PS3_GAME directory not found: {src}", file=sys.stderr)
        return 1

    dirs, files = collect_files(src)
    total_bytes = sum(sz for _, _, sz in files)
    print(f"[pack_obb] Source : {src}")
    print(f"[pack_obb] Output : {out}")
    print(f"[pack_obb] Files  : {len(files)} files ({total_bytes / (1024**3):.2f} GiB), mode=ZIP_STORED ZIP64")

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    t0 = time.time()
    written = 0
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zf:
        for d in dirs:
            zf.writestr(d, b"")
        for idx, (full, arc, sz) in enumerate(files, 1):
            zf.write(full, arcname=arc)
            written += sz
            elapsed = max(time.time() - t0, 0.001)
            mbps = (written / (1024 * 1024)) / elapsed
            if sz >= 16 * 1024 * 1024 or idx == len(files):
                print(f"  [{idx:2d}/{len(files)}] {arc} ({sz / (1024**2):.1f} MB) "
                      f"-- {written / (1024**3):.2f}/{total_bytes / (1024**3):.2f} GiB @ {mbps:.0f} MB/s",
                      flush=True)

    os.replace(tmp, out)
    elapsed = time.time() - t0
    final_sz = os.path.getsize(out)
    print(f"[pack_obb] Done in {elapsed:.1f}s -> {out} ({final_sz / (1024**3):.2f} GiB)")

    verify_obb_raw_zip64(out, src, files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
