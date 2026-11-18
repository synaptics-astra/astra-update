# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Synaptics Incorporated

"""Build and archive the release assets for the current host platform."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import zipfile


ROOT = Path(__file__).resolve().parents[1]
CLI_TARGETS = ("astra-update", "astra-boot")
PLATFORMS = {"win32": "windows", "darwin": "macos", "linux": "linux"}


def run(*args: str | Path) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def build_cli(build_dir: Path, stage: Path) -> str:
    run(
        "cmake", "-S", ROOT, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=OFF", "-DASTRA_BUILD_TESTS=OFF", "-DASTRA_BUILD_PYTHON_BINDINGS=OFF",
    )
    run("cmake", "--build", build_dir, "--config", "Release", "--target", *CLI_TARGETS)
    run("cmake", "--install", build_dir, "--config", "Release",
        "--component", "release-cli", "--prefix", stage)
    arch = (build_dir / "release-cli-arch.txt").read_text(encoding="utf-8").strip().lower()
    arch = arch.replace(";", "_")
    return {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64", "win32": "x86"}.get(arch, arch)


def zip_file(source: Path, archive: Path) -> None:
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        output.write(source, arcname=source.name)


def package_release(output_dir: Path, platform: str) -> list[Path]:
    if output_dir.exists() and (not output_dir.is_dir() or any(output_dir.iterdir())):
        raise ValueError(f"Output directory must be empty or new: {output_dir}")
    with (ROOT / "pyproject.toml").open("rb") as metadata:
        version = tomllib.load(metadata)["project"]["version"]

    with tempfile.TemporaryDirectory(prefix="astra-release-") as tmp:
        work = Path(tmp)
        stage = work / "bin"
        wheelhouse = work / "wheels"
        archives = work / "archives"
        archives.mkdir()
        arch = build_cli(work / "build", stage)
        for target in CLI_TARGETS:
            name = f"{target}-{version}-{platform}-{arch}"
            if platform == "windows":
                zip_file(stage / f"{target}.exe", archives / f"{name}.zip")
            else:
                with tarfile.open(archives / f"{name}.tar.gz", "w:gz") as archive:
                    archive.add(stage / target, arcname=target)

        run("pipx", "run", "--spec", "cibuildwheel>=4,<5", "cibuildwheel",
            "--platform", platform, "--output-dir", wheelhouse)
        wheels = sorted(wheelhouse.glob("*.whl"))
        if not wheels:
            raise RuntimeError("cibuildwheel produced no wheels; check the build selection.")
        for wheel in wheels:
            zip_file(wheel, archives / f"{wheel.name}.zip")

        output_dir.mkdir(parents=True, exist_ok=True)
        return [Path(shutil.copy2(archive, output_dir)) for archive in sorted(archives.iterdir())]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release-assets",
                        help="New or empty destination (default: release-assets/ in the checkout).")
    args = parser.parse_args()
    platform = PLATFORMS.get(sys.platform)
    if platform is None:
        parser.error(f"Unsupported platform: {sys.platform}")
    for tool in ("cmake", "pipx"):
        if shutil.which(tool) is None:
            parser.error(f"{tool} is required and must be on PATH.")
    try:
        assets = package_release(args.output_dir.resolve(), platform)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")
    print("Release assets (upload these manually):", flush=True)
    for asset in assets:
        print(f"  {asset}")


if __name__ == "__main__":
    main()
