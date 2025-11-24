#!/usr/bin/env python3
"""
Generate C header files from soil_db_new.csv for enhanced irrigation system.
This script processes the comprehensive soil database with hydraulic properties.
Usage: python csv2soil_enhanced.py soil_db_new.csv
"""

import csv
import json
import sys
from pathlib import Path
from typing import List, Dict, Any

def safe_float(value: str, default: float = 0.0) -> float:
    """Safely convert string to float, return default if empty or invalid."""
    if not value or value.strip() == '':
        return default
    try:
        return float(value)
    except (ValueError, TypeError):
        return default

def safe_int(value: str, default: int = 0) -> int:
    """Safely convert string to int, return default if empty or invalid."""
    if not value or value.strip() == '':
        return default
    try:
        return int(float(value))  # Handle decimal strings
    except (ValueError, TypeError):
        return default

def generate_soil_files(csv_file: Path) -> None:
    """Generate soil_enhanced_db.inc, soil_enhanced_db.c and soil_enhanced_db.json from CSV data."""
    
    with open(csv_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    
    if not rows:
        print("ERROR: No data in soil_db_new.csv")
        sys.exit(1)
    
    print(f"Processing {len(rows)} soil type entries...")
    
    processed_soils = []
    
    for i, row in enumerate(rows):
        # Basic identification
        soil_id = safe_int(row.get('soil_id', ''), i)
        soil_type = row.get('soil_type', '').strip()
        texture = row.get('texture', '').strip()
        
        # Hydraulic properties (percentages as volume/volume)
        fc_pctvol = safe_float(row.get('fc_pctvol', ''), 25.0)  # Field capacity %
        pwp_pctvol = safe_float(row.get('pwp_pctvol', ''), 12.0)  # Permanent wilting point %
        
        # Available water capacity (mm per meter of soil depth)
        awc_mm_per_m = safe_float(row.get('awc_mm_per_m', ''), 150.0)
        
        # Infiltration rate (mm per hour)
        infil_mm_h = safe_float(row.get('infil_mm_h', ''), 10.0)
        
        # Default readily available water fraction
        p_raw = safe_float(row.get('p_raw', ''), 0.5)
        
        # Validate data ranges
        if fc_pctvol < pwp_pctvol:
            print(f"WARNING: Soil {soil_type} has FC < PWP, adjusting...")
            fc_pctvol = pwp_pctvol + 5.0  # Minimum 5% difference
        
        if awc_mm_per_m <= 0:
            # Calculate AWC from FC and PWP if not provided
            awc_mm_per_m = (fc_pctvol - pwp_pctvol) * 10.0  # Rough estimate
        
        if p_raw <= 0 or p_raw > 1.0:
            p_raw = 0.5  # Default to 50%
        
        soil_data = {
            'soil_id': soil_id,
            'soil_type': soil_type,
            'texture': texture,
            'fc_pctvol': fc_pctvol,
            'pwp_pctvol': pwp_pctvol,
            'awc_mm_per_m': awc_mm_per_m,
            'infil_mm_h': infil_mm_h,
            'p_raw': p_raw
        }
        
        processed_soils.append(soil_data)
    
    print(f"Processed {len(processed_soils)} soil types")
    
    # Generate .inc header file
    inc_content = f"""/**
 * @file soil_enhanced_db.inc
 * @brief Enhanced soil database definitions for irrigation system
 * Generated from {csv_file.name} - {len(processed_soils)} soil types with hydraulic properties
 */

#ifndef SOIL_ENHANCED_DB_INC
#define SOIL_ENHANCED_DB_INC

#include <stdint.h>

typedef struct {{
    uint8_t soil_id;
    const char *soil_type;
    const char *texture;
    
    // Hydraulic Properties (scaled for embedded storage)
    uint16_t fc_pctvol_x100;           // Field capacity % × 100
    uint16_t pwp_pctvol_x100;          // Permanent wilting point % × 100
    uint16_t awc_mm_per_m;             // Available water capacity mm/m
    uint16_t infil_mm_h;               // Infiltration rate mm/h
    uint16_t p_raw_x1000;              // Default depletion fraction × 1000
}} soil_enhanced_data_t;

#define SOIL_ENHANCED_TYPES_COUNT {len(processed_soils)}

extern const soil_enhanced_data_t soil_enhanced_database[SOIL_ENHANCED_TYPES_COUNT];

// Soil type indices for quick lookup
"""
    
    # Add soil type defines
    for soil in processed_soils:
        safe_name = soil['soil_type'].upper().replace(' ', '_').replace('-', '_').replace('/', '_')
        inc_content += f"#define SOIL_{safe_name} {soil['soil_id']}\n"
    
    inc_content += "\n#endif // SOIL_ENHANCED_DB_INC\n"
    
    # Generate .c implementation file
    c_content = f"""/**
 * @file soil_enhanced_db.c
 * @brief Enhanced soil database implementation for irrigation system
 * Generated from {csv_file.name} - {len(processed_soils)} soil types with hydraulic properties
 */

#include "soil_enhanced_db.inc"

const soil_enhanced_data_t soil_enhanced_database[SOIL_ENHANCED_TYPES_COUNT] = {{
"""
    
    for i, soil in enumerate(processed_soils):
        c_content += f"""    {{ // [{soil['soil_id']}] {soil['soil_type']} ({soil['texture']})
        .soil_id = {soil['soil_id']},
        .soil_type = "{soil['soil_type']}",
        .texture = "{soil['texture']}",
        .fc_pctvol_x100 = {int(soil['fc_pctvol'] * 100)},
        .pwp_pctvol_x100 = {int(soil['pwp_pctvol'] * 100)},
        .awc_mm_per_m = {int(soil['awc_mm_per_m'])},
        .infil_mm_h = {int(soil['infil_mm_h'])},
        .p_raw_x1000 = {int(soil['p_raw'] * 1000)}
    }}{"," if i < len(processed_soils) - 1 else ""}
"""
    
    c_content += "};\n"
    
    # Generate JSON for mobile app and validation
    json_data = {
        "metadata": {
            "source": csv_file.name,
            "soil_types_count": len(processed_soils),
            "generation_timestamp": str(Path(__file__).stat().st_mtime)
        },
        "soil_types": []
    }
    
    for soil in processed_soils:
        json_data["soil_types"].append({
            "soil_id": soil['soil_id'],
            "soil_type": soil['soil_type'],
            "texture": soil['texture'],
            "hydraulic_properties": {
                "field_capacity_percent": soil['fc_pctvol'],
                "permanent_wilting_point_percent": soil['pwp_pctvol'],
                "available_water_capacity_mm_per_m": soil['awc_mm_per_m'],
                "available_water_range_percent": soil['fc_pctvol'] - soil['pwp_pctvol']
            },
            "infiltration": {
                "infiltration_rate_mm_per_hour": soil['infil_mm_h'],
                "infiltration_category": "High" if soil['infil_mm_h'] > 15 else "Medium" if soil['infil_mm_h'] > 5 else "Low"
            },
            "water_management": {
                "default_depletion_fraction": soil['p_raw'],
                "default_depletion_percent": soil['p_raw'] * 100,
                "management_strategy": "Conservative" if soil['p_raw'] < 0.4 else "Moderate" if soil['p_raw'] < 0.6 else "Aggressive"
            }
        })
    
    # Write files to main src directory
    tools_dir = csv_file.resolve().parent
    project_root = tools_dir.parent
    src_dir = project_root / "src"
    if not src_dir.exists():
        print(f"ERROR: Directory {src_dir} does not exist!")
        sys.exit(1)
    
    inc_file = src_dir / "soil_enhanced_db.inc"
    c_file = src_dir / "soil_enhanced_db.c"
    json_file = csv_file.parent / "soil_enhanced_db.json"
    
    with open(inc_file, 'w', encoding='utf-8') as f:
        f.write(inc_content)
    
    with open(c_file, 'w', encoding='utf-8') as f:
        f.write(c_content)
    
    with open(json_file, 'w', encoding='utf-8') as f:
        json.dump(json_data, f, indent=2, ensure_ascii=False)
    
    print(f"OK Generated {inc_file} ({len(processed_soils)} soil types)")
    print(f"OK Generated {c_file}")
    print(f"OK Generated {json_file}")
    
    # Print summary statistics
    print(f"\nSoil Database Summary:")
    print(f"  • Total soil types: {len(processed_soils)}")
    
    # Analyze hydraulic properties
    fc_values = [s['fc_pctvol'] for s in processed_soils]
    awc_values = [s['awc_mm_per_m'] for s in processed_soils]
    infil_values = [s['infil_mm_h'] for s in processed_soils]
    
    print(f"  • Field capacity range: {min(fc_values):.1f}% - {max(fc_values):.1f}%")
    print(f"  • Available water capacity range: {min(awc_values):.0f} - {max(awc_values):.0f} mm/m")
    print(f"  • Infiltration rate range: {min(infil_values):.0f} - {max(infil_values):.0f} mm/h")
    
    # Categorize soils by texture
    texture_groups = {}
    for soil in processed_soils:
        texture = soil['texture']
        if texture not in texture_groups:
            texture_groups[texture] = 0
        texture_groups[texture] += 1
    
    print(f"  • Texture distribution:")
    for texture, count in sorted(texture_groups.items()):
        print(f"    - {texture}: {count} types")

def main():
    if len(sys.argv) != 2:
        print("Usage: python csv2soil_enhanced.py soil_db_new.csv")
        sys.exit(1)
    
    csv_file = Path(sys.argv[1])
    
    if not csv_file.exists():
        print(f"ERROR: {csv_file} not found")
        sys.exit(1)
    
    print("*** Generating enhanced soil database files...")
    generate_soil_files(csv_file)
    print("SUCCESS: Enhanced soil database generation complete!")

if __name__ == "__main__":
    main()