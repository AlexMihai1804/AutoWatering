#!/usr/bin/env python3
"""
Generate C header and JSON files from plant_db.csv for firmware and mobile app.
Usage: python csv2plant_header.py plant_db.csv
"""

import csv
import json
import sys
from pathlib import Path
from typing import List, Dict, Any

def generate_plant_files(csv_file: Path) -> None:
    """Generate plant_db.inc and plant_db.json from CSV data."""
    
    with open(csv_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    
    if not rows:
        print("ERROR: No data in plant_db.csv")
        sys.exit(1)
    
    # Group by category for better organization
    categories = {}
    for row in rows:
        cat = row['category']
        if cat not in categories:
            categories[cat] = []
        categories[cat].append(row)
    
    # Generate .inc header file
    inc_content = f"""/**
 * @file plant_db.inc
 * @brief Plant database definitions for irrigation system
 * Generated from {csv_file.name}
 */

#ifndef PLANT_DB_INC
#define PLANT_DB_INC

#include <stdint.h>

typedef struct {{
    const char *category;
    const char *species;
    uint16_t kc_i_x1000;         // Initial crop coefficient × 1000
    uint16_t kc_mid_x1000;       // Mid-season crop coefficient × 1000
    uint16_t kc_end_x1000;       // End-season crop coefficient × 1000
    uint16_t root_m_x1000;       // Root depth in meters × 1000
    uint8_t raw_pct;             // RAW depletion percentage
    uint8_t mm_min;              // Minimum irrigation mm
    uint16_t deficit_resist_x1000; // Deficit resistance × 1000
}} plant_data_t;

#define PLANT_SPECIES_COUNT {len(rows)}
#define PLANT_CATEGORIES_COUNT {len(categories)}

extern const plant_data_t plant_database[PLANT_SPECIES_COUNT];

// Category indices for quick lookup
"""
    
    # Add category enum
    for i, category in enumerate(categories.keys()):
        safe_name = category.upper().replace(' ', '_').replace('-', '_')
        inc_content += f"#define CATEGORY_{safe_name} {i}\n"
    
    inc_content += "\n#endif // PLANT_DB_INC\n"
    
    # Generate .c implementation file
    c_content = f"""/**
 * @file plant_db.c
 * @brief Plant database implementation for irrigation system
 * Generated from {csv_file.name}
 */

#include "plant_db.inc"

const plant_data_t plant_database[PLANT_SPECIES_COUNT] = {{
"""
    
    for i, row in enumerate(rows):
        kc_i = int(float(row['kc_i']) * 1000)
        kc_mid = int(float(row['kc_mid']) * 1000)
        kc_end = int(float(row['kc_end']) * 1000)
        root_m = int(float(row['root_m']) * 1000)
        raw_pct = int(float(row['raw_pct']))
        mm_min = int(float(row['mm_min']))
        deficit_resist = int(float(row['deficit_resist']) * 1000)
        
        c_content += f"""    {{ // [{i}] {row['species']}
        .category = "{row['category']}",
        .species = "{row['species']}",
        .kc_i_x1000 = {kc_i},
        .kc_mid_x1000 = {kc_mid},
        .kc_end_x1000 = {kc_end},
        .root_m_x1000 = {root_m},
        .raw_pct = {raw_pct},
        .mm_min = {mm_min},
        .deficit_resist_x1000 = {deficit_resist}
    }}{"," if i < len(rows) - 1 else ""}
"""
    
    c_content += "};\n"
    
    # Generate JSON for mobile app
    json_data = {
        "metadata": {
            "source": csv_file.name,
            "species_count": len(rows),
            "categories_count": len(categories)
        },
        "categories": {}
    }
    
    for category, plants in categories.items():
        json_data["categories"][category] = []
        for plant in plants:
            json_data["categories"][category].append({
                "species": plant['species'],
                "kc_initial": float(plant['kc_i']),
                "kc_mid": float(plant['kc_mid']),
                "kc_end": float(plant['kc_end']),
                "root_depth_m": float(plant['root_m']),
                "raw_percent": float(plant['raw_pct']),
                "min_irrigation_mm": float(plant['mm_min']),
                "deficit_resistance": float(plant['deficit_resist'])
            })
    
    # Write files to main src directory
    tools_dir = csv_file.resolve().parent
    project_root = tools_dir.parent
    src_dir = project_root / "src"
    if not src_dir.exists():
        print(f"EROARE: Directorul {src_dir} nu exista!")
        sys.exit(1)
    inc_file = src_dir / "plant_db.inc"
    c_file = src_dir / "plant_db.c"
    json_file = csv_file.parent / "plant_db.json"
    
    with open(inc_file, 'w', encoding='utf-8') as f:
        f.write(inc_content)
    
    with open(c_file, 'w', encoding='utf-8') as f:
        f.write(c_content)
    
    with open(json_file, 'w', encoding='utf-8') as f:
        json.dump(json_data, f, indent=2, ensure_ascii=False)
    
    print(f"OK Generated {inc_file} ({len(rows)} species)")
    print(f"OK Generated {c_file}")
    print(f"OK Generated {json_file} ({len(categories)} categories)")

def main():
    if len(sys.argv) != 2:
        print("Usage: python csv2plant_header.py plant_db.csv")
        sys.exit(1)
    
    csv_file = Path(sys.argv[1])
    
    if not csv_file.exists():
        print(f"ERROR: {csv_file} not found")
        sys.exit(1)
    
    print("*** Generating plant database files...")
    generate_plant_files(csv_file)
    print("SUCCESS: Plant database generation complete!")

if __name__ == "__main__":
    main()
