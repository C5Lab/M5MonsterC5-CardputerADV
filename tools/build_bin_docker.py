#!/usr/bin/env python3
"""
Build ESP32S3 firmware inside the official ESP-IDF docker image without
touching the working tree. Creates a temporary copy of the repo, builds in
the container, and then copies artifacts to tools/docker_bin_output.

Usage:
    python tools/build_bin_docker.py [--image espressif/idf:v6.0-dev]
"""

import argparse
import tempfile
import os
import shutil
import subprocess
import sys
from pathlib import Path
import re


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--image",
        default="espressif/idf:v6.0-beta1",
        help="Docker image to use (default: espressif/idf:v6.0-beta1)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    workflow_script = repo_root / ".github" / "scripts" / "container_build.sh"

    if not workflow_script.is_file():
        print(f"Missing build script: {workflow_script}", file=sys.stderr)
        return 1

    tmpdir = Path(tempfile.mkdtemp(prefix="M5MonsterC5-CardputerADV-build-"))
    workspace = tmpdir / "src"
    print(f"Creating temporary workspace at {workspace}")

    try:
        shutil.copytree(
            repo_root,
            workspace,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(
                ".git",
                "__pycache__",
                "build",
                "managed_components",
                "binaries-esp32s3",
            ),
        )
    except Exception as exc:
        print(f"Failed to prepare workspace: {exc}", file=sys.stderr)
        shutil.rmtree(tmpdir, ignore_errors=True)
        return 1

    cmd = [
        "docker",
        "run",
        "--rm",
        "-it",
        "-v",
        f"{workspace.as_posix()}:/project",
        "-w",
        "/project",
        "-e",
        "IDF_PY_FLAGS=--preview",
        args.image,
        "bash",
        "-lc",
        "python -m pip install --no-cache-dir esptool && "
        "bash .github/scripts/container_build.sh --no-docker",
    ]

    print("Running build inside docker:")
    print(" ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        print("docker not found. Please install Docker and ensure it's on PATH.", file=sys.stderr)
        shutil.rmtree(tmpdir, ignore_errors=True)
        return 1
    except subprocess.CalledProcessError as exc:
        shutil.rmtree(tmpdir, ignore_errors=True)
        return exc.returncode

    # Copy artifacts back to a stable location in the repo
    src_bins = workspace / "binaries-esp32s3"
    version = None
    try:
        text = (workspace / "main" / "main.c").read_text(encoding="utf-8")
        match = re.search(r'JANOS_ADV_VERSION\s*"([^"]+)"', text)
        if match:
            version = match.group(1)
    except OSError:
        version = None

    if src_bins.is_dir() and version:
        base_bin = src_bins / "M5MonsterC5-CardputerADV.bin"
        if base_bin.is_file():
            shutil.copy2(base_bin, src_bins / "M5MonsterC5-CardputerADV-launcher.bin")
            shutil.copy2(
                base_bin, src_bins / f"M5MonsterC5-CardputerADV-launcher-{version}.bin"
            )
            shutil.copy2(
                base_bin, src_bins / f"M5MonsterC5-CardputerADV-{version}.bin"
            )
        full_bin = src_bins / "M5MonsterC5-CardputerADV-full.bin"
        if full_bin.is_file():
            shutil.copy2(
                full_bin, src_bins / f"M5MonsterC5-CardputerADV-full-{version}.bin"
            )

    dest_bins = repo_root / "tools" / "docker_bin_output"
    dest_bins.mkdir(parents=True, exist_ok=True)
    copied = []
    if src_bins.is_dir():
        for item in src_bins.iterdir():
            if not item.is_file():
                continue
            if item.name.lower() == "readme.md":
                continue  # skip local README file from being treated as an artifact
            shutil.copy2(item, dest_bins / item.name)
            copied.append(item.name)

    shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nBuild finished.")
    if copied:
        print(f"Copied artifacts to: {dest_bins}")
        print("Files:")
        for name in copied:
            print(f"  - {name}")
    else:
        print("No artifacts were found to copy.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
