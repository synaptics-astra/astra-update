# Building astra-update wheels manually (Windows, macOS, Linux)

Compiled wheels must be built on the OS they target. Run the steps below once on each OS and collect the resulting `.whl` files.

The repository is configured to build the `_astra_update` pybind11 extension
and package it with the `astra_update` Python sources. Python 3.11 through 3.14
are included, matching the `requires-python` metadata. The native dependencies
libusb and yaml-cpp are built statically; Linux wheels leave `libudev.so.1` as
a system runtime dependency.

The wheel backend and cibuildwheel matrix are configured in `pyproject.toml`.
CMake installs the Python sources and native module into the wheel. The
build-system fetches pybind11 through CMake, so it is not a separate Python
build dependency. The existing macOS CMake configuration resolves the active
SDK; use `CMAKE_OSX_SYSROOT` only when selecting a specific SDK.

---

## Method A (recommended): cibuildwheel on each OS

cibuildwheel builds every Python version listed in `pyproject.toml`, repairs
the platform wheel, and runs an import test. The command is the same on every OS.

Use cibuildwheel 4.x for the configured Python 3.11 through 3.14 wheel matrix.

```bash
pipx run --spec "cibuildwheel>=4,<5" cibuildwheel --output-dir wheelhouse
```

(`pip install "cibuildwheel>=4,<5"` then `cibuildwheel --output-dir wheelhouse` also works.)

### Linux

Prerequisites: Docker (running) and `pipx`.

```bash
cd astra-update
pipx run --spec "cibuildwheel>=4,<5" cibuildwheel --platform linux --output-dir wheelhouse
```

- The build runs inside a manylinux container, so wheels work on most distros.
- The first run downloads the container image.
- Output: `wheelhouse/astra_update-*-manylinux_2_28_x86_64.whl`, one per Python version.

### macOS

Prerequisites: Xcode Command Line Tools (`xcode-select --install`) and `pipx`.

```bash
cd astra-update
pipx run --spec "cibuildwheel>=4,<5" cibuildwheel --platform macos --output-dir wheelhouse
```

- On an Apple Silicon Mac this builds `arm64` wheels natively.
- On an Intel Mac it builds `x86_64` wheels.
- To get both from one Apple Silicon Mac, run again with `CIBW_ARCHS_MACOS=x86_64`. This cross-compiles; test the resulting wheels on a compatible Intel Mac if possible.
- This configuration produces separate architecture wheels, not universal2 wheels.

### Windows

Prerequisites: Visual Studio 2019 or newer (or Build Tools) with the
"Desktop development with C++" workload and `pipx`. Run from a normal
PowerShell.

```powershell
cd astra-update
pipx run --spec "cibuildwheel>=4,<5" cibuildwheel --platform windows --output-dir wheelhouse
```

- Output: `wheelhouse\astra_update-*-win_amd64.whl`.
- Windows wheels target AMD64 only; 32-bit x86 wheels are not built.
- For ARM64 wheels, set `$env:CIBW_ARCHS_WINDOWS = "ARM64"` first (cross-compile, no tests).

---

## Method B (fallback): build and repair by hand

Use this if cibuildwheel doesn't work for you. Repeat for each Python version you want to support (install them with `uv`, pyenv or python.org, or use the `py -3.12` launcher on Windows).

### Linux

```bash
sudo apt install libudev-dev            # Debian/Ubuntu
python3 -m venv .venv-build && . .venv-build/bin/activate
pip install build auditwheel
python -m build --wheel --outdir dist
auditwheel repair --exclude libudev.so.1 -w wheelhouse dist/*.whl
```

The resulting wheel's `manylinux` tag depends on the glibc of the machine you built on. Build on an old distro (for example Ubuntu 20.04 or 22.04) so the wheel runs on newer ones. The Docker route in Method A avoids this problem.

### macOS

```bash
python3 -m venv .venv-build && source .venv-build/bin/activate
pip install build delocate
export MACOSX_DEPLOYMENT_TARGET=11.0
python -m build --wheel --outdir dist
delocate-wheel -w wheelhouse -v dist/*.whl
```

### Windows (PowerShell)

```powershell
py -3.12 -m venv .venv-build
.\.venv-build\Scripts\Activate.ps1
pip install build delvewheel
python -m build --wheel --outdir dist
delvewheel repair -w wheelhouse (Get-ChildItem dist\*.whl)
```

---

## Verify every wheel

Do this in a clean virtualenv, ideally on a machine that has no build tools installed.

```bash
python -m venv /tmp/wheeltest && . /tmp/wheeltest/bin/activate   # Windows: .\wheeltest\Scripts\Activate.ps1
pip install wheelhouse/<wheel-for-this-python>.whl
python -c "import astra_update; print(astra_update.__file__)"
```

Then test against a real board in USB boot mode.

Check the metadata before sharing:

```bash
pip install twine
twine check wheelhouse/*.whl
```

---

## Collect and share

1. Copy the `wheelhouse/` contents from each OS into one folder.
2. Expected set per Python version: `manylinux_2_28_x86_64`, `macosx_*_arm64`, `macosx_*_x86_64`, `win_amd64`.
3. Share via a GitHub Release on your fork, or upload to PyPI with `twine upload wheelhouse/*.whl` (use a distinct package name if this isn't the official project).

---

## Runtime requirements (not packaged in the wheel)

- **Windows:** users must install the WinUSB driver for the Astra device.
- **Linux:** users need `libudev.so.1` and the udev rules from `config/`, or must run as root.

Document both in the wheel's README.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Wheel is missing Python files or the native module | Check the CMake `install()` rules in `lib/CMakeLists.txt` |
| `libudev.h` not found | Install `libudev-dev` (Debian/Ubuntu) or `systemd-devel` (Fedora/RHEL/manylinux) |
| Linux wheel won't upload to PyPI (`linux_x86_64` tag) | It wasn't repaired; use Method A or run `auditwheel repair` |
| `ImportError: DLL load failed` on Windows | Run `delvewheel repair`, and check for a missing Visual C++ runtime |
| Wheel works for one Python version only | Expected with pybind11: one wheel per CPython version (nanobind can build abi3 wheels) |
| Docker permission denied (Linux) | Add your user to the `docker` group, or run with `sudo` |

---

## Later: CI ticket

When the CI ticket is filed upstream, this file's `[tool.cibuildwheel]` config can be reused unchanged. Use `pypa/cibuildwheel@v4` on a Linux, macOS and Windows runner matrix. For Intel macOS use the `macos-15-intel` runner label, and note GitHub plans to end Intel macOS runner support in Fall 2027.
