#!/usr/bin/env python3
"""
Generate C header files from irrigation_methods.csv for enhanced irrigation system.
This script processes the comprehensive irrigation methods database with efficiency and application parameters.
Usage: python csv2irrigation_methods.py irrigation_methods.csv
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

def parse_range_value(value: str, use_min: bool = True) -> float:
    """Parse range values like '15-35' or '5-10', return min or max."""
    if not value or value.strip() == '':
        return 0.0
    
    value = value.strip()
    if '-' in value:
        parts = value.split('-')
        try:
            min_val = float(parts[0])
            max_val = float(parts[1])
            return min_val if use_min else max_val
        except (ValueError, IndexError):
            return 0.0
    else:
        return safe_float(value, 0.0)

def generate_irrigation_methods_files(csv_file: Path) -> None:
    """Generate irrigation_methods_db.inc, irrigation_methods_db.c and irrigation_methods_db.json from CSV data."""
    
    with open(csv_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    
    if not rows:
        print("ERROR: No data in irrigation_methods.csv")
        sys.exit(1)
    
    print(f"Processing {len(rows)} irrigation method entries...")
    
    processed_methods = []
    
    for i, row in enumerate(rows):
        # Basic identification
        method_id = safe_int(row.get('method_id', ''), i)
        method_name = row.get('method_name', '').strip()
        code_enum = row.get('code_enum', '').strip()
        
        # Efficiency parameters
        efficiency_pct = safe_int(row.get('efficiency_pct', ''), 70)
        distribution_uniformity_pct = safe_int(row.get('distribution_uniformity_pct', ''), 75)
        
        # Wetting characteristics
        wetting_fraction = safe_float(row.get('wetting_fraction', ''), 1.0)
        
        # Application characteristics - parse ranges
        depth_typical_str = row.get('depth_typical_mm', '15-30')
        depth_min = parse_range_value(depth_typical_str, use_min=True)
        depth_max = parse_range_value(depth_typical_str, use_min=False)
        
        application_rate_str = row.get('application_rate_mm_h', '5-10')
        app_rate_min = parse_range_value(application_rate_str, use_min=True)
        app_rate_max = parse_range_value(application_rate_str, use_min=False)
        
        # Additional metadata
        compatible_soils = (row.get('compatible_soil_textures') or '').strip()
        recommended_for = (row.get('recommended_for') or '').strip()
        notes = (row.get('notes') or '').strip()
        
        # Validate and adjust data
        if efficiency_pct < 10 or efficiency_pct > 100:
            print(f"WARNING: Method {method_name} has invalid efficiency {efficiency_pct}%, adjusting to 70%")
            efficiency_pct = 70
        
        if wetting_fraction <= 0 or wetting_fraction > 1.0:
            print(f"WARNING: Method {method_name} has invalid wetting fraction {wetting_fraction}, adjusting to 1.0")
            wetting_fraction = 1.0
        
        if depth_min <= 0:
            depth_min = 10.0  # Default minimum
        if depth_max <= depth_min:
            depth_max = depth_min + 10.0  # Ensure max > min
        
        if app_rate_min <= 0:
            app_rate_min = 1.0  # Default minimum
        if app_rate_max <= app_rate_min:
            app_rate_max = app_rate_min + 5.0  # Ensure max > min
        
        method_data = {
            'method_id': method_id,
            'method_name': method_name,
            'code_enum': code_enum,
            'efficiency_pct': efficiency_pct,
            'wetting_fraction': wetting_fraction,
            'distribution_uniformity_pct': distribution_uniformity_pct,
            'depth_typical_min_mm': depth_min,
            'depth_typical_max_mm': depth_max,
            'application_rate_min_mm_h': app_rate_min,
            'application_rate_max_mm_h': app_rate_max,
            'compatible_soils': compatible_soils,
            'recommended_for': recommended_for,
            'notes': notes
        }
        
        processed_methods.append(method_data)
    
    print(f"Processed {len(processed_methods)} irrigation methods")
    
    # Generate .inc header file
    inc_content = f"""/**
 * @file irrigation_methods_db.inc
 * @brief Irrigation methods database definitions for irrigation system
 * Generated from {csv_file.name} - {len(processed_methods)} irrigation methods with efficiency and application parameters
 */

