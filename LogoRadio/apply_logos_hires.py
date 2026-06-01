#!/usr/bin/env python3
"""Apply custom BC7 logos to Forza Horizon 6's HiRes/Anthem.zip."""
import math
import shutil
import struct
import zipfile
from pathlib import Path

SWATCHBIN_HEADER_SIZE = 0x8C

def extract_bc7_from_dds(dds_path: Path, width: int, height: int) -> bytes:
    """Extract BC7 payload from a DDS file (skip header, take only mip0)."""
    dds_bytes = dds_path.read_bytes()
    if len(dds_bytes) < 128:
        raise RuntimeError(f"DDS file too small: {dds_path}")

    fourcc = dds_bytes[84:88]
    header_size = 148 if fourcc == b"DX10" else 128

    # Calculate mip0 size for BC7
    mip0_size = math.ceil(width / 4) * math.ceil(height / 4) * 16
    return dds_bytes[header_size:header_size + mip0_size]

def patch_swatchbin(original: bytes, bc7_data: bytes, width: int, height: int) -> bytes:
    """Replace BC7 payload and update headers."""
    hcxt = original.find(b"HCXT")
    if hcxt < 0:
        raise ValueError("No HCXT chunk in swatchbin")

    buf = bytearray(original[:SWATCHBIN_HEADER_SIZE]) + bytearray(bc7_data)
    total_sz = len(buf)
    data_sz = len(bc7_data)

    struct.pack_into("<I", buf, 0x0C, total_sz)
    struct.pack_into("<I", buf, 0x24, data_sz)
    struct.pack_into("<I", buf, 0x28, data_sz)
    struct.pack_into("<I", buf, hcxt + 0x20, width)
    struct.pack_into("<I", buf, hcxt + 0x24, height)

    return bytes(buf)

def apply_logos(dds_map: dict[int, tuple[Path, int, int]], game_root: Path) -> None:
    """Apply logos to HiRes/Anthem.zip."""
    anthem = game_root / "media/UI/Textures/HiRes/Anthem.zip"
    backup = game_root / "media/UI/Textures/HiRes/Anthem.zip.py_backup"

    if not anthem.exists():
        raise FileNotFoundError(f"HiRes Anthem.zip not found at {anthem}")

    # Backup original
    if not backup.exists():
        shutil.copy2(anthem, backup)
        print(f"[backup] Created {backup}")

    # Load entries from backup
    with zipfile.ZipFile(backup, "r") as zin:
        entries = {info.filename: (info, zin.read(info.filename))
                   for info in zin.infolist()}

    # Apply each logo
    station_names = {
        1: "Horizon_Pulse",
        2: "Horizon_BassArena",
        3: "Horizon_BlockParty",
        4: "Horizon_XS",
        5: "Hospital_Records",
        6: "Gacha_City_Radio",
        7: "Sub_Pop_black_and_white",
        8: "Horizon_Wave",
        9: "Horizon_Opus",
        10: "Streamer_Mode",
    }

    for stn, (dds_path, width, height) in dds_map.items():
        fname = station_names.get(stn)
        if not fname:
            print(f"[warn] Station {stn} not found")
            continue

        zip_entry = f"HUD/RadioLogos/{fname}.swatchbin"
        if zip_entry not in entries:
            print(f"[warn] {zip_entry} not in ZIP")
            continue

        print(f"[encode] {dds_path.name} -> BC7 {width}x{height} (R{stn})")
        bc7_data = extract_bc7_from_dds(dds_path, width, height)

        expected = math.ceil(width / 4) * math.ceil(height / 4) * 16
        if len(bc7_data) != expected:
            print(f"[error] BC7 size mismatch: got {len(bc7_data)}, expected {expected}")
            raise RuntimeError(f"BC7 payload invalid for R{stn}")

        orig_info, orig_data = entries[zip_entry]
        patched = patch_swatchbin(orig_data, bc7_data, width, height)
        entries[zip_entry] = (orig_info, patched)
        print(f"[logo] R{stn} patched ({len(patched):,} bytes)")

    # Write new ZIP
    with zipfile.ZipFile(anthem, "w", zipfile.ZIP_DEFLATED) as zout:
        for path_name, (info, data) in entries.items():
            zout.writestr(path_name, data)

    print(f"[install] HiRes Anthem.zip written ({len(entries)} entries)")

if __name__ == "__main__":
    game_root = Path("E:/Steam/steamapps/common/ForzaHorizon6")

    # Map: station -> (DDS path, width, height)
    logos = {
        9: (Path("F:/WIP/EuroBeatFH6/LogoRadio/Horizon_Opus3.dds"), 494, 230),
    }

    apply_logos(logos, game_root)
    print("\n[OK] Done!")
