#!/usr/bin/env python3
"""
Build script complet pentru AutoWatering - genereaza baza de date si construieste firmware-ul.
Usage: python build_all.py [clean]
"""

import sys
import subprocess
import time
import shutil
from pathlib import Path

def run_command(cmd, description, cwd=None):
    """Ruleaza o comanda si afiseaza progresul."""
    start_time = time.time()
    print(f"\n[BUILD] {description}")
    print(f"Comanda: {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    
    if cwd is None:
        cwd = Path.cwd()
    
    try:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, shell=True, encoding='utf-8', errors='replace')
        duration = time.time() - start_time
        
        if result.returncode != 0:
            print(f"EROARE (dupa {duration:.2f}s):")
            print("STDOUT:", result.stdout)
            print("STDERR:", result.stderr)
            return False
        
        print(f"SUCCESS (in {duration:.2f}s)")
        if result.stdout.strip():
            print("Output:", result.stdout.strip())
        return True
        
    except Exception as e:
        duration = time.time() - start_time
        print(f"EXCEPTIE (dupa {duration:.2f}s): {e}")
        return False

def clean_build():
    """Curata directoarele de build."""
    project_root = Path.cwd()
    build_dirs = ["build", "build_1", "cmake-build-debug"]
    
    for build_dir in build_dirs:
        build_path = project_root / build_dir
        if build_path.exists():
            print(f"Stergem {build_path}")
            shutil.rmtree(build_path)

def check_prerequisites():
    """Verifica ca toate instrumentele necesare sunt disponibile."""
    tools = [
        ("python", "--version"),
        ("west", "--version"),
        ("cmake", "--version")
    ]
    
    for tool, version_arg in tools:
        try:
            result = subprocess.run([tool, version_arg], capture_output=True, text=True)
            if result.returncode == 0:
                version = result.stdout.strip().split('\n')[0]
                print(f"OK {tool}: {version}")
            else:
                print(f"FAIL {tool}: Nu a putut fi executat")
                return False
        except FileNotFoundError:
            print(f"FAIL {tool}: Nu este instalat sau nu este in PATH")
            return False
    
    return True

def main():
    """Functia principala de build."""
    start_time = time.time()
    project_root = Path.cwd()
    tools_dir = project_root / "tools"
    
    print("=" * 70)
    print("AutoWatering - Build Script Complet")
    print("=" * 70)
    print(f"Proiect: {project_root}")
    print(f"Timp inceput: {time.strftime('%H:%M:%S')}")
    
    # Verifica argumente
    clean_first = len(sys.argv) > 1 and sys.argv[1] == "clean"
    
    if clean_first:
        print("\n[CLEAN] Curatam build-urile anterioare...")
        clean_build()
    
    # Verifica prerequisite
    print("\n[CHECK] Verificam instrumentele necesare...")
    if not check_prerequisites():
        print("EROARE: Lipsesc instrumente necesare!")
        sys.exit(1)
    
    # Pasul 1: Genereaza baza de date
    print("\n" + "="*50)
    print("PASUL 1: Generare baza de date")
    print("="*50)
    
    if not tools_dir.exists():
        print(f"EROARE: Directorul {tools_dir} nu exista!")
        sys.exit(1)
    
    build_db_script = tools_dir / "build_database.py"
    if not build_db_script.exists():
        print(f"EROARE: {build_db_script} nu exista!")
        sys.exit(1)
    
    if not run_command([sys.executable, "build_database.py"],
                      "Genereaza baza de date din CSV",
                      cwd=tools_dir):
        print("EROARE: Generarea bazei de date a esuat!")
        sys.exit(1)
    
    # Pasul 2: Verifica ca fisierele au fost generate
    print("\n[CHECK] Verificam fisierele generate...")
    required_files = [
        project_root / "src" / "plant_full_db.inc",
        project_root / "src" / "plant_full_db.c",
        project_root / "src" / "soil_enhanced_db.inc",
        project_root / "src" / "soil_enhanced_db.c",
        project_root / "src" / "irrigation_methods_db.inc",
        project_root / "src" / "irrigation_methods_db.c",
    ]
    
    for file_path in required_files:
        if file_path.exists():
            size = file_path.stat().st_size
            print(f"OK {file_path.relative_to(project_root)} ({size} bytes)")
        else:
            print(f"FAIL {file_path.relative_to(project_root)} - LIPSESTE!")
            sys.exit(1)
    
    # Pasul 3: Build firmware
    print("\n" + "="*50)
    print("PASUL 2: Build firmware Zephyr")
    print("="*50)
    
    if not run_command("west build -b promicro_nrf52840",
                      "Construim firmware-ul pentru nRF52840",
                      cwd=project_root):
        print("EROARE: Build-ul firmware-ului a esuat!")
        sys.exit(1)
    
    # Sumar final
    end_time = time.time()
    total_duration = end_time - start_time
    
    print("\n" + "="*70)
    print("BUILD COMPLET FINALIZAT CU SUCCES!")
    print("="*70)
    print(f"Timp total: {total_duration:.2f} secunde")
    print(f"Timp finalizare: {time.strftime('%H:%M:%S')}")
    
    # Verifica output-ul final
    build_dir = project_root / "build"
    zephyr_dir = build_dir / "zephyr"
    
    if zephyr_dir.exists():
        firmware_files = [
            zephyr_dir / "zephyr.hex",
            zephyr_dir / "zephyr.bin",
            zephyr_dir / "zephyr.elf"
        ]
        
        print(f"\nFisiere firmware generate in {zephyr_dir.relative_to(project_root)}:")
        for fw_file in firmware_files:
            if fw_file.exists():
                size_kb = fw_file.stat().st_size / 1024
                print(f"  OK {fw_file.name} ({size_kb:.1f} KB)")
            else:
                print(f"  FAIL {fw_file.name} - LIPSESTE!")
    
    print("\nPentru a flasha firmware-ul:")
    print("  west flash")
    print("\nPentru debug:")
    print("  west debug")

if __name__ == "__main__":
    main()
