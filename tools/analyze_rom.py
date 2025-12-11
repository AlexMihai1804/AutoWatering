#!/usr/bin/env python3
"""ROM usage analyzer for Zephyr builds"""
import re
import os
from collections import defaultdict

map_path = "build/zephyr/zephyr.map"

if not os.path.exists(map_path):
    print(f"Map file not found: {map_path}")
    print("Run 'west build' first")
    exit(1)

print("=" * 75)
print("AUTOWATERING ROM USAGE REPORT")
print("=" * 75)

with open(map_path, 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

# Parse symbols with sizes from map file
symbol_pattern = re.compile(r'^\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)', re.IGNORECASE)

symbols = []
for line in content.split('\n'):
    match = symbol_pattern.match(line)
    if match:
        addr, size_hex, name = match.groups()
        try:
            size = int(size_hex, 16)
            if size > 0 and not name.startswith('.'):
                symbols.append((size, name))
        except:
            pass

# Categorize by component
categories = defaultdict(int)
app_files = defaultdict(int)

for size, name in symbols:
    if 'app/libapp.a' in name:
        # Extract filename
        match = re.search(r'\(([^)]+\.c)\.obj\)', name)
        if match:
            fname = match.group(1)
            app_files[fname] += size
        categories['Application Code'] += size
    elif 'bluetooth' in name.lower():
        categories['Bluetooth Stack'] += size
    elif 'mcumgr' in name.lower():
        categories['MCUmgr (OTA)'] += size
    elif 'mbedtls' in name.lower() or 'crypto' in name.lower():
        categories['mbedTLS/Crypto'] += size
    elif 'kernel' in name.lower():
        categories['Zephyr Kernel'] += size
    elif 'usb' in name.lower():
        categories['USB Stack'] += size
    elif 'hal_nordic' in name.lower() or 'nrfx' in name.lower():
        categories['Nordic HAL'] += size
    elif 'posix' in name.lower():
        categories['POSIX Layer'] += size
    elif 'newlib' in name.lower() or 'libc' in name.lower():
        categories['C Library (newlib)'] += size
    elif 'zephyr-sdk' in name or 'toolchains' in name:
        categories['Toolchain Libraries'] += size
    elif 'zephyr' in name.lower():
        categories['Zephyr Subsystems'] += size
    else:
        categories['Other'] += size

# Print category summary
print("\n📊 ROM USAGE BY CATEGORY:")
print("-" * 75)
total = sum(categories.values())
for cat, size in sorted(categories.items(), key=lambda x: -x[1]):
    pct = (size / total * 100) if total else 0
    bar = "█" * int(pct / 2)
    print(f"{size:>8,} bytes ({size/1024:>6.1f} KB) {pct:>5.1f}%  {bar} {cat}")

print("-" * 75)
print(f"{'TOTAL:':>8} {total:>14,} bytes ({total/1024:.1f} KB)")

# Print app file breakdown
print("\n📁 APPLICATION FILES BREAKDOWN:")
print("-" * 75)
app_total = sum(app_files.values())
for fname, size in sorted(app_files.items(), key=lambda x: -x[1])[:20]:
    pct = (size / app_total * 100) if app_total else 0
    print(f"{size:>8,} bytes ({size/1024:>6.1f} KB) {pct:>5.1f}%  {fname}")

print("-" * 75)
print(f"{'App Total:':>10} {app_total:>12,} bytes ({app_total/1024:.1f} KB)")

# Top individual symbols
print("\n🔍 TOP 30 LARGEST SYMBOLS:")
print("-" * 75)
symbols.sort(reverse=True, key=lambda x: x[0])
for size, name in symbols[:30]:
    # Shorten the name
    short = name
    if 'libapp.a' in name:
        match = re.search(r'\(([^)]+)\)', name)
        short = f"app: {match.group(1)}" if match else name
    elif len(name) > 60:
        short = "..." + name[-57:]
    print(f"{size:>8,} bytes  {short}")

print("\n" + "=" * 75)
print("OPTIMIZATION SUGGESTIONS:")
print("=" * 75)

suggestions = []
if categories.get('Application Code', 0) > 100000:
    suggestions.append("⚠️  environmental_history.c (53KB) - Consider reducing history buffer size")
    suggestions.append("⚠️  rain_history.c (28KB) - Consider reducing rain history entries")
if categories.get('Bluetooth Stack', 0) > 80000:
    suggestions.append("💡 BLE stack is large - consider disabling unused BLE features")
if categories.get('MCUmgr (OTA)', 0) > 10000:
    suggestions.append("ℹ️  MCUmgr adds ~10KB for OTA - expected for BLE DFU")
if categories.get('POSIX Layer', 0) > 5000:
    suggestions.append("💡 POSIX layer in use - disable if not needed (CONFIG_POSIX_API=n)")
if categories.get('C Library (newlib)', 0) > 20000:
    suggestions.append("💡 Consider CONFIG_NEWLIB_LIBC_FLOAT_PRINTF=n if float printing not needed")

for s in suggestions:
    print(s)

if not suggestions:
    print("✅ No major optimization opportunities found")

