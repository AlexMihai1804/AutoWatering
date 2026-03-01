#!/usr/bin/env python3
"""
Simple one-command factory script (Python).

What it does:
1) Loads local settings from tools/factory.settings.json (creates from example on first run)
2) Detects HW ID automatically from nRF FICR via nrfjprog
3) Calls factory_provision_build_flash.py for provision/build/flash

Usage:
  python tools/factory_flash.py
  python tools/factory_flash.py --dry-run
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List


def run_capture(cmd: List[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)


def ensure_settings(settings_path: Path, example_path: Path) -> Dict[str, Any]:
    if not settings_path.exists():
        if example_path.exists():
            settings_path.write_text(example_path.read_text(encoding="utf-8"), encoding="utf-8")
        else:
            settings_path.write_text(json.dumps({
                "apiBaseUrl": "",
                "factoryToken": "",
                "defaultHwId": "",
                "board": "arduino_nano_33_ble",
                "buildDir": "build_factory",
                "sysbuild": True,
                "pristine": True,
                "flashRunner": "bossac",
                "openocdBin": "",
                "openocdInterfaceCfg": "interface/stlink.cfg",
                "openocdTargetCfg": "target/nordic/nrf52.cfg",
                "openocdTransport": "swd",
                "openocdSpeedKHz": 1000,
                "operator": "",
                "batch": ""
            }, indent=2), encoding="utf-8")

        print(f"[Factory] First run setup: {settings_path}")

    settings = json.loads(settings_path.read_text(encoding="utf-8-sig"))

    env_api = os.getenv("AUTOWATERING_FACTORY_API_BASE_URL", "").strip()
    env_token = os.getenv("AUTOWATERING_FACTORY_TOKEN", "").strip()

    changed = False
    if not str(settings.get("apiBaseUrl", "")).strip() and env_api:
        settings["apiBaseUrl"] = env_api
        changed = True
    if not str(settings.get("factoryToken", "")).strip() and env_token:
        settings["factoryToken"] = env_token
        changed = True

    if not str(settings.get("apiBaseUrl", "")).strip():
        settings["apiBaseUrl"] = input("API Base URL: ").strip()
        changed = True
    if not str(settings.get("factoryToken", "")).strip():
        settings["factoryToken"] = input("Factory token: ").strip()
        changed = True

    if changed:
        settings_path.write_text(json.dumps(settings, indent=2), encoding="utf-8")
        print(f"[Factory] Saved settings: {settings_path}")

    if not str(settings.get("apiBaseUrl", "")).strip() or not str(settings.get("factoryToken", "")).strip():
        raise RuntimeError("apiBaseUrl and factoryToken are required in tools/factory.settings.json")

    return settings


def detect_hw_id(project_root: Path, settings: Dict[str, Any]) -> str:
    default_hw = str(settings.get("defaultHwId", "")).strip()
    if default_hw:
        return default_hw

    def parse_ficr_words(text: str) -> List[str]:
        words = [m.group(1).upper() for m in re.finditer(r"0x([0-9A-Fa-f]{8})", text)]
        uniq: List[str] = []
        for value in words:
            if value == "10000060":
                continue
            if value not in uniq:
                uniq.append(value)
        return uniq

    probe = run_capture(["nrfjprog", "--memrd", "0x10000060", "--w", "32", "--n", "2"], cwd=project_root)
    if probe.returncode == 0:
        uniq = parse_ficr_words(probe.stdout + "\n" + probe.stderr)
        if len(uniq) >= 2:
            return f"{uniq[0]}{uniq[1]}"

    flash_runner = str(settings.get("flashRunner", "")).strip().lower()
    if flash_runner not in {"openocd", "openocd-stlink", "openocd_stlink", "stlink"}:
        raise RuntimeError(
            "Cannot auto-detect HW ID. Ensure nrfjprog is installed/in PATH and SWD/J-Link is connected."
        )

    openocd_bin = str(settings.get("openocdBin", "")).strip() or shutil.which("openocd") or ""
    if not openocd_bin and os.name == "nt":
        win_default = Path("C:/Program Files/OpenOCD/bin/openocd.exe")
        if win_default.exists():
            openocd_bin = str(win_default)

    if not openocd_bin:
        raise RuntimeError("Cannot auto-detect HW ID with ST-LINK: OpenOCD not found.")

    interface_cfg = str(settings.get("openocdInterfaceCfg", "interface/stlink.cfg")).strip() or "interface/stlink.cfg"
    target_cfg = str(settings.get("openocdTargetCfg", "target/nordic/nrf52.cfg")).strip() or "target/nordic/nrf52.cfg"
    transport = str(settings.get("openocdTransport", "swd")).strip() or "swd"
    speed_khz = int(settings.get("openocdSpeedKHz", 1000))

    cmd = [
        openocd_bin,
        "-f",
        interface_cfg,
        "-c",
        f"transport select {transport}",
        "-f",
        target_cfg,
        "-c",
        f"adapter speed {speed_khz}",
        "-c",
        "init; reset halt; mdw 0x10000060 2; shutdown",
    ]
    probe_openocd = run_capture(cmd, cwd=project_root)
    if probe_openocd.returncode != 0:
        raise RuntimeError("Cannot auto-detect HW ID with ST-LINK/OpenOCD.")

    uniq_openocd = parse_ficr_words(probe_openocd.stdout + "\n" + probe_openocd.stderr)
    if len(uniq_openocd) < 2:
        raise RuntimeError("Failed to parse DEVICEID words from OpenOCD output")

    return f"{uniq_openocd[0]}{uniq_openocd[1]}"


def build_command_args(settings: Dict[str, Any], hw_id: str, no_build: bool, no_flash: bool) -> List[str]:
    args: List[str] = [
        "--api-base-url", str(settings.get("apiBaseUrl", "")).strip(),
        "--factory-token", str(settings.get("factoryToken", "")).strip(),
        "--hw-id", hw_id,
    ]

    for key, flag in [
        ("board", "--board"),
        ("buildDir", "--build-dir"),
        ("flashRunner", "--flash-runner"),
        ("openocdBin", "--openocd-bin"),
        ("openocdInterfaceCfg", "--openocd-interface-cfg"),
        ("openocdTargetCfg", "--openocd-target-cfg"),
        ("openocdTransport", "--openocd-transport"),
        ("operator", "--operator"),
        ("batch", "--batch"),
    ]:
        value = str(settings.get(key, "")).strip()
        if value:
            args.extend([flag, value])

    if bool(settings.get("sysbuild", False)):
        args.append("--sysbuild")
    if bool(settings.get("pristine", False)):
        args.append("--pristine")
    openocd_speed = settings.get("openocdSpeedKHz", 1000)
    if isinstance(openocd_speed, int) and openocd_speed > 0:
        args.extend(["--openocd-speed-khz", str(openocd_speed)])
    if no_build:
        args.append("--skip-build")
    if no_flash:
        args.append("--skip-flash")

    return args


def main() -> int:
    parser = argparse.ArgumentParser(description="Simple one-command factory provisioning/build/flash")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()

    script_path = Path(__file__).resolve()
    tools_dir = script_path.parent
    project_root = tools_dir.parent

    settings_path = tools_dir / "factory.settings.json"
    example_path = tools_dir / "factory.settings.json.example"
    worker_script = tools_dir / "factory_provision_build_flash.py"

    settings = ensure_settings(settings_path, example_path)
    try:
        hw_id = detect_hw_id(project_root, settings)
    except Exception:
        if args.dry_run:
            hw_id = "AUTO_HW_ID_UNAVAILABLE"
        else:
            raise

    cmd = [sys.executable, str(worker_script)] + build_command_args(settings, hw_id, args.no_build, args.no_flash)

    print(f"[Factory] HW ID: {hw_id}")
    if args.dry_run:
        print("[DryRun] " + " ".join(cmd))
        return 0

    result = subprocess.run(cmd, cwd=str(project_root), text=True)
    return result.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[ERROR] {exc}")
        raise SystemExit(1)
