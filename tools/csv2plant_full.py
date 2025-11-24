#!/usr/bin/env python3
"""Generate enhanced plant database sources from plants_full.csv.

Outputs (written under src/ unless JSON skipped):
  - plant_full_db.inc (struct + macros + constants)
  - plant_full_db.c   (const database array)
  - plant_full_db.fingerprint (sha256 integrity marker)
  - tools/plant_full_db.json (unless --no-json)

Non-functional improvements only; consuming code should behave identically.
"""

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def safe_float(v: str, default: float = 0.0) -> float:
    if v is None:
        return default
    v = v.strip()
    if not v:
        return default
    try:
        return float(v)
    except ValueError:
        return default


def safe_int(v: str, default: int = 0) -> int:
    if v is None:
        return default
    v = v.strip()
    if not v:
        return default
    try:
        return int(float(v))
    except ValueError:
        return default


def interpolate_kc_dev(kc_ini: float, kc_mid: float, kc_dev: Optional[float]) -> float:
    if kc_dev is not None and kc_dev > 0:
        return kc_dev
    return (kc_ini + kc_mid) / 2.0


def generate_plant_enum(subtype: str) -> str:
    if not subtype:
        return "PLANT_UNKNOWN"
    return subtype.strip().upper() or "PLANT_UNKNOWN"


def calculate_density_from_spacing(row_m: float, plant_m: float) -> float:
    if row_m <= 0 or plant_m <= 0:
        return 0.0
    area = row_m * plant_m
    return 0.0 if area <= 0 else 1.0 / area


def validate_ranges(p: Dict[str, Any]) -> List[str]:
    w: List[str] = []
    if not (0.2 <= p['kc_ini'] <= 1.2):
        w.append(f"kc_ini {p['kc_ini']:.2f} unusual")
    if not (0.4 <= p['kc_mid'] <= 1.5):
        w.append(f"kc_mid {p['kc_mid']:.2f} unusual")
    if not (0.3 <= p['kc_end'] <= 1.3):
        w.append(f"kc_end {p['kc_end']:.2f} unusual")
    if p['canopy_cover_max_frac'] > 1.0:
        w.append(f"canopy_cover_max_frac {p['canopy_cover_max_frac']:.2f} >1.0 (clamped)")
    if not (0.05 <= p['depletion_fraction_p'] <= 0.9):
        w.append(f"depletion_fraction_p {p['depletion_fraction_p']:.2f} unusual")
    if p['stage_days_mid'] > 1200:
        w.append(f"stage_days_mid {p['stage_days_mid']} very large")
    return w


def aggregate_stats(processed: List[Dict[str, Any]]) -> Dict[str, Tuple[float, float, float]]:
    stats: Dict[str, Tuple[float, float, float]] = {}
    for key in [
        'kc_ini','kc_mid','kc_end','root_depth_min_m','root_depth_max_m',
        'depletion_fraction_p','canopy_cover_max_frac','default_density_plants_m2']:
        vals = [p[key] for p in processed if p.get(key) is not None]
        if vals:
            stats[key] = (min(vals), max(vals), sum(vals)/len(vals))
    return stats


def compute_fingerprint(csv_file: Path, processed: List[Dict[str, Any]]) -> str:
    h = hashlib.sha256()
    try:
        h.update(csv_file.read_bytes())
    except FileNotFoundError:
        pass
    ordered = (
        'kc_ini','kc_mid','kc_end','kc_dev','root_depth_min_m','root_depth_max_m',
        'stage_days_ini','stage_days_dev','stage_days_mid','stage_days_end',
        'depletion_fraction_p','spacing_row_m','spacing_plant_m','default_density_plants_m2',
        'canopy_cover_max_frac'
    )
    for p in processed:
        for k in ordered:
            h.update(str(p.get(k, '')).encode('utf-8'))
    return h.hexdigest()


