#!/usr/bin/env python3
"""
Build script complet pentru AutoWatering - generează baza de date și construiește firmware-ul.
Usage: python build_all.py [clean]
"""

import sys
import subprocess
import time
import shutil
from pathlib import Path

def run_command(cmd, description, cwd=None):
    """Rulează o comandă și afișează progresul."""
    start_time = time.time()
    print(f"\n[BUILD] {description}")
    print(f"Comanda: {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    
    if cwd is None:
        cwd = Path.cwd()
    
    try:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, shell=True)
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
    """Curăță directoarele de build."""
    project_root = Path.cwd()
    build_dirs = ["build", "build_1", "cmake-build-debug"]
    
    for build_dir in build_dirs:
        build_path = project_root / build_dir
        if build_path.exists():
            print(f"Stergem {build_path}")
            shutil.rmtree(build_path)

def check_prerequisites():
    """Verifică că toate instrumentele necesare sunt disponibile."""
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
                print(f"✓ {tool}: {version}")
            else:
                print(f"✗ {tool}: Nu a putut fi executat")
                return False
        except FileNotFoundError:
            print(f"✗ {tool}: Nu este instalat sau nu este în PATH")
            return False
    
    return True

def main():
    """Funcția principală de build."""
    start_time = time.time()
    project_root = Path.cwd()
    tools_dir = project_root / "tools"
    
    print("=" * 70)
    print("🚀 AutoWatering - Build Script Complet")
    print("=" * 70)
    print(f"Proiect: {project_root}")
    print(f"Timp început: {time.strftime('%H:%M:%S')}")
    
    # Verifică argumente
    clean_first = len(sys.argv) > 1 and sys.argv[1] == "clean"
    
    if clean_first:
        print("\n[CLEAN] Curățăm build-urile anterioare...")
        clean_build()
    
    # Verifică prerequisite
    print("\n[CHECK] Verificăm instrumentele necesare...")
    if not check_prerequisites():
        print("EROARE: Lipsesc instrumente necesare!")
        sys.exit(1)
    
    # Pasul 1: Generează baza de date
    print("\n" + "="*50)
    print("PASUL 1: Generare bază de date")
    print("="*50)
    
    if not tools_dir.exists():
        print(f"EROARE: Directorul {tools_dir} nu există!")
        sys.exit(1)
    
    build_db_script = tools_dir / "build_database.py"
    if not build_db_script.exists():
        print(f"EROARE: {build_db_script} nu există!")
        sys.exit(1)
    
    if not run_command([sys.executable, "build_database.py"], 
                      "Generează baza de date din CSV", 
                      cwd=tools_dir):
        print("EROARE: Generarea bazei de date a eșuat!")
        sys.exit(1)
    
    # Pasul 2: Verifică că fișierele au fost generate
    print("\n[CHECK] Verificăm fișierele generate...")
    required_files = [
        project_root / "src" / "soil_table.inc",
        project_root / "src" / "soil_table.c", 
        project_root / "src" / "plant_db.inc",
        project_root / "src" / "plant_db.c"
    ]
    
    for file_path in required_files:
        if file_path.exists():
            size = file_path.stat().st_size
            print(f"✓ {file_path.relative_to(project_root)} ({size} bytes)")
        else:
            print(f"✗ {file_path.relative_to(project_root)} - LIPSEȘTE!")
            sys.exit(1)
    
    # Pasul 3: Build firmware
    print("\n" + "="*50)
    print("PASUL 2: Build firmware Zephyr")
    print("="*50)
    
    if not run_command("west build -b promicro_nrf52840",
                      "Construim firmware-ul pentru nRF52840",
                      cwd=project_root):
        print("EROARE: Build-ul firmware-ului a eșuat!")
        sys.exit(1)
    
    # Sumar final
    end_time = time.time()
    total_duration = end_time - start_time
    
    print("\n" + "="*70)
    print("🎉 BUILD COMPLET FINALIZAT CU SUCCES!")
    print("="*70)
    print(f"Timp total: {total_duration:.2f} secunde")
    print(f"Timp finalizare: {time.strftime('%H:%M:%S')}")
    
    # Verifică output-ul final
    build_dir = project_root / "build"
    zephyr_dir = build_dir / "zephyr" 
    
    if zephyr_dir.exists():
        firmware_files = [
            zephyr_dir / "zephyr.hex",
            zephyr_dir / "zephyr.bin", 
            zephyr_dir / "zephyr.elf"
        ]
        
        print(f"\nFișiere firmware generate în {zephyr_dir.relative_to(project_root)}:")
        for fw_file in firmware_files:
            if fw_file.exists():
                size_kb = fw_file.stat().st_size / 1024
                print(f"  ✓ {fw_file.name} ({size_kb:.1f} KB)")
            else:
                print(f"  ✗ {fw_file.name} - LIPSEȘTE!")
    
    print("\n📱 Fișiere pentru aplicația mobilă:")
    json_file = tools_dir / "plant_db.json"
    if json_file.exists():
        size_kb = json_file.stat().st_size / 1024
        print(f"  ✓ {json_file.relative_to(project_root)} ({size_kb:.1f} KB)")
    
    print("\n🔧 Pentru a flasha firmware-ul:")
    print("  west flash")
    print("\n💡 Pentru debug:")
    print("  west debug")

if __name__ == "__main__":
    main()
