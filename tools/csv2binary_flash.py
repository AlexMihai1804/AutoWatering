#!/usr/bin/env python3
"""
CSV to Binary Database Converter for External Flash LittleFS
Converts CSV databases to binary format for Zephyr embedded firmware.

Output: 
  - plants.bin   (plant database)
  - soils.bin    (soil database)  
  - irrigation.bin (irrigation methods database)

Binary format:
  - 16-byte header (magic, version, count, crc32, record_size, reserved)
  - Fixed-size records with no pointers

Usage: python csv2binary_flash.py [--output-dir DIR]
"""

import argparse
import csv
import struct
import zlib
from pathlib import Path
from typing import List, Tuple, Any

# Match sizes from database_flash.h
DB_MAGIC_PLANT = 0x504C4E54       # 'PLNT'
DB_MAGIC_SOIL = 0x534F494C        # 'SOIL'
DB_MAGIC_IRRIGATION = 0x49525247  # 'IRRG'
DB_VERSION_CURRENT = 1

# Header: magic(4) + version(2) + count(2) + crc32(4) + record_size(2) + reserved(2) = 16
HEADER_FORMAT = '<IHHIHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

# Plant record: 64 bytes
#   common_name[32] + scientific_name[32] = 64 base
#   But we need FAO-56 params too... let's define properly
# Adjusted for actual data:
#   category_id(1) + subtype_id(2) + name[24] + kc_ini(1)*100 + kc_mid(1)*100 + kc_end(1)*100 
#   + root_depth_max(1)*10 + depletion_fraction(1)*100 + stage_ini(1) + stage_dev(1) + stage_mid(1) + stage_end(1)
#   + flags(1) + padding = ~40 bytes, let's use 48

# Actual plant record layout - 48 bytes (packed, no alignment padding):
# H=subtype_id(2), B=category(1), B=pad(1), 24s=name, B*12=params, 8s=reserved
PLANT_RECORD_FORMAT = '<HBB24sBBBBBBBBBBBB8s'
PLANT_RECORD_SIZE = struct.calcsize(PLANT_RECORD_FORMAT)

# Soil record layout - 24 bytes:
# B=soil_id(1), 15s=name, B*4=params, H=awc(2), 2s=padding
SOIL_RECORD_FORMAT = '<B15sBBHBB2s'
SOIL_RECORD_SIZE = struct.calcsize(SOIL_RECORD_FORMAT)

# Irrigation record layout - 24 bytes:
# B=method_id(1), 15s=name, B*5=params, 3s=padding
IRRIGATION_RECORD_FORMAT = '<B15sBBBBB3s'
IRRIGATION_RECORD_SIZE = struct.calcsize(IRRIGATION_RECORD_FORMAT)

# Category mapping
CATEGORY_MAP = {
    'Agriculture': 0,
    'Vegetables': 1,
    'Fruits': 2,
    'Herbs': 3,
    'Ornamental': 4,
    'Trees': 5,
    'Houseplants': 6,
    'Lawns': 7,
}

# Tolerance mapping
TOLERANCE_MAP = {
    '': 1, 'LOW': 0, 'MED': 1, 'MEDIUM': 1, 'HIGH': 2, 'VHIGH': 3,
}

# Irrigation method mapping
IRRIGATION_METHOD_MAP = {
    'DRIP': 0, 'DRIP_PC': 1, 'SPRINKLER': 2, 'SURFACE': 3, 'FLOOD': 4,
    'MICRO_SPRAY': 5, 'SUBSURFACE': 6, 'MANUAL': 7
}


def safe_float(val: str, default: float = 0.0) -> float:
    """Parse float safely"""
    try:
        return float(val) if val.strip() else default
    except ValueError:
        return default


def safe_int(val: str, default: int = 0) -> int:
    """Parse int safely"""
    try:
        return int(float(val)) if val.strip() else default
    except ValueError:
        return default


def encode_string(s: str, max_len: int) -> bytes:
    """Encode string to fixed-length bytes, truncating if needed"""
    encoded = s.encode('utf-8', errors='replace')[:max_len - 1]
    return encoded + b'\x00' * (max_len - len(encoded))


def calc_crc32(data: bytes) -> int:
    """Calculate CRC32 matching Zephyr's implementation"""
    return zlib.crc32(data) & 0xFFFFFFFF


def pack_header(magic: int, count: int, crc32: int, record_size: int) -> bytes:
    """Pack database header"""
    return struct.pack(HEADER_FORMAT, magic, DB_VERSION_CURRENT, count, crc32, record_size, 0)


