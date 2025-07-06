#!/usr/bin/env python3
"""
Generate C header files from soil_table.csv for nRF52840 firmware.
Usage: python csv2soil_header.py soil_table.csv
"""

import csv
import sys
from pathlib import Path
from typing import List, Dict

def generate_soil_header(csv_file: Path) -> None:
    """Generate soil_table.inc and soil_table.c from CSV data."""
    
    with open(csv_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    
    if not rows:
        print("ERROR: No data in soil_table.csv")
        sys.exit(1)
    
    # Generate .inc header file
    inc_content = f"""/**
 * @file soil_table.inc
 * @brief Soil type definitions for irrigation system
 * Generated from {csv_file.name}
 */

#ifndef SOIL_TABLE_INC
#define SOIL_TABLE_INC

#include <stdint.h>

typedef struct {{
    const char *soil_type;
    const char *texture;
    uint16_t fc_pctvol_x100;      // Field capacity % × 100
    uint16_t pwp_pctvol_x100;     // Permanent wilting point % × 100
    uint16_t awc_mm_per_m;        // Available water capacity mm/m
    uint16_t infil_mm_h;          // Infiltration rate mm/h
    uint16_t p_raw_x1000;         // RAW depletion factor × 1000
}} soil_data_t;

#define SOIL_TYPES_COUNT {len(rows)}

extern const soil_data_t soil_types[SOIL_TYPES_COUNT];

#endif // SOIL_TABLE_INC
"""
    
    # Generate .c implementation file
    c_content = f"""/**
 * @file soil_table.c
 * @brief Soil type data table for irrigation system
 * Generated from {csv_file.name}
 */

#include "soil_table.inc"

const soil_data_t soil_types[SOIL_TYPES_COUNT] = {{
"""
    
    for i, row in enumerate(rows):
        fc = int(float(row['fc_pctvol']) * 100)
        pwp = int(float(row['pwp_pctvol']) * 100)
        awc = int(float(row['awc_mm_per_m']))
        infil = int(float(row['infil_mm_h']))
        p_raw = int(float(row['p_raw']) * 1000)
        
        c_content += f"""    {{ // [{i}] {row['soil_type']}
        .soil_type = "{row['soil_type']}",
        .texture = "{row['texture']}",
        .fc_pctvol_x100 = {fc},
        .pwp_pctvol_x100 = {pwp},
        .awc_mm_per_m = {awc},
        .infil_mm_h = {infil},
        .p_raw_x1000 = {p_raw}
    }}{"," if i < len(rows) - 1 else ""}
"""
    
    c_content += "};\n"
    
    # Write files to main src directory
    tools_dir = csv_file.resolve().parent
    project_root = tools_dir.parent 
    src_dir = project_root / "src"
    if not src_dir.exists():
        print(f"EROARE: Directorul {src_dir} nu exista!")
        sys.exit(1)
    inc_file = src_dir / "soil_table.inc"
    c_file = src_dir / "soil_table.c"
    
    with open(inc_file, 'w', encoding='utf-8') as f:
        f.write(inc_content)
    
    with open(c_file, 'w', encoding='utf-8') as f:
        f.write(c_content)
    
    print(f"OK Generated {inc_file} ({len(rows)} soil types)")
    print(f"OK Generated {c_file}")

def main():
    if len(sys.argv) != 2:
        print("Usage: python csv2soil_header.py soil_table.csv")
        sys.exit(1)
    
    csv_file = Path(sys.argv[1])
    
    if not csv_file.exists():
        print(f"ERROR: {csv_file} not found")
        sys.exit(1)
    
    print("*** Generating soil type C headers...")
    generate_soil_header(csv_file)
    print("SUCCESS: Soil header generation complete!")

if __name__ == "__main__":
    main()
