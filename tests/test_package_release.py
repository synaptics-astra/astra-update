# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Synaptics Incorporated

from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from tools import package_release


class PackageReleaseTests(unittest.TestCase):
    def test_cli_and_wheel_archives_by_host(self):
        cases = (
            ("macos", "arm64", ("x86_64", "arm64")),
            ("macos", "x86_64", ()),
            ("linux", "arm64", ()),
            ("windows", "AMD64", ()),
        )
        for platform, machine, cross_archs in cases:
            with self.subTest(platform=platform, machine=machine), tempfile.TemporaryDirectory() as tmp:
                configured = {}
                wheel_calls = []
                exe = ".exe" if platform == "windows" else ""

                def run(*args):
                    if args[0] == "pipx":
                        wheel_calls.append(args)
                        wheelhouse = args[-1]
                        wheelhouse.mkdir()
                        archs = args[args.index("--archs") + 1].split(",") if "--archs" in args else [machine]
                        for arch in archs:
                            (wheelhouse / f"example-{arch}.whl").write_bytes(arch.encode())
                    elif "-B" in args:
                        build_dir = args[args.index("-B") + 1]
                        arch = next(
                            (arg.split("=", 1)[1] for arg in args
                             if isinstance(arg, str) and arg.startswith("-DCMAKE_OSX_ARCHITECTURES=")),
                            None,
                        )
                        configured[build_dir.name] = arch
                        build_dir.mkdir()
                        (build_dir / "release-cli-arch.txt").write_text(arch or machine)
                    elif args[1] == "--install":
                        arch = configured[args[2].name] or machine
                        stage = args[-1]
                        stage.mkdir()
                        for target in package_release.CLI_TARGETS:
                            (stage / f"{target}{exe}").write_bytes(arch.encode())

                with patch.object(package_release, "machine", return_value=machine), \
                        patch.object(package_release, "run", side_effect=run):
                    assets = package_release.package_release(Path(tmp) / "assets", platform)

                self.assertEqual(configured, {f"build_{arch}": arch for arch in cross_archs}
                                 if cross_archs else {"build": None})
                self.assertEqual(len(wheel_calls), 1)
                if not cross_archs:
                    self.assertNotIn("--archs", wheel_calls[0])
                wheel_archs = cross_archs or (machine,)
                wheel_assets = {asset.name: asset for asset in assets if asset.name.endswith(".whl.zip")}
                self.assertEqual(set(wheel_assets), {f"example-{arch}.whl.zip" for arch in wheel_archs})
                for arch in wheel_archs:
                    wheel_name = f"example-{arch}.whl"
                    with zipfile.ZipFile(wheel_assets[f"{wheel_name}.zip"]) as archive:
                        self.assertEqual(archive.namelist(), [wheel_name])
                        self.assertEqual(archive.read(wheel_name), arch.encode())
                self.assertEqual(len(assets), len(wheel_archs) + 2 * max(1, len(cross_archs)))
                cli_archs = cross_archs or (machine if not exe else "x86_64",)
                for arch in cli_archs:
                    for target in package_release.CLI_TARGETS:
                        suffix = ".zip" if exe else ".tar.gz"
                        asset, = [a for a in assets if a.name.startswith(f"{target}-")
                                  and a.name.endswith(f"-{platform}-{arch}{suffix}")]
                        if exe:
                            with zipfile.ZipFile(asset) as archive:
                                self.assertEqual(archive.read(f"{target}{exe}"), machine.encode())
                        else:
                            with tarfile.open(asset) as archive:
                                self.assertEqual(archive.getnames(), [target])
                                member = archive.extractfile(target)
                                assert member is not None
                                self.assertEqual(member.read(), arch.encode())