def convert_plants(csv_path: Path) -> Tuple[bytes, int]:
    """Convert plants CSV to binary records"""
    records = []
    
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        plant_id = 0
        
        for row in reader:
            # Parse values
            category = CATEGORY_MAP.get(row.get('category', ''), 0)
            
            kc_ini = int(safe_float(row.get('kc_ini', '0.3')) * 100)
            kc_mid = int(safe_float(row.get('kc_mid', '1.0')) * 100)
            kc_end = int(safe_float(row.get('kc_end', '0.5')) * 100)
            
            root_depth_max = int(safe_float(row.get('root_depth_max_m', '0.5')) * 10)  # dm
            depletion = int(safe_float(row.get('depletion_fraction_p', '0.5')) * 100)
            
            stage_ini = safe_int(row.get('stage_days_ini', '20'))
            stage_dev = safe_int(row.get('stage_days_dev', '30'))
            stage_mid = safe_int(row.get('stage_days_mid', '40'))
            stage_end = safe_int(row.get('stage_days_end', '20'))
            
            # Flags: bit0=indoor, bit1=toxic, bit2=edible
            flags = 0
            if row.get('indoor_ok', '').lower() in ('yes', 'true', '1'):
                flags |= 0x01
            if row.get('toxic_flag', '').lower() in ('yes', 'true', '1'):
                flags |= 0x02
            if row.get('edible_part', '').strip():
                flags |= 0x04
            
            drought_tol = TOLERANCE_MAP.get(row.get('drought_tolerance', '').upper(), 1)
            
            # Default irrigation method
            irrig_str = row.get('typ_irrig_method', 'DRIP').upper()
            irrig_method = IRRIGATION_METHOD_MAP.get(irrig_str, 0)
            
            # Common name
            name = row.get('common_name_en', row.get('common_name_ro', ''))[:23]
            
            # Clamp values to byte range
            kc_ini = min(255, max(0, kc_ini))
            kc_mid = min(255, max(0, kc_mid))
            kc_end = min(255, max(0, kc_end))
            root_depth_max = min(255, max(0, root_depth_max))
            depletion = min(255, max(0, depletion))
            stage_ini = min(255, max(0, stage_ini))
            stage_dev = min(255, max(0, stage_dev))
            stage_mid = min(255, max(0, stage_mid))
            stage_end = min(255, max(0, stage_end))
            
            record = struct.pack(PLANT_RECORD_FORMAT,
                plant_id,                     # subtype_id (H)
                category,                     # category_id (B)
                0,                            # padding byte (B)
                encode_string(name, 24),      # common_name
                kc_ini,                       # kc_ini
                kc_mid,                       # kc_mid
                kc_end,                       # kc_end
                root_depth_max,               # root_depth_max_dm
                depletion,                    # depletion_fraction
                stage_ini,                    # stage_ini
                stage_dev,                    # stage_dev
                stage_mid,                    # stage_mid
                stage_end,                    # stage_end
                flags,                        # flags
                drought_tol,                  # drought_tolerance
                irrig_method,                 # default_irrigation
                encode_string('', 8)          # reserved (8 bytes)
            )
            
            assert len(record) == PLANT_RECORD_SIZE, f"Plant record size mismatch: {len(record)} != {PLANT_RECORD_SIZE}"
            records.append(record)
            plant_id += 1
    
    data = b''.join(records)
    return data, len(records)


def convert_soils(csv_path: Path) -> Tuple[bytes, int]:
    """Convert soils CSV to binary records"""
    records = []
    
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        
        for row in reader:
            soil_id = safe_int(row.get('soil_id', '0'))
            soil_type = row.get('soil_type', '')[:14]
            fc = safe_int(row.get('fc_pctvol', '30'))
            pwp = safe_int(row.get('pwp_pctvol', '15'))
            awc = safe_int(row.get('awc_mm_per_m', '150'))
            infil = safe_int(row.get('infil_mm_h', '10'))
            p_raw = int(safe_float(row.get('p_raw', '0.5')) * 100)
            
            # Clamp
            fc = min(255, max(0, fc))
            pwp = min(255, max(0, pwp))
            awc = min(65535, max(0, awc))
            infil = min(255, max(0, infil))
            p_raw = min(255, max(0, p_raw))
            
            record = struct.pack(SOIL_RECORD_FORMAT,
                soil_id,
                encode_string(soil_type, 15),
                fc,
                pwp,
                awc,
                infil,
                p_raw,
                encode_string('', 2)
            )
            
            assert len(record) == SOIL_RECORD_SIZE, f"Soil record size mismatch: {len(record)} != {SOIL_RECORD_SIZE}"
            records.append(record)
    
    data = b''.join(records)
    return data, len(records)


