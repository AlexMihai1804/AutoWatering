#!/usr/bin/env python3
"""
Master script pentru generarea completă a bazei de date de irigații.
Rulează toate scripturile în ordine și generează toate fișierele necesare.
Usage: python build_database.py
"""

import sys
import subprocess
import time
from pathlib import Path

def run_script(script_name, args=None):
    """Rulează un script Python și returnează rezultatul."""
    step_start = time.time()
    cmd = [sys.executable, script_name]
    if args:
        cmd.extend(args)
    
    print(f"Ruland: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=Path(__file__).parent)
    
    step_duration = time.time() - step_start
    
    if result.returncode != 0:
        print(f"EROARE in {script_name} (dupa {step_duration:.2f}s):")
        print(result.stderr)
        return False
    
    print(result.stdout)
    print(f"Finalizat in {step_duration:.2f} secunde")
    return True

def main():
    """Funcția principală care orchestrează întregul proces."""
    start_time = time.time()
    tools_dir = Path(__file__).parent
    
    print("*** === GENERATOR COMPLET BAZA DE DATE IRIGATII ===")
    print(f"Directorul de lucru: {tools_dir}")
    print(f"Timp inceput: {time.strftime('%H:%M:%S')}")
    
    # Verifică dacă fișierele CSV există
    soil_csv = tools_dir / "soil_table.csv"
    plant_csv = tools_dir / "plant_db.csv"
    
    if not soil_csv.exists():
        print(f"EROARE: {soil_csv} nu existe!")
        print("Creaza fisierul soil_table.csv cu coloanele:")
        print("soil_type,texture,fc_pctvol,pwp_pctvol,awc_mm_per_m,infil_mm_h,p_raw")
        sys.exit(1)
    
    if not plant_csv.exists():
        print(f"EROARE: {plant_csv} nu existe!")
        print("Creaza fisierul plant_db.csv cu coloanele:")
        print("category,species,kc_i,kc_mid,kc_end,root_m,raw_pct,mm_min,deficit_resist")
        sys.exit(1)
    
    # Pasul 1: Validare CSV
    print("\nPASUL 1: Validare fisiere CSV")
    if not run_script("validate_db.py", ["soil_table.csv", "plant_db.csv"]):
        print("Validarea a esuat! Verifica fisierele CSV.")
        sys.exit(1)
    
    # Pasul 2: Generare soil headers
    print("\nPASUL 2: Generare headers pentru tipuri de sol")
    if not run_script("csv2soil_header.py", ["soil_table.csv"]):
        print("Generarea headers pentru sol a esuat!")
        sys.exit(1)
    
    # Pasul 3: Generare plant database
    print("\nPASUL 3: Generare baza de date plante")
    if not run_script("csv2plant_header.py", ["plant_db.csv"]):
        print("Generarea bazei de date pentru plante a esuat!")
        sys.exit(1)
    
    # Sumar final
    end_time = time.time()
    duration = end_time - start_time
    print(f"\nSUCCESS === PROCESUL S-A FINALIZAT CU SUCCES ===")
    print(f"Timp total de generare: {duration:.2f} secunde")
    print(f"Timp finalizare: {time.strftime('%H:%M:%S')}")
    
    src_dir = tools_dir.parent / "src"
    generated_files = [
        src_dir / "soil_table.inc",
        src_dir / "soil_table.c",
        src_dir / "plant_db.inc", 
        src_dir / "plant_db.c",
        tools_dir / "plant_db.json"
    ]
    
    print("\nFisiere generate:")
    total_size_kb = 0
    for file_path in generated_files:
        if file_path.exists():
            size_kb = file_path.stat().st_size / 1024
            total_size_kb += size_kb
            print(f"  OK {file_path.relative_to(tools_dir.parent)} ({size_kb:.1f} KB)")
        else:
            print(f"  EROARE {file_path.relative_to(tools_dir.parent)} (LIPSESTE!)")
    
    print(f"\nSumar:")
    print(f"  • Total fisiere: {len([f for f in generated_files if f.exists()])}/{len(generated_files)}")
    print(f"  • Dimensiune totala: {total_size_kb:.1f} KB")
    print(f"  • Timp total: {duration:.2f} secunde")
    print(f"  • Viteza: {total_size_kb/duration:.1f} KB/s")
    print("-" * 60)
    
    print("\nGATA! Poti include fisierele .inc in firmware-ul nRF52840.")
    print("Foloseste plant_db.json pentru aplicatia mobila.")

if __name__ == "__main__":
    main()
