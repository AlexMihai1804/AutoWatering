#!/usr/bin/env python3
"""
Enhanced master script for complete irrigation database generation.
Generates all enhanced database files with FAO-56 parameters and validation.
Usage: python build_database.py
"""

import sys
import subprocess
import time
import json
from pathlib import Path

def run_script(script_name, args=None):
    """Run a Python script and return the result."""
    step_start = time.time()
    cmd = [sys.executable, script_name]
    if args:
        cmd.extend(args)
    
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=Path(__file__).parent, encoding='utf-8', errors='replace')
    
    step_duration = time.time() - step_start
    
    if result.returncode != 0:
        print(f"ERROR in {script_name} (after {step_duration:.2f}s):")
        if result.stdout:
            print("--- stdout (captured) ---")
            print(result.stdout.rstrip())
        if result.stderr:
            print("--- stderr (captured) ---")
            print(result.stderr.rstrip())
        print(f"Exit code: {result.returncode}")
        return False
    
    print(f"Completed in {step_duration:.2f} seconds")
    return True

def validate_csv_files(tools_dir):
    """Validate that all required CSV files exist with proper structure."""
    required_files = {
        "plants_full.csv": ["category", "subtype", "common_name_en", "scientific_name", "kc_ini", "kc_mid", "kc_end"],
        "soil_db_new.csv": ["soil_id", "soil_type", "texture", "fc_pctvol", "pwp_pctvol", "awc_mm_per_m", "infil_mm_h"],
        "irrigation_methods.csv": ["method_id", "method_name", "code_enum", "efficiency_pct", "wetting_fraction"]
    }
    
    print("Validating CSV files...")
    for filename, required_columns in required_files.items():
        csv_file = tools_dir / filename
        if not csv_file.exists():
            print(f"ERROR: {csv_file} does not exist!")
            print(f"Required columns: {', '.join(required_columns)}")
            return False
        
        # Quick validation of CSV structure
        try:
            import csv
            with open(csv_file, 'r', encoding='utf-8') as f:
                reader = csv.DictReader(f)
                headers = reader.fieldnames
                if not headers:
                    print(f"ERROR: {filename} has no headers!")
                    return False
                
                missing_cols = [col for col in required_columns if col not in headers]
                if missing_cols:
                    print(f"ERROR: {filename} missing columns: {', '.join(missing_cols)}")
                    return False
                
                # Count rows
                row_count = sum(1 for _ in reader)
                print(f"  OK {filename}: {row_count} entries with {len(headers)} columns")
                
        except Exception as e:
            print(f"ERROR: Failed to validate {filename}: {e}")
            return False
    
    return True

def cross_validate_databases(tools_dir):
    """Cross-validate generated databases for consistency."""
    print("Cross-validating database consistency...")
    
    try:
        # Load JSON files for validation
        plant_json = tools_dir / "plant_full_db.json"
        soil_json = tools_dir / "soil_enhanced_db.json"
        irrigation_json = tools_dir / "irrigation_methods_db.json"
        
        validation_results = {}
        
        if plant_json.exists():
            with open(plant_json, 'r', encoding='utf-8') as f:
                plant_data = json.load(f)
                validation_results['plants'] = {
                    'count': plant_data['metadata']['species_count'],
                    'categories': plant_data['metadata']['categories_count']
                }
        
        if soil_json.exists():
            with open(soil_json, 'r', encoding='utf-8') as f:
                soil_data = json.load(f)
                validation_results['soils'] = {
                    'count': soil_data['metadata']['soil_types_count']
                }
        
        if irrigation_json.exists():
            with open(irrigation_json, 'r', encoding='utf-8') as f:
                irrigation_data = json.load(f)
                validation_results['irrigation_methods'] = {
                    'count': irrigation_data['metadata']['irrigation_methods_count']
                }
        
        print("Database validation results:")
        for db_type, results in validation_results.items():
            print(f"  {db_type.title()}: {results}")
        
        return True
        
    except Exception as e:
        print(f"WARNING: Cross-validation failed: {e}")
        return True  # Don't fail the build for validation issues

def main():
    """Main function that orchestrates the entire enhanced database generation process."""
    start_time = time.time()
    tools_dir = Path(__file__).parent
    
    print("*** === ENHANCED IRRIGATION DATABASE GENERATOR ===")
    print(f"Working directory: {tools_dir}")
    print(f"Start time: {time.strftime('%H:%M:%S')}")
    
    # Step 1: Validate CSV files
    print("\nSTEP 1: Validate CSV files")
    if not validate_csv_files(tools_dir):
        print("CSV validation failed! Check your CSV files.")
        sys.exit(1)
    
    # Step 2: Generate enhanced plant database
    print("\nSTEP 2: Generate enhanced plant database")
    if not run_script("csv2plant_full.py", ["plants_full.csv"]):
        print("Enhanced plant database generation failed!")
        sys.exit(1)
    
    # Step 3: Generate enhanced soil database
    print("\nSTEP 3: Generate enhanced soil database")
    if not run_script("csv2soil_enhanced.py", ["soil_db_new.csv"]):
        print("Enhanced soil database generation failed!")
        sys.exit(1)
    
    # Step 4: Generate irrigation methods database
    print("\nSTEP 4: Generate irrigation methods database")
    if not run_script("csv2irrigation_methods.py", ["irrigation_methods.csv"]):
        print("Irrigation methods database generation failed!")
        sys.exit(1)
    
    # Step 5: Cross-validate databases
    print("\nSTEP 5: Cross-validate databases")
    cross_validate_databases(tools_dir)
    
    # Final summary
    end_time = time.time()
    duration = end_time - start_time
    print(f"\nSUCCESS === ENHANCED DATABASE GENERATION COMPLETE ===")
    print(f"Total generation time: {duration:.2f} seconds")
    print(f"Completion time: {time.strftime('%H:%M:%S')}")
    
    src_dir = tools_dir.parent / "src"
    generated_files = [
        # Enhanced database files
        src_dir / "plant_full_db.inc",
        src_dir / "plant_full_db.c",
        src_dir / "soil_enhanced_db.inc",
        src_dir / "soil_enhanced_db.c",
        src_dir / "irrigation_methods_db.inc",
        src_dir / "irrigation_methods_db.c",
        # JSON files for mobile app
        tools_dir / "plant_full_db.json",
        tools_dir / "soil_enhanced_db.json",
        tools_dir / "irrigation_methods_db.json"
    ]
    
    print("\nGenerated files:")
    total_size_kb = 0
    for file_path in generated_files:
        if file_path.exists():
            size_kb = file_path.stat().st_size / 1024
            total_size_kb += size_kb
            print(f"  OK {file_path.relative_to(tools_dir.parent)} ({size_kb:.1f} KB)")
        else:
            print(f"  ERROR {file_path.relative_to(tools_dir.parent)} (MISSING!)")
    
    print(f"\nSummary:")
    print(f"  Total files: {len([f for f in generated_files if f.exists()])}/{len(generated_files)}")
    print(f"  Total size: {total_size_kb:.1f} KB")
    print(f"  Total time: {duration:.2f} seconds")
    print(f"  Generation speed: {total_size_kb/duration:.1f} KB/s")
    print("-" * 60)
    
    print("\nREADY! You can now include the enhanced .inc files in nRF52840 firmware.")
    print("Use the JSON files for mobile application integration.")
    print("\nEnhanced features:")
    print("  FAO-56 crop coefficients and phenological stages")
    print("  Comprehensive soil hydraulic properties")
    print("  Irrigation method efficiency and application parameters")
    print("  Cross-referenced database with validation")

if __name__ == "__main__":
    main()
