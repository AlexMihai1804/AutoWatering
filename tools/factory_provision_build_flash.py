#!/usr/bin/env python3
"""
Factory provisioning + build + flash flow for AutoWatering.

Flow:
1) Calls backend POST /provision with hw_id
2) Generates a temporary Zephyr conf fragment with device serial
3) Builds firmware (optionally sysbuild/pristine/size-optimized)
4) Flashes firmware with bossac wrapper
5) Stores a provisioning manifest JSON for traceability

Usage example:
  python tools/factory_provision_build_flash.py \
    --api-base-url https://<api-id>.execute-api.<region>.amazonaws.com \
    --factory-token <token> \
    --hw-id A1B2C3D4E5F60708 \
    --sysbuild \
    --pristine
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Tuple
from urllib import error, request
import shutil


def run_command(cmd: list[str], cwd: Path) -> None:
    print(f"\n[RUN] {' '.join(cmd)}")
    env = os.environ.copy()
    python_exe = sys.executable
    env["WEST_PYTHON"] = python_exe
    env["PYTHON_EXECUTABLE"] = python_exe
    result = subprocess.run(cmd, cwd=str(cwd), text=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(cmd)}")


def resolve_openocd_bin(openocd_bin: str) -> str:
    user_value = openocd_bin.strip()
    if user_value:
        return user_value

    from_path = shutil.which("openocd")
    if from_path:
        return from_path

    win_default = Path("C:/Program Files/OpenOCD/bin/openocd.exe")
    if os.name == "nt" and win_default.exists():
        return str(win_default)

    raise RuntimeError(
        "OpenOCD binary not found. Set --openocd-bin or add openocd to PATH."
    )


def get_hex_image_path(build_dir: Path) -> Path:
    """Return the best hex image to flash.

    Prefer the *signed* hex produced by MCUboot's imgtool – it carries the
    MCUboot image header with the magic number.  The unsigned zephyr.hex has
    an empty header (all-zeros), which MCUboot rejects.
    """
    candidates = [
        # Sysbuild signed image (preferred)
        build_dir / "AutoWatering" / "zephyr" / "zephyr.signed.hex",
        # Standard signed image
        build_dir / "zephyr" / "zephyr.signed.hex",
        # Sysbuild unsigned fallback
        build_dir / "AutoWatering" / "zephyr" / "zephyr.hex",
        # Standard unsigned fallback
        build_dir / "zephyr" / "zephyr.hex",
    ]
    for hex_path in candidates:
        if hex_path.exists():
            return hex_path
    raise RuntimeError(f"Expected image not found in: {', '.join(str(p) for p in candidates)}")


def get_mcuboot_hex_path(build_dir: Path):
    """Return MCUboot bootloader hex path for sysbuild, or None."""
    mcuboot_hex = build_dir / "mcuboot" / "zephyr" / "zephyr.hex"
    if mcuboot_hex.exists():
        return mcuboot_hex
    return None


def flash_with_openocd_stlink(
    project_root: Path,
    build_dir: Path,
    openocd_bin: str,
    interface_cfg: str,
    target_cfg: str,
    transport: str,
    speed_khz: int,
) -> None:
    if speed_khz <= 0:
        raise RuntimeError(f"Invalid --openocd-speed-khz: {speed_khz}")

    hex_path = get_hex_image_path(build_dir)
    mcuboot_hex = get_mcuboot_hex_path(build_dir)
    openocd_exe = resolve_openocd_bin(openocd_bin)

    # Build the OpenOCD program sequence: flash MCUboot first (if sysbuild),
    # then the app image, verify both, and soft-reset.
    # We clear RESETREAS and use AIRCR software reset instead of 'reset run'
    # because CONFIG_BOOT_SERIAL_PIN_RESET makes MCUboot enter serial recovery
    # when the RESETPIN bit is set (which happens with SWD 'reset' commands).
    program_cmds = "init; halt; nrf5 mass_erase; "
    if mcuboot_hex:
        print(f"[INFO] Will flash MCUboot: {mcuboot_hex}")
        program_cmds += f"program {str(mcuboot_hex).replace(chr(92), '/')} verify; "
    print(f"[INFO] Will flash app: {hex_path}")
    program_cmds += f"program {str(hex_path).replace(chr(92), '/')} verify; "
    # Clear nRF52 RESETREAS register (w1c) and trigger AIRCR software reset
    program_cmds += "mww 0x40000400 0xFFFFFFFF; mww 0xE000ED0C 0x05FA0004; shutdown"

    cmd = [
        openocd_exe,
        "-f",
        interface_cfg,
        "-c",
        f"transport select {transport}",
        "-f",
        target_cfg,
        "-c",
        f"adapter speed {speed_khz}",
        "-c",
        program_cmds,
    ]

    run_command(cmd, project_root)


def normalize_base_url(base_url: str) -> str:
    url = base_url.strip().rstrip("/")
    if not (url.startswith("http://") or url.startswith("https://")):
        raise ValueError("--api-base-url must start with http:// or https://")
    return url


def call_provision(api_base_url: str, factory_token: str, hw_id: str, metadata: Dict[str, Any]) -> Dict[str, Any]:
    endpoint = f"{normalize_base_url(api_base_url)}/provision"
    body = json.dumps({"hw_id": hw_id, "metadata": metadata}).encode("utf-8")

    req = request.Request(
        endpoint,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "x-factory-token": factory_token,
        },
    )

    try:
        with request.urlopen(req, timeout=20) as resp:
            payload = resp.read().decode("utf-8")
            data = json.loads(payload)
            if not data.get("ok"):
                raise RuntimeError(f"Provisioning failed response: {payload}")
            return data
    except error.HTTPError as exc:
        details = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Provisioning HTTP {exc.code}: {details}") from exc
    except error.URLError as exc:
        raise RuntimeError(f"Provisioning request failed: {exc}") from exc


def validate_provisioning_payload(payload: Dict[str, Any]) -> Tuple[str, int, str]:
    serial = payload.get("serial")
    serial_seq = payload.get("serial_seq")
    thing_name = payload.get("thing_name")

    if not isinstance(serial, str) or len(serial) != 6 or not serial.isdigit():
        raise RuntimeError(f"Invalid serial in provisioning response: {serial!r}")

    if not isinstance(serial_seq, int) or serial_seq <= 0:
        raise RuntimeError(f"Invalid serial_seq in provisioning response: {serial_seq!r}")

    if not isinstance(thing_name, str) or len(thing_name.strip()) == 0:
        raise RuntimeError(f"Invalid thing_name in provisioning response: {thing_name!r}")

    return serial, serial_seq, thing_name


def write_conf_fragment(project_root: Path, serial: str) -> Path:
    conf_path = project_root / "build" / "factory_identity.conf"
    conf_text = "\n".join([
        "# Auto-generated by factory_provision_build_flash.py",
        f'CONFIG_BT_DIS_SERIAL_NUMBER_STR="{serial}"',
        "",
    ])
    conf_path.parent.mkdir(parents=True, exist_ok=True)
    conf_path.write_text(conf_text, encoding="utf-8")
    return conf_path


def write_size_conf_fragment(project_root: Path) -> Path:
    conf_path = project_root / "build" / "factory_size_optimized.conf"
    conf_text = "\n".join([
        "# Auto-generated by factory_provision_build_flash.py",
        "# Size-optimized profile for production factory builds",
        "CONFIG_SIZE_OPTIMIZATIONS=y",
        "CONFIG_LTO=y",
        "CONFIG_DEBUG=n",
        "CONFIG_ASSERT=n",
        "CONFIG_THREAD_NAME=n",
        "CONFIG_STACK_CANARIES=n",
        "CONFIG_LOG_DEFAULT_LEVEL=2",
        "CONFIG_CBPRINTF_NANO=y",
        "CONFIG_NEWLIB_LIBC_NANO=y",
        "",
    ])
    conf_path.parent.mkdir(parents=True, exist_ok=True)
    conf_path.write_text(conf_text, encoding="utf-8")
    return conf_path


def write_manifest(manifest_dir: Path, payload: Dict[str, Any], hw_id: str, serial: str, serial_seq: int, thing_name: str) -> Path:
    manifest_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    manifest_path = manifest_dir / f"provision_{serial}_{timestamp}.json"

    manifest = {
        "timestamp_utc": timestamp,
        "hw_id": hw_id,
        "serial": serial,
        "serial_seq": serial_seq,
        "thing_name": thing_name,
        "provision_response": payload,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Provision device identity, build, and flash AutoWatering firmware.")
    parser.add_argument("--api-base-url", required=True, help="API base URL (e.g. https://<id>.execute-api.<region>.amazonaws.com)")
    parser.add_argument("--factory-token", required=True, help="Factory provisioning token")
    parser.add_argument("--hw-id", required=True, help="Hardware ID used for provisioning")
    parser.add_argument("--board", default="arduino_nano_33_ble", help="Zephyr board (default: arduino_nano_33_ble)")
    parser.add_argument("--build-dir", default="build_factory", help="Build directory name (default: build_factory)")
    parser.add_argument("--sysbuild", action="store_true", help="Build with --sysbuild")
    parser.add_argument("--pristine", action="store_true", help="Build with --pristine")
    parser.add_argument(
        "--size-optimized",
        action="store_true",
        help="Enable size-optimized app profile (LTO, -Os, reduced debug/log footprint)",
    )
    parser.add_argument("--skip-build", action="store_true", help="Skip west build")
    parser.add_argument("--skip-flash", action="store_true", help="Skip west flash")
    parser.add_argument("--flash-runner", default="bossac", help="west flash runner (default: bossac)")
    parser.add_argument("--openocd-bin", default="", help="Path to openocd binary (used with openocd-stlink)")
    parser.add_argument("--openocd-interface-cfg", default="interface/stlink.cfg", help="OpenOCD interface cfg file")
    parser.add_argument("--openocd-target-cfg", default="target/nordic/nrf52.cfg", help="OpenOCD target cfg file")
    parser.add_argument("--openocd-transport", default="swd", help="OpenOCD transport (default: swd)")
    parser.add_argument("--openocd-speed-khz", type=int, default=1000, help="OpenOCD adapter speed in kHz")
    parser.add_argument("--operator", default="", help="Optional operator name for metadata")
    parser.add_argument("--batch", default="", help="Optional batch label for metadata")

    args = parser.parse_args()

    project_root = Path(__file__).resolve().parents[1]
    build_dir = project_root / args.build_dir

    metadata: Dict[str, Any] = {
        "source": "factory_build_flash",
    }
    if args.operator:
        metadata["operator"] = args.operator
    if args.batch:
        metadata["batch"] = args.batch

    print("[STEP] Provisioning identity from backend...")
    payload = call_provision(args.api_base_url, args.factory_token, args.hw_id, metadata)
    serial, serial_seq, thing_name = validate_provisioning_payload(payload)
    print(f"[OK] Provisioned hw_id={args.hw_id} serial={serial} serial_seq={serial_seq} thing_name={thing_name}")

    conf_fragment = write_conf_fragment(project_root, serial)
    print(f"[OK] Generated conf fragment: {conf_fragment}")

    extra_conf_files = [conf_fragment]
    if args.size_optimized:
        size_conf_fragment = write_size_conf_fragment(project_root)
        extra_conf_files.append(size_conf_fragment)
        print(f"[OK] Generated size profile fragment: {size_conf_fragment}")

    extra_conf_arg = ";".join(str(p) for p in extra_conf_files)

    if not args.skip_build:
        print("[STEP] Building firmware...")
        cmd = ["west", "build", "-b", args.board, "-d", str(build_dir)]
        if args.sysbuild:
            cmd.append("--sysbuild")
        if args.pristine:
            cmd.append("--pristine")
        if args.sysbuild:
            cmd.extend([
                "--",
                f"-DAutoWatering_EXTRA_CONF_FILE={extra_conf_arg}",
                f"-Dapp_EXTRA_CONF_FILE={extra_conf_arg}",
            ])
        else:
            cmd.extend(["--", f"-DEXTRA_CONF_FILE={extra_conf_arg}"])
        run_command(cmd, project_root)
        print("[OK] Build complete.")

    if not args.skip_flash:
        print("[STEP] Flashing firmware...")
        flash_runner = args.flash_runner.lower()
        if flash_runner in {"openocd-stlink", "openocd_stlink", "stlink", "openocd"}:
            flash_with_openocd_stlink(
                project_root=project_root,
                build_dir=build_dir,
                openocd_bin=args.openocd_bin,
                interface_cfg=args.openocd_interface_cfg,
                target_cfg=args.openocd_target_cfg,
                transport=args.openocd_transport,
                speed_khz=args.openocd_speed_khz,
            )
        else:
            bossac_wrapper = project_root / "tools" / "bossac_wrapper.bat"
            cmd = [
                "west",
                "flash",
                "--build-dir",
                str(build_dir),
                "-r",
                args.flash_runner,
            ]
            if flash_runner == "bossac":
                cmd.extend(["--bossac", str(bossac_wrapper)])
            run_command(cmd, project_root)
        print("[OK] Flash complete.")

    manifest_path = write_manifest(project_root / "build" / "factory-manifests", payload, args.hw_id, serial, serial_seq, thing_name)
    print(f"[OK] Wrote provisioning manifest: {manifest_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[ERROR] {exc}")
        raise SystemExit(1)
