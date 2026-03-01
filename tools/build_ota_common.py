#!/usr/bin/env python3
"""
Build a common OTA artifact (same image for all devices).

This script builds firmware without per-device personalization and exports
OTA artifacts to build/ota-artifacts/latest.

Usage:
  python tools/build_ota_common.py
  python tools/build_ota_common.py --sysbuild --pristine
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import List


def run_command(cmd: List[str], cwd: Path) -> None:
    print("[RUN] " + " ".join(cmd))
    result = subprocess.run(cmd, cwd=str(cwd), text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(cmd)}")


def collect_artifacts(build_dir: Path) -> List[Path]:
    candidates = [
        build_dir / "zephyr" / "zephyr.bin",
        build_dir / "zephyr" / "zephyr.hex",
        build_dir / "zephyr" / "zephyr.elf",
        build_dir / "zephyr" / "zephyr.signed.bin",
        build_dir / "zephyr" / "zephyr.signed.hex",
        build_dir / "AutoWatering" / "zephyr" / "zephyr.bin",
        build_dir / "AutoWatering" / "zephyr" / "zephyr.hex",
        build_dir / "AutoWatering" / "zephyr" / "zephyr.elf",
        build_dir / "AutoWatering" / "zephyr" / "zephyr.signed.bin",
        build_dir / "AutoWatering" / "zephyr" / "zephyr.signed.hex",
    ]
    return [p for p in candidates if p.exists()]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build common OTA binary/artifacts")
    parser.add_argument("--board", default="arduino_nano_33_ble")
    parser.add_argument("--build-dir", default="build_ota_common")
    parser.add_argument("--sysbuild", action="store_true", default=True)
    parser.add_argument("--pristine", action="store_true", default=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parents[1]
    build_dir = project_root / args.build_dir

    cmd = ["west", "build", "-b", args.board, "-d", str(build_dir)]
    if args.sysbuild:
        cmd.append("--sysbuild")
    if args.pristine:
        cmd.append("--pristine")

    if args.dry_run:
        print("[DryRun] " + " ".join(cmd))
        return 0

    run_command(cmd, project_root)

    artifacts = collect_artifacts(build_dir)
    if not artifacts:
        raise RuntimeError("No OTA artifacts found after build")

    out_dir = project_root / "build" / "ota-artifacts" / "latest"
    out_dir.mkdir(parents=True, exist_ok=True)

    copied = []
    for artifact in artifacts:
        target = out_dir / artifact.name
        shutil.copy2(artifact, target)
        copied.append(target)

    manifest = {
        "generated_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "build_dir": str(build_dir),
        "artifacts": [str(p) for p in copied],
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print("[OK] OTA artifacts exported to:")
    for p in copied:
        print(f"  - {p}")
    print(f"  - {out_dir / 'manifest.json'}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[ERROR] {exc}")
        raise SystemExit(1)