def convert_irrigation(csv_path: Path) -> Tuple[bytes, int]:
    """Convert irrigation methods CSV to binary records"""
    records = []
    
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        
        for row in reader:
            method_id = safe_int(row.get('method_id', '0'))
            name = row.get('method_name', '')[:14]
            efficiency = safe_int(row.get('efficiency_pct', '80'))
            wetting = int(safe_float(row.get('wetting_fraction', '0.5')) * 100)
            
            # Parse depth range (e.g., "40-80" -> take average)
            depth_str = row.get('depth_typical_mm', '30')
            if '-' in depth_str:
                parts = depth_str.split('-')
                depth = (safe_int(parts[0]) + safe_int(parts[1])) // 2
            else:
                depth = safe_int(depth_str)
            
            # Parse application rate
            rate_str = row.get('application_rate_mm_h', '10')
            if '-' in rate_str:
                parts = rate_str.split('-')
                rate = (safe_int(parts[0]) + safe_int(parts[1])) // 2
            else:
                rate = safe_int(rate_str)
            
            du = safe_int(row.get('distribution_uniformity_pct', '85'))
            
            # Clamp
            efficiency = min(255, max(0, efficiency))
            wetting = min(255, max(0, wetting))
            depth = min(255, max(0, depth))
            rate = min(255, max(0, rate))
            du = min(255, max(0, du))
            
            record = struct.pack(IRRIGATION_RECORD_FORMAT,
                method_id,
                encode_string(name, 15),
                efficiency,
                wetting,
                depth,
                rate,
                du,
                encode_string('', 3)
            )
            
            assert len(record) == IRRIGATION_RECORD_SIZE, f"Irrigation record size mismatch: {len(record)} != {IRRIGATION_RECORD_SIZE}"
            records.append(record)
    
    data = b''.join(records)
    return data, len(records)


def write_binary_db(output_path: Path, magic: int, data: bytes, count: int, record_size: int):
    """Write binary database file with header"""
    crc = calc_crc32(data)
    header = pack_header(magic, count, crc, record_size)
    
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(data)
    
    total_size = len(header) + len(data)
    print(f"  Created {output_path.name}: {count} records, {total_size} bytes (CRC32: 0x{crc:08X})")


def main():
    parser = argparse.ArgumentParser(description='Convert CSV databases to binary format')
    parser.add_argument('--output-dir', '-o', type=Path, default=None,
                        help='Output directory for binary files (default: same as script)')
    parser.add_argument('--csv-dir', '-c', type=Path, default=None,
                        help='Directory containing CSV files (default: same as script)')
    args = parser.parse_args()
    
    script_dir = Path(__file__).parent
    csv_dir = args.csv_dir or script_dir
    output_dir = args.output_dir or script_dir / 'flash_db'
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print("=" * 60)
    print("CSV to Binary Database Converter")
    print("=" * 60)
    print(f"Record sizes: Plant={PLANT_RECORD_SIZE}, Soil={SOIL_RECORD_SIZE}, Irrigation={IRRIGATION_RECORD_SIZE}")
    print()
    
    # Convert plants
    plants_csv = csv_dir / 'plants_full.csv'
    if plants_csv.exists():
        print(f"Converting plants from {plants_csv}...")
        data, count = convert_plants(plants_csv)
        write_binary_db(output_dir / 'plants.bin', DB_MAGIC_PLANT, data, count, PLANT_RECORD_SIZE)
    else:
        print(f"WARNING: {plants_csv} not found, skipping plants")
    
    # Convert soils
    soils_csv = csv_dir / 'soil_db_new.csv'
    if soils_csv.exists():
        print(f"Converting soils from {soils_csv}...")
        data, count = convert_soils(soils_csv)
        write_binary_db(output_dir / 'soils.bin', DB_MAGIC_SOIL, data, count, SOIL_RECORD_SIZE)
    else:
        print(f"WARNING: {soils_csv} not found, skipping soils")
    
    # Convert irrigation
    irrigation_csv = csv_dir / 'irrigation_methods.csv'
    if irrigation_csv.exists():
        print(f"Converting irrigation from {irrigation_csv}...")
        data, count = convert_irrigation(irrigation_csv)
        write_binary_db(output_dir / 'irrigation.bin', DB_MAGIC_IRRIGATION, data, count, IRRIGATION_RECORD_SIZE)
    else:
        print(f"WARNING: {irrigation_csv} not found, skipping irrigation")
    
    print()
    print("=" * 60)
    print(f"Binary databases created in: {output_dir}")
    print("Copy these files to LittleFS /db/ directory on device")
    print("=" * 60)


if __name__ == '__main__':
    main()