def generate(csv_file: Path, strict: bool, no_json: bool) -> None:
    with csv_file.open('r', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print('ERROR: empty CSV')
        sys.exit(1)

    categories: Dict[str, List[Dict[str, Any]]] = {}
    processed: List[Dict[str, Any]] = []
    warnings: List[str] = []

    for idx, row in enumerate(rows):
        cat = (row.get('category') or 'Unknown').strip()
        categories.setdefault(cat, [])

        kc_ini = safe_float(row.get('kc_ini', ''), 0.5)
        kc_mid = safe_float(row.get('kc_mid', ''), 1.0)
        kc_end = safe_float(row.get('kc_end', ''), 0.8)
        kc_dev = interpolate_kc_dev(kc_ini, kc_mid, safe_float(row.get('kc_dev', ''), None))

        root_min = safe_float(row.get('root_depth_min_m', ''), 0.3)
        root_max = safe_float(row.get('root_depth_max_m', ''), 1.0)

        st_ini = safe_int(row.get('stage_days_ini', ''), 20)
        st_dev = safe_int(row.get('stage_days_dev', ''), 30)
        st_mid = safe_int(row.get('stage_days_mid', ''), 40)
        st_end = safe_int(row.get('stage_days_end', ''), 20)

        depl = safe_float(row.get('depletion_fraction_p', ''), 0.5)
        if depl <= 0:
            depl = safe_float(row.get('allowable_depletion_pct', ''), 50.0) / 100.0

        spacing_row = safe_float(row.get('spacing_row_m', ''), 0.5)
        spacing_plant = safe_float(row.get('spacing_plant_m', ''), 0.3)
        density = safe_float(row.get('default_density_plants_m2', ''), 0.0)
        if density <= 0:
            density = calculate_density_from_spacing(spacing_row, spacing_plant)

        canopy = safe_float(row.get('canopy_cover_max_frac', ''), 0.8)

        frost_tol = safe_int(row.get('frost_tolerance_c', ''), 0)
        t_opt_min = safe_int(row.get('temp_opt_min_c', ''), 18)
        t_opt_max = safe_int(row.get('temp_opt_max_c', ''), 25)

        growth_cycle = {'Annual':0,'Biennial':1,'Perennial':2}.get((row.get('growth_cycle') or 'Annual').strip(), 0)
        method_id = {
            'SURFACE':0,'SPRINKLER':3,'DRIP':7,'MICRO':6,'MANUAL':13,'RAINFED':14
        }.get((row.get('typ_irrig_method') or 'SPRINKLER').strip(), 3)

        plant = {
            'index': idx,
            'subtype_enum': generate_plant_enum(row.get('subtype','')),
            'common_name_en': (row.get('common_name_en') or '').strip(),
            'scientific_name': (row.get('scientific_name') or '').strip(),
            'category': cat,
            'kc_ini': kc_ini,
            'kc_mid': kc_mid,
            'kc_end': kc_end,
            'kc_dev': kc_dev,
            'root_depth_min_m': root_min,
            'root_depth_max_m': root_max,
            'stage_days_ini': st_ini,
            'stage_days_dev': st_dev,
            'stage_days_mid': st_mid,
            'stage_days_end': st_end,
            'depletion_fraction_p': depl,
            'spacing_row_m': spacing_row,
            'spacing_plant_m': spacing_plant,
            'default_density_plants_m2': density,
            'canopy_cover_max_frac': canopy,
            'frost_tolerance_c': frost_tol,
            'temp_opt_min_c': t_opt_min,
            'temp_opt_max_c': t_opt_max,
            'growth_cycle': growth_cycle,
            'typ_irrig_method_id': method_id,
        }
        categories[cat].append(plant)
        processed.append(plant)
        for m in validate_ranges(plant):
            warnings.append(f"Row {idx} ({plant['subtype_enum'] or plant['common_name_en']}): {m}")

    print(f"Processed {len(processed)} plants across {len(categories)} categories")

    # Header (.inc)
    expected_struct_size = 44  # 3 ptr (12) + 12*2 + 8*1 for 32-bit
    inc_lines: List[str] = []
    inc_lines.append('/**')
    inc_lines.append(' * @file plant_full_db.inc')
    inc_lines.append(' * @brief Enhanced plant database definitions')
    inc_lines.append(f' * Generated from {csv_file.name} - {len(processed)} species')
    inc_lines.append(' */\n')
    inc_lines.append('#ifndef PLANT_FULL_DB_INC')
    inc_lines.append('#define PLANT_FULL_DB_INC\n')
    inc_lines.append('#include <stdint.h>\n')
    inc_lines.append('typedef struct {')
    inc_lines.append('    const char *subtype_enum;')
    inc_lines.append('    const char *common_name_en;')
    inc_lines.append('    const char *scientific_name;')
    inc_lines.append('    uint16_t kc_ini_x1000;')
    inc_lines.append('    uint16_t kc_mid_x1000;')
    inc_lines.append('    uint16_t kc_end_x1000;')
    inc_lines.append('    uint16_t kc_dev_x1000;')
    inc_lines.append('    uint16_t root_depth_min_m_x1000;')
    inc_lines.append('    uint16_t root_depth_max_m_x1000;')
    inc_lines.append('    uint16_t stage_days_mid;')
    inc_lines.append('    uint16_t depletion_fraction_p_x1000;')
    inc_lines.append('    uint16_t spacing_row_m_x1000;')
    inc_lines.append('    uint16_t spacing_plant_m_x1000;')
    inc_lines.append('    uint16_t default_density_plants_m2_x100;')
    inc_lines.append('    uint16_t canopy_cover_max_frac_x1000;')
    inc_lines.append('    uint8_t stage_days_ini;')
    inc_lines.append('    uint8_t stage_days_dev;')
    inc_lines.append('    uint8_t stage_days_end;')
    inc_lines.append('    int8_t  frost_tolerance_c;')
    inc_lines.append('    uint8_t temp_opt_min_c;')
    inc_lines.append('    uint8_t temp_opt_max_c;')
    inc_lines.append('    uint8_t growth_cycle;')
    inc_lines.append('    uint8_t typ_irrig_method_id;')
    inc_lines.append('} plant_full_data_t;\n')
    inc_lines.append(f'#define PLANT_FULL_SPECIES_COUNT {len(processed)}')
    inc_lines.append(f'#define PLANT_FULL_CATEGORIES_COUNT {len(categories)}')
    inc_lines.append(f'#define PLANT_FULL_DATA_T_EXPECTED_SIZE {expected_struct_size}')
    inc_lines.append('_Static_assert(sizeof(plant_full_data_t) == PLANT_FULL_DATA_T_EXPECTED_SIZE, "Unexpected plant_full_data_t size");\n')
    inc_lines.append('#define PLANT_KC_INI(p) ((p)->kc_ini_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_KC_MID(p) ((p)->kc_mid_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_KC_END(p) ((p)->kc_end_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_KC_DEV(p) ((p)->kc_dev_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_ROOT_MIN_M(p) ((p)->root_depth_min_m_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_ROOT_MAX_M(p) ((p)->root_depth_max_m_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_DEPL_FRACTION(p) ((p)->depletion_fraction_p_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_ROW_SPACING_M(p) ((p)->spacing_row_m_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_PLANT_SPACING_M(p) ((p)->spacing_plant_m_x1000 / 1000.0f)')
    inc_lines.append('#define PLANT_DENSITY_P_M2(p) ((p)->default_density_plants_m2_x100 / 100.0f)')
    inc_lines.append('#define PLANT_CANOPY_MAX_FRAC(p) ((p)->canopy_cover_max_frac_x1000 / 1000.0f)\n')
    inc_lines.append('extern const plant_full_data_t plant_full_database[PLANT_FULL_SPECIES_COUNT];')
    inc_lines.append('// Category indices')
    for i, cat in enumerate(categories.keys()):
        inc_lines.append(f'#define CATEGORY_{cat.upper().replace(" ","_").replace("-","_")} {i}')
    inc_lines.append('\n#endif // PLANT_FULL_DB_INC\n')
    inc_content = '\n'.join(inc_lines)

    # Implementation (.c)
    c_lines: List[str] = []
    c_lines.append('/**')
    c_lines.append(' * @file plant_full_db.c')
    c_lines.append(' * @brief Enhanced plant database implementation')
    c_lines.append(f' * Generated from {csv_file.name} - {len(processed)} species')
    c_lines.append(' */\n')
    c_lines.append('#include "plant_full_db.inc"\n')
    c_lines.append('#ifdef CONFIG_PLANT_DB_SECTION')
    c_lines.append('#define PLANT_DB_SECTION __attribute__((section(CONFIG_PLANT_DB_SECTION)))')
    c_lines.append('#else')
    c_lines.append('#define PLANT_DB_SECTION')
    c_lines.append('#endif\n')
    c_lines.append('const plant_full_data_t plant_full_database[PLANT_FULL_SPECIES_COUNT] PLANT_DB_SECTION = {')
    for i, p in enumerate(processed):
        c_lines.append(f'    {{ // [{i}] {p["common_name_en"]} ({p["scientific_name"]})')
        c_lines.append(f'        .subtype_enum = "{p["subtype_enum"]}",')
        c_lines.append(f'        .common_name_en = "{p["common_name_en"]}",')
        c_lines.append(f'        .scientific_name = "{p["scientific_name"]}",')
        c_lines.append(f'        .kc_ini_x1000 = {int(p["kc_ini"]*1000)},')
        c_lines.append(f'        .kc_mid_x1000 = {int(p["kc_mid"]*1000)},')
        c_lines.append(f'        .kc_end_x1000 = {int(p["kc_end"]*1000)},')
        c_lines.append(f'        .kc_dev_x1000 = {int(p["kc_dev"]*1000)},')
        c_lines.append(f'        .root_depth_min_m_x1000 = {int(p["root_depth_min_m"]*1000)},')
        c_lines.append(f'        .root_depth_max_m_x1000 = {int(p["root_depth_max_m"]*1000)},')
        c_lines.append(f'        .stage_days_mid = {p["stage_days_mid"]},')
        c_lines.append(f'        .depletion_fraction_p_x1000 = {int(p["depletion_fraction_p"]*1000)},')
        c_lines.append(f'        .spacing_row_m_x1000 = {int(p["spacing_row_m"]*1000)},')
        c_lines.append(f'        .spacing_plant_m_x1000 = {int(p["spacing_plant_m"]*1000)},')
        c_lines.append(f'        .default_density_plants_m2_x100 = {int(p["default_density_plants_m2"]*100)},')
        c_lines.append(f'        .canopy_cover_max_frac_x1000 = {min(1000, max(0, int(p["canopy_cover_max_frac"]*1000)))},')
        c_lines.append(f'        .stage_days_ini = {p["stage_days_ini"]},')
        c_lines.append(f'        .stage_days_dev = {p["stage_days_dev"]},')
        c_lines.append(f'        .stage_days_end = {p["stage_days_end"]},')
        c_lines.append(f'        .frost_tolerance_c = {p["frost_tolerance_c"]},')
        c_lines.append(f'        .temp_opt_min_c = {p["temp_opt_min_c"]},')
        c_lines.append(f'        .temp_opt_max_c = {p["temp_opt_max_c"]},')
        c_lines.append(f'        .growth_cycle = {p["growth_cycle"]},')
        c_lines.append(f'        .typ_irrig_method_id = {p["typ_irrig_method_id"]}')
        c_lines.append(f'    }}{"," if i < len(processed)-1 else ""}')
    c_lines.append('};\n')
    c_content = '\n'.join(c_lines)

    # Resolve project paths relative to this script's location (stable regardless of how CSV path is provided)
    script_dir = Path(__file__).resolve().parent
    tools_dir = script_dir  # alias for clarity
    project_root = script_dir.parent
    src_dir = project_root / 'src'
    if not src_dir.is_dir():
        print(f'ERROR: expected src dir missing: {src_dir.resolve()}')
        sys.exit(1)

    inc_path = src_dir / 'plant_full_db.inc'
    c_path = src_dir / 'plant_full_db.c'
    inc_path.write_text(inc_content, encoding='utf-8')
    c_path.write_text(c_content, encoding='utf-8')
    print(f'OK Generated {inc_path} ({len(processed)} species)')
    print(f'OK Generated {c_path}')

    fingerprint = compute_fingerprint(csv_file, processed)
    (src_dir / 'plant_full_db.fingerprint').write_text(fingerprint + '\n', encoding='utf-8')

    if not no_json:
        json_data: Dict[str, Any] = {
            'metadata': {
                'source': csv_file.name,
                'species_count': len(processed),
                'categories_count': len(categories),
                'fingerprint_sha256': fingerprint,
            },
            'categories': {}
        }
        for cat, plist in categories.items():
            json_data['categories'][cat] = []
            for p in plist:
                json_data['categories'][cat].append({
                    'index': p['index'],
                    'subtype_enum': p['subtype_enum'],
                    'common_name_en': p['common_name_en'],
                    'scientific_name': p['scientific_name'],
                    'fao56_coefficients': {
                        'kc_initial': p['kc_ini'],
                        'kc_development': p['kc_dev'],
                        'kc_mid_season': p['kc_mid'],
                        'kc_end_season': p['kc_end']
                    },
                    'root_development': {
                        'min_depth_m': p['root_depth_min_m'],
                        'max_depth_m': p['root_depth_max_m']
                    },
                    'phenology': {
                        'initial_days': p['stage_days_ini'],
                        'development_days': p['stage_days_dev'],
                        'mid_season_days': p['stage_days_mid'],
                        'end_season_days': p['stage_days_end'],
                        'total_cycle_days': p['stage_days_ini'] + p['stage_days_dev'] + p['stage_days_mid'] + p['stage_days_end']
                    },
                    'water_management': {
                        'depletion_fraction': p['depletion_fraction_p'],
                        'depletion_percent': p['depletion_fraction_p'] * 100
                    },
                    'plant_geometry': {
                        'row_spacing_m': p['spacing_row_m'],
                        'plant_spacing_m': p['spacing_plant_m'],
                        'density_plants_per_m2': p['default_density_plants_m2'],
                        'max_canopy_cover_fraction': p['canopy_cover_max_frac']
                    },
                    'environmental_limits': {
                        'frost_tolerance_c': p['frost_tolerance_c'],
                        'optimal_temp_min_c': p['temp_opt_min_c'],
                        'optimal_temp_max_c': p['temp_opt_max_c']
                    },
                    'metadata': {
                        'growth_cycle': ['Annual','Biennial','Perennial'][p['growth_cycle']],
                        'typical_irrigation_method_id': p['typ_irrig_method_id']
                    }
                })
        json_path = tools_dir / 'plant_full_db.json'
        json_path.write_text(json.dumps(json_data, indent=2, ensure_ascii=False), encoding='utf-8')
        print(f'OK Generated {json_path}')

    # Summary
    print('\nDatabase Summary:')
    print(f'  • Total species: {len(processed)}')
    print(f'  • Categories: {len(categories)}')
    for cat, plist in categories.items():
        print(f'    - {cat}: {len(plist)}')
    kc_mid_vals = [p['kc_mid'] for p in processed]
    print(f'  • Kc mid-season range: {min(kc_mid_vals):.2f} - {max(kc_mid_vals):.2f}')
    root_max_vals = [p['root_depth_max_m'] for p in processed]
    print(f'  • Root depth range: {min(root_max_vals):.2f} - {max(root_max_vals):.2f} m')
    stats = aggregate_stats(processed)
    for k,(mn,mx,av) in stats.items():
        print(f'  • {k}: min={mn:.3f} max={mx:.3f} avg={av:.3f}')
    if warnings:
        print(f'\nWARNINGS ({len(warnings)}):')
        for w in warnings:
            print(f'  - {w}')
        if strict:
            print('STRICT mode: aborting due to warnings')
            sys.exit(2)
    else:
        print('  • No range warnings.')


def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description='Generate enhanced plant database sources')
    p.add_argument('csv_file', help='Path to plants_full.csv')
    p.add_argument('--strict', action='store_true', help='Exit non-zero if validation warnings occur')
    p.add_argument('--no-json', action='store_true', help='Skip JSON output')
    return p.parse_args(argv)


def main() -> None:
    args = parse_args(sys.argv[1:])
    csv_path = Path(args.csv_file)
    if not csv_path.exists():
        print(f'ERROR: file not found: {csv_path}')
        sys.exit(1)
    print('*** Generating enhanced plant database files ...')
    generate(csv_path, args.strict, args.no_json)
    print('SUCCESS: generation complete')


if __name__ == '__main__':
    main()