#!/usr/bin/env python3
"""
Database validation tool for irrigation system CSV files.
Usage: python validate_db.py soil_table.csv plant_db.csv
"""

import csv
import sys
from pathlib import Path
from typing import List, Dict, Any

def validate_soil_table(filepath: Path) -> Dict[str, Any]:
    """Validate soil_table.csv schema and data ranges."""
    expected_cols = ['soil_type', 'texture', 'fc_pctvol', 'pwp_pctvol', 'awc_mm_per_m', 'infil_mm_h', 'p_raw']
    
    with open(filepath, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != expected_cols:
            print(f"ERROR: soil_table.csv has wrong columns. Expected: {expected_cols}")
            sys.exit(1)
        
        rows = list(reader)
        for i, row in enumerate(rows, 1):
            try:
                # Validate numeric ranges
                fc = float(row['fc_pctvol'])
                pwp = float(row['pwp_pctvol'])
                awc = float(row['awc_mm_per_m'])
                infil = float(row['infil_mm_h'])
                p_raw = float(row['p_raw'])
                
                if not (0 <= fc <= 100): raise ValueError(f"fc_pctvol out of range: {fc}")
                if not (0 <= pwp <= 100): raise ValueError(f"pwp_pctvol out of range: {pwp}")
                if not (0 <= awc <= 1000): raise ValueError(f"awc_mm_per_m out of range: {awc}")
                if not (0 <= infil <= 1000): raise ValueError(f"infil_mm_h out of range: {infil}")
                if not (0 <= p_raw <= 1): raise ValueError(f"p_raw out of range: {p_raw}")
                
            except ValueError as e:
                print(f"ERROR: soil_table.csv row {i}: {e}")
                sys.exit(1)
    
    return {'count': len(rows), 'file': 'soil_table.csv'}

def validate_plant_db(filepath: Path) -> Dict[str, Any]:
    """Validate plant_db.csv schema and data ranges."""
    expected_cols = ['category', 'species', 'kc_i', 'kc_mid', 'kc_end', 'root_m', 'raw_pct', 'mm_min', 'deficit_resist']
    
    with open(filepath, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != expected_cols:
            print(f"ERROR: plant_db.csv has wrong columns. Expected: {expected_cols}")
            sys.exit(1)
        
        rows = list(reader)
        categories = set()
        
        for i, row in enumerate(rows, 1):
            try:
                categories.add(row['category'])
                
                # Validate numeric ranges
                kc_i = float(row['kc_i'])
                kc_mid = float(row['kc_mid'])
                kc_end = float(row['kc_end'])
                root_m = float(row['root_m'])
                raw_pct = float(row['raw_pct'])
                mm_min = float(row['mm_min'])
                deficit_resist = float(row['deficit_resist'])
                
                if not (0 <= kc_i <= 2): raise ValueError(f"kc_i out of range: {kc_i}")
                if not (0 <= kc_mid <= 2): raise ValueError(f"kc_mid out of range: {kc_mid}")
                if not (0 <= kc_end <= 2): raise ValueError(f"kc_end out of range: {kc_end}")
                if not (0 <= root_m <= 10): raise ValueError(f"root_m out of range: {root_m}")
                if not (0 <= raw_pct <= 100): raise ValueError(f"raw_pct out of range: {raw_pct}")
                if not (0 <= mm_min <= 200): raise ValueError(f"mm_min out of range: {mm_min}")
                if not (0 <= deficit_resist <= 3): raise ValueError(f"deficit_resist out of range: {deficit_resist}")
                
            except ValueError as e:
                print(f"ERROR: plant_db.csv row {i}: {e}")
                sys.exit(1)
    
    return {'count': len(rows), 'categories': len(categories), 'file': 'plant_db.csv'}

def main():
    if len(sys.argv) != 3:
        print("Usage: python validate_db.py soil_table.csv plant_db.csv")
        sys.exit(1)
    
    soil_file = Path(sys.argv[1])
    plant_file = Path(sys.argv[2])
    
    if not soil_file.exists():
        print(f"ERROR: {soil_file} not found")
        sys.exit(1)
    
    if not plant_file.exists():
        print(f"ERROR: {plant_file} not found")
        sys.exit(1)
    
    print("*** Validating irrigation database files...")
    
    soil_info = validate_soil_table(soil_file)
    plant_info = validate_plant_db(plant_file)
    
    print(f"OK {soil_info['file']}: {soil_info['count']} soil types validated")
    print(f"OK {plant_info['file']}: {plant_info['count']} plants in {plant_info['categories']} categories validated")
    print("SUCCESS: All validation checks passed!")

if __name__ == "__main__":
    main()