#ifndef IRRIGATION_METHODS_DB_INC
#define IRRIGATION_METHODS_DB_INC

#include <stdint.h>

typedef struct {{
    uint8_t method_id;
    const char *method_name;
    const char *code_enum;
    
    // Efficiency Parameters
    uint8_t efficiency_pct;            // Application efficiency %
    uint16_t wetting_fraction_x1000;   // Wetted area fraction × 1000
    uint8_t distribution_uniformity_pct; // Distribution uniformity %
    
    // Application Characteristics
    uint8_t depth_typical_min_mm;      // Minimum application depth
    uint8_t depth_typical_max_mm;      // Maximum application depth
    uint8_t application_rate_min_mm_h; // Minimum application rate
    uint8_t application_rate_max_mm_h; // Maximum application rate
}} irrigation_method_data_t;

#define IRRIGATION_METHODS_COUNT {len(processed_methods)}

extern const irrigation_method_data_t irrigation_methods_database[IRRIGATION_METHODS_COUNT];

// Irrigation method indices for quick lookup
"""
    
    # Add method defines
    for method in processed_methods:
        if method['code_enum']:
            safe_name = method['code_enum'].upper().replace(' ', '_').replace('-', '_')
            inc_content += f"#define {safe_name} {method['method_id']}\n"
    
    inc_content += "\n#endif // IRRIGATION_METHODS_DB_INC\n"
    
    # Generate .c implementation file
    c_content = f"""/**
 * @file irrigation_methods_db.c
 * @brief Irrigation methods database implementation for irrigation system
 * Generated from {csv_file.name} - {len(processed_methods)} irrigation methods with efficiency and application parameters
 */

#include "irrigation_methods_db.inc"

const irrigation_method_data_t irrigation_methods_database[IRRIGATION_METHODS_COUNT] = {{
"""
    
    for i, method in enumerate(processed_methods):
        c_content += f"""    {{ // [{method['method_id']}] {method['method_name']}
        .method_id = {method['method_id']},
        .method_name = "{method['method_name']}",
        .code_enum = "{method['code_enum']}",
        .efficiency_pct = {method['efficiency_pct']},
        .wetting_fraction_x1000 = {int(method['wetting_fraction'] * 1000)},
        .distribution_uniformity_pct = {method['distribution_uniformity_pct']},
        .depth_typical_min_mm = {int(method['depth_typical_min_mm'])},
        .depth_typical_max_mm = {int(method['depth_typical_max_mm'])},
        .application_rate_min_mm_h = {int(method['application_rate_min_mm_h'])},
        .application_rate_max_mm_h = {int(method['application_rate_max_mm_h'])}
    }}{"," if i < len(processed_methods) - 1 else ""}
"""
    
    c_content += "};\n"
    
    # Generate JSON for mobile app and validation
    json_data = {
        "metadata": {
            "source": csv_file.name,
            "irrigation_methods_count": len(processed_methods),
            "generation_timestamp": str(Path(__file__).stat().st_mtime)
        },
        "irrigation_methods": []
    }
    
    for method in processed_methods:
        json_data["irrigation_methods"].append({
            "method_id": method['method_id'],
            "method_name": method['method_name'],
            "code_enum": method['code_enum'],
            "efficiency": {
                "application_efficiency_percent": method['efficiency_pct'],
                "distribution_uniformity_percent": method['distribution_uniformity_pct'],
                "efficiency_category": "High" if method['efficiency_pct'] >= 85 else "Medium" if method['efficiency_pct'] >= 70 else "Low"
            },
            "coverage": {
                "wetting_fraction": method['wetting_fraction'],
                "wetting_percent": method['wetting_fraction'] * 100,
                "coverage_type": "Full" if method['wetting_fraction'] >= 0.9 else "Partial" if method['wetting_fraction'] >= 0.5 else "Localized"
            },
            "application_characteristics": {
                "typical_depth_range_mm": {
                    "minimum": method['depth_typical_min_mm'],
                    "maximum": method['depth_typical_max_mm']
                },
                "application_rate_range_mm_per_hour": {
                    "minimum": method['application_rate_min_mm_h'],
                    "maximum": method['application_rate_max_mm_h']
                }
            },
            "suitability": {
                "compatible_soil_textures": method['compatible_soils'],
                "recommended_for": method['recommended_for'],
                "notes": method['notes']
            }
        })
    
    # Write files to main src directory
    tools_dir = csv_file.resolve().parent
    project_root = tools_dir.parent
    src_dir = project_root / "src"
    if not src_dir.exists():
        print(f"ERROR: Directory {src_dir} does not exist!")
        sys.exit(1)
    
    inc_file = src_dir / "irrigation_methods_db.inc"
    c_file = src_dir / "irrigation_methods_db.c"
    json_file = csv_file.parent / "irrigation_methods_db.json"
    
    with open(inc_file, 'w', encoding='utf-8') as f:
        f.write(inc_content)
    
    with open(c_file, 'w', encoding='utf-8') as f:
        f.write(c_content)
    
    with open(json_file, 'w', encoding='utf-8') as f:
        json.dump(json_data, f, indent=2, ensure_ascii=False)
    
    print(f"OK Generated {inc_file} ({len(processed_methods)} irrigation methods)")
    print(f"OK Generated {c_file}")
    print(f"OK Generated {json_file}")
    
    # Print summary statistics
    print(f"\nIrrigation Methods Database Summary:")
    print(f"  • Total irrigation methods: {len(processed_methods)}")
    
    # Analyze efficiency distribution
    efficiency_values = [m['efficiency_pct'] for m in processed_methods]
    wetting_values = [m['wetting_fraction'] for m in processed_methods]
    
    print(f"  • Efficiency range: {min(efficiency_values)}% - {max(efficiency_values)}%")
    print(f"  • Wetting fraction range: {min(wetting_values):.2f} - {max(wetting_values):.2f}")
    
    # Categorize by efficiency
    high_eff = len([m for m in processed_methods if m['efficiency_pct'] >= 85])
    med_eff = len([m for m in processed_methods if 70 <= m['efficiency_pct'] < 85])
    low_eff = len([m for m in processed_methods if m['efficiency_pct'] < 70])
    
    print(f"  • Efficiency distribution:")
    print(f"    - High efficiency (>=85%): {high_eff} methods")
    print(f"    - Medium efficiency (70-84%): {med_eff} methods")
    print(f"    - Low efficiency (<70%): {low_eff} methods")
    
    # Categorize by coverage type
    full_coverage = len([m for m in processed_methods if m['wetting_fraction'] >= 0.9])
    partial_coverage = len([m for m in processed_methods if 0.5 <= m['wetting_fraction'] < 0.9])
    localized = len([m for m in processed_methods if m['wetting_fraction'] < 0.5])
    
    print(f"  • Coverage distribution:")
    print(f"    - Full coverage (>=90%): {full_coverage} methods")
    print(f"    - Partial coverage (50-89%): {partial_coverage} methods")
    print(f"    - Localized coverage (<50%): {localized} methods")

def main():
    if len(sys.argv) != 2:
        print("Usage: python csv2irrigation_methods.py irrigation_methods.csv")
        sys.exit(1)
    
    csv_file = Path(sys.argv[1])
    
    if not csv_file.exists():
        print(f"ERROR: {csv_file} not found")
        sys.exit(1)
    
    print("*** Generating irrigation methods database files...")
    generate_irrigation_methods_files(csv_file)
    print("SUCCESS: Irrigation methods database generation complete!")

if __name__ == "__main__":
    main()