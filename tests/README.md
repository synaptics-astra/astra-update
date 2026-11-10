# Library unit tests

This suite implements phases 1-5 of the library-wide test plan: the
foundation, hardware-independent core APIs, flash-image parsing and command
generation, scripted fastboot/USB request flows, and SL16xx device requests
over a fake USB transport. It does not enumerate USB devices, flash storage,
or instantiate a hardware-backed device manager. Commands and payloads are
captured by fakes, never sent to real devices.

## Running

From the repository root:

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --target unit-tests --parallel 2
ctest --test-dir build -C Debug -L unit --output-on-failure
```

For single-configuration generators, configure a separate Release build with
`-DCMAKE_BUILD_TYPE=Release`. Visual Studio supports Debug and Release in the
same build tree. YAML linking follows the actual build configuration.

Run just one component with `ctest -R "^image$"` or build its `image-test`
target. Each executable accepts `--list` or an exact case name:

```powershell
.\build\tests\Debug\image-test.exe --list
.\build\tests\Debug\image-test.exe copies-share-cursor-and-outlive-source
```

On single-configuration generators, executables are in `build/tests`.
Assertions remain active in Release. All suites have the `unit` label; phases
1-2 also have `core`, and phase 3 has `flash`. Run just phase 3 with
`ctest --test-dir build -C Debug -L flash --output-on-failure`.
Phase 4 uses the `simulated-device` label:
`ctest --test-dir build -C Debug -L simulated-device --output-on-failure`.
The phase 5 SL16xx device-flow suite uses the same label and can be run with
`ctest --test-dir build -C Debug -R "^sl16xx-device$" --output-on-failure`.
Each suite has a 30-second deadlock guard. Case failures print their name and
assertion location, and do not prevent the remaining cases from running.

`-DBUILD_TESTING=OFF` omits the test targets and registration. Use a fresh
build directory when checking this option: old CTest files can remain in an
already-configured build tree.

## Behavior-to-test matrix

There are 692 named cases across 19 executables, including 327 phase 3,
59 phase 4, and 10 phase 5 SL16xx cases. Table-driven cases also make
multiple assertions across supported enum combinations.

| Suite | Cases | Behaviors asserted |
|---|---:|---|
| [test-support](test_support_test.cpp) | 5 | Always-active checks, expected/wrong/missing exceptions, failed-case continuation, duplicate names, list/exact selection, unknown arguments, visible skips, fixture ownership and cleanup. |
| [image](image_test.cpp) | 11 | Inferred/explicit name, path and all payload types; unloaded/missing/empty/directory/unreadable inputs; diagnostic messages; binary chunking, zero read, clamp and EOF; truncation/short read; reload; copy/shared cursor, surviving source destruction, handle detachment, move and vector reallocation. |
| [boot-image-collection](boot_image_collection_test.cpp) | 116 | The original 30 regressions; missing/empty/file paths, valid versus invalid/manifest-less children, exact ID lookup and miss, primary VID/PID results; all secure-boot/layout/DDR combinations; boot memory <= target memory across all five sizes, multiple eligible sizes, exact-chip priority and series fallback; invalid target layout rejection; independent filter rejection; board/case/omission constraints; malformed and non-series chip inputs; legacy reload/direct-directory/duplicate-ID behavior. |
| [boot-image](boot_image_test.cpp) | 76 | Required fields/getters and optional defaults; every memory/DDR value; normalization; transport, console, variant and boot stage; short/full version banners; primary/SysMgr/fastboot IDs and incomplete pairs; NAND/uEnv flags; Gen2/Gen3 final-image choices; both Linux layouts, precedence and incomplete pairs; relative nested payload names and manifest exclusion; every missing required field, malformed/wrong-shaped/typed YAML, invalid fields and unreadable manifests. |
| [device-api](device_api_test.cpp) | 39 | All valid status names and response variants; series names/completion commands; boot-stage parsing; enum string/default conversions; response fields/copies/wrong-accessor exceptions; FlashImage value getters and version-change hook; public manager version. No live device or manager is created. |
| [log](log_test.cpp) | 23 | Every level, invalid level, threshold truth table, record/timestamp shape; closed/filtered formatting; enabled stream overloads/manipulators/buffer reset; trace gating; file append/flush/close/reopen, stdout, open failure/recovery; concurrent complete records and close during active writing. |
| [console](console_test.cpp) | 14 | Buffered/logged binary bytes; prompt before/after wait, trailing whitespace, prompt consumption, non-prompts; shutdown before/during wait, multiple waiters/repeated shutdown; legacy split-prompt behavior. |
| [utils](utils_test.cpp) | 7 | Little-endian byte representation including high bits; existing/unique generated temp directories; concurrent creation. Cleanup only removes directories returned by the creation function. |
| [flash-image](flash_image_test.cpp) | 67 | Factory types, defaults, paths/fallback, copied input config, CLI precedence, normalization, every memory/DDR value, Coralboard defaults, reset options, type autodetection, ordered manifest images and all SPI overrides; missing/unreadable/malformed/empty/scalar manifests, partial parsing and diagnostics; end-to-end payload and command checks. |
| [tag-detection](tag_detection_test.cpp) | 51 | SL1620/SL1640/SL1680 and every SL261x chip, board-specific DDR, explicit chip compatibility and mismatch warnings, malformed/unsupported/case-sensitive tags, missing directories, board suffix parsing, coherent results with multiple compatible tags. |
| [uboot-validation](uboot_validation_test.cpp) | 73 | Decimal/hexadecimal/$filesize numeric tokens, whitespace/sign/injection/NUL/non-ASCII rejection; filename character grammar and exact 254/255/256-byte boundaries. Validation is lexical, not a numeric range check. |
| [emmc-flash-image](emmc_flash_image_test.cpp) | 27 | Immediate image discovery, metadata inclusion, unordered payload set, exact l2emmc command, trailing-slash paths, comma/CRLF/blank/EOF parsing, erase/format filtering, final-image selection; directory/list/open/name failures with error logs; TAG/default precedence, version changes and repeated-load/list characterization. |
| [spi-flash-image](spi_flash_image_test.cpp) | 76 | Golden legacy/modern/unknown-version commands across chip families, ordered single/multiple images, standalone/config/image maps, all eight overrides and each invalid field, filename/path/map failures with error logs, version rebuilding and override retention, repeated loads. |
| [nand-flash-image](nand_flash_image_test.cpp) | 33 | Standalone/config/image/default/custom payloads, first recognized map, exact usbload/m2nand command, valid/default/invalid read addresses, unsafe filenames and missing paths with error logs, TAG/default precedence, factory integration, version changes and repeated loads. |
| [fastboot-device](fastboot_device_test.cpp) | 34 | Open/close/reopen and disconnect events; getvar/oem/reboot framing, INFO/OKAY/FAIL/short/unknown responses, timeout forwarding and 63/64/65-byte commands; serial probe ownership; exact-size download negotiation, empty files, 4-MiB chunk boundaries, payload/progress, short/failed I/O, disconnect, malformed DATA lengths, changing/missing files and final ACK failures. |
| [usb-callback-worker](usb_callback_worker_test.cpp) | 5 | Queued event-data ownership and callback/action thread identity; FIFO actions, nested queueing outside the lock, restartable/idempotent startup and shutdown, drain-before-join and ignored empty actions. |
| [usb-transport](usb_transport_test.cpp) | 5 | Handle-free completion observation through the shared Windows/POSIX open gate, VID/PID selectivity, port identity across devices/re-enumeration, and default/cleared observer behavior. No live USB device is opened. |
| [astra-device-impl](astra_device_impl_test.cpp) | 20 | Generated/supplied uEnv and UUID reuse; boot/update list construction, Linux boot-only/update final-image choice, request name normalization and payload selection, status sequences, final-image and size-request completion gates, send/missing-image failures, suppressed events, keep-serving behavior, idle-timeout policy, prompt thread shutdown and idempotent close. |
| [sl16xx-device](sl16xx_device_test.cpp) | 10 | Public boot/close lifecycle; SL16xx interrupt request decoding, padding/truncated packet handling, ordered back-to-back image requests, binary console reads/writes, image size header and payload, `07_IMAGE` size handshake/status suppression, missing-image failure, no-device/cancel/error disconnects, miniloader reconnect handling, and concurrent-close/completion-wait wakeup. |

Fixtures own unique temporary directories and restore permissions, log stores,
streams and workers on failure. Waiters are explicitly woken and joined;
tests use bounded future waits rather than sleeps. Text-mode log assertions
respect native newline translation; binary image assertions preserve bytes.

Unreadable-file tests use exclusive file locks on Windows and restricted
permissions on POSIX. They check the restriction independently before
calling production code. If the POSIX process can bypass the restriction
(for example, running as root), those cases report an explicit `SKIP` reason.
Use `ctest -V` or the executable directly to see case-level passes/skips.
Skips are also retained in CTest's `LastTest.log`.

## Coverage

GCC instrumentation is opt-in and applies only to project code and tests,
not the external libusb/yaml-cpp builds:

```sh
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DASTRA_ENABLE_COVERAGE=ON
cmake --build build-coverage --target unit-tests --parallel 2
ctest --test-dir build-coverage -L unit --output-on-failure
gcovr --root . --filter '^lib/' --filter '^include/' --exclude-unreachable-branches --exclude-throw-branches --print-summary --xml-pretty --output build-coverage/coverage.xml build-coverage
```

`gcovr` is reporting tooling, not a runtime dependency. Install it separately
if absent. GCC's built-in `gcov --json-format --branch-probabilities` can
also report coverage without gcovr. Unsupported compilers reject
`ASTRA_ENABLE_COVERAGE=ON` explicitly; use a native coverage tool for MSVC.
Use a fresh coverage directory when measuring a baseline to avoid old counts.

The [CI workflow](../.github/workflows/unit-tests.yml) runs Debug/Release unit
tests and testing-disabled library builds on Windows, Linux and macOS, and
publishes a project-only GCC coverage report. It establishes a baseline;
there is no aggregate percentage gate yet because device and
transport coverage belongs to later phases.

### Phase 1-2 measured baseline

Measured with GCC/gcov 13.3 on Ubuntu 24.04 (WSL), using a clean set of
execution counts from all 273 cases with no skipped prerequisites. Native
Windows MSVC Debug and Release also passed all cases; both CLI applications
built, the four support/concurrency suites passed five repeated Debug runs,
and a fresh testing-disabled library build had zero registered tests.
The GitHub Actions matrix is added but has not been executed here; macOS
results remain pending.

| Core implementation | Executed lines | Line coverage |
|---|---:|---:|
| [image.cpp](../lib/image.cpp) | 42/46 | 91.30% |
| [astra_boot_image.cpp](../lib/astra_boot_image.cpp) | 173/174 | 99.43% |
| [boot_image_collection.cpp](../lib/boot_image_collection.cpp) | 56/56 | 100% |
| [astra_log.cpp](../lib/astra_log.cpp) | 102/102 | 100% |
| [astra_console.cpp](../lib/astra_console.cpp) | 45/45 | 100% |
| [utils.cpp](../lib/utils.cpp) (Linux implementation) | 6/7 | 85.71% |

Merged project-owned Linux library/header coverage is **584/4533 lines
(12.88%)**, including the untested flash/device/transport implementations.
This is a library-wide starting baseline, not a claim of full coverage.
Public header lines are merged across translation units, rather than summing
duplicate inline definitions. Only Linux-compiled code is represented;
Windows/macOS adapter coverage cannot be inferred from these numbers.

The local numbers use GCC JSON reports and count all executable source
lines. The CI gcovr report also supplies branch coverage with unreachable
and exception-only branches excluded. Do not equate raw gcov branch
counts (which include compiler-generated exception paths) with decision
coverage or use this baseline as a percentage gate without reviewing gaps.

### Phase 3 measured baseline

All **600 cases across 14 executables** passed on Windows MSVC Debug/Release
and GCC 13.3 Debug on Ubuntu 24.04 (WSL). The Linux run had no skips and used
fresh execution counts. The six flash suites also passed three repeated Windows
Debug runs before the final additional cases. No hardware was accessed.

| Flash implementation | Executed lines | Line coverage |
|---|---:|---:|
| [flash_image.cpp](../lib/flash_image.cpp) | 215/236 | 91.10% |
| [emmc_flash_image.cpp](../lib/emmc_flash_image.cpp) | 72/74 | 97.30% |
| [spi_flash_image.cpp](../lib/spi_flash_image.cpp) | 115/115 | 100% |
| [nand_flash_image.cpp](../lib/nand_flash_image.cpp) | 59/61 | 96.72% |
| [flash_image.hpp](../include/flash_image.hpp) | 32/32 | 100% |

Merged project-owned Linux library/header coverage is now **1093/4533 lines
(24.11%)**, superseding the initial baseline. The same GCC JSON merging method
is used; these figures are line coverage, not branch coverage or hardware
qualification. The remaining unexecuted flash lines are the unused internal
`FlashImageTypeToString()` helper and generic exception handlers. Deterministic
filesystem/allocation fault injection is deferred rather than introducing
races or production-only test switches. Native macOS and remote CI results
remain pending.

### Phase 4 measured baseline

All **659 cases across 17 executables** passed on Windows MSVC Debug/Release
and GCC 13.3 Debug on Ubuntu 24.04 (WSL), with no skips. The simulated-device
suites were repeated five times during validation; final additions also
passed the full suite. No real USB devices were accessed.

| Shared protocol/request implementation | Executed lines | Line coverage |
|---|---:|---:|
| [fastboot_device.cpp](../lib/fastboot_device.cpp) | 207/218 | 94.95% |
| [usb_device.cpp](../lib/usb_device.cpp) | 56/58 | 96.55% |
| [astra_device_impl.cpp](../lib/astra_device_impl.cpp) | 202/222 | 90.99% |

Merged project-owned Linux library/header coverage is now **1604/4545 lines
(35.29%)**, superseding the phase 3 baseline. Coverage is line coverage, not
branch coverage or qualification of the concrete SL16xx/SL26xx transports.
The USB worker test exercises the shared queue worker; libusb and CDC workers
are outside the simulated device-flow coverage. The greater-than-4-GiB
fastboot file guard is not covered with a sparse file across supported
filesystems. Native macOS and remote CI results remain pending.

## Remaining contracts and fault-injection gaps

Tests prefixed `characterization-` record current behavior, not an endorsement
of it. Keep any future behavior correction in a separately explained change:

- Repeated collection/boot-image loads append rather than replace entries.
- Duplicate boot IDs are retained; no iteration-order-specific winner is
  asserted.
- A collection path pointing directly to a single boot-image directory is
  ignored rather than loading that image.
- Some unknown manifest values default to Gen3/unspecified DDR/USB console,
  and direct chip/board matching is case-sensitive.
- A console prompt split across separate `Append()` calls is not detected.
- Factory configuration maps are appended only after successful manifest parsing.
  With a missing manifest, `image_file` configuration is lost; a standalone SPI
  directory can then be accepted as a payload until `Image::Load()` rejects it.
  Global SPI address fields do not override individual manifest image maps.
- The default `eMMCimg` to `SYNAIMG` fallback computes the manifest path before
  changing directories, so the fallback's manifest is not read by default.
- Malformed manifests log a warning and continue, sometimes with partial maps.
  Scalar/empty roots can parse without warning but configure no SPI images.
- eMMC list parsing does not verify that its final payload exists, preserves
  name whitespace, and a final comma-only line clears the selected final image.
- Repeated flash loads append payloads; SPI also appends command overrides.
- eMMC/NAND TAG handling applies chip/memory/DDR defaults but not the detected
  board. Explicit chip configuration prevents TAG defaults from being applied.
- Multiple compatible TAG files select the first directory entry; tests assert
  a coherent result without relying on platform-specific enumeration order.
- Fastboot `DATA` replies must contain exactly eight hexadecimal digits, and
  command writes must report the complete command length; malformed replies
  and short writes fail before payload transfer.
- The USB callback worker drains queued events/actions before shutdown returns.
  Work submitted after its worker has stopped remains unprocessed; callers
  must stop producers before joining the worker.

Known uncovered cases are not simulated with races or unsafe private access:

- A boot directory without a root manifest can use uninitialized fields.
  Do not execute that undefined-behavior path until its contract is fixed.
- Invalid numeric `AstraDeviceStatus` values are outside the valid enum
  tests; its current string lookup has no bounds guard.
- Unknown enum values are tested only where an unnamed value is inside the
  enum's representable range. Secure-boot and transport enums have no such
  value; out-of-range casts are not used to manufacture default-branch
  coverage.
- File deletion between exists/stat/open, `ftell` failure, and file-size
  overflow on 64-bit hosts need an internal I/O seam or an applicable
  platform test. Deterministic short reads and unreadable files are covered.
- Failed temp-directory creation/collision retries need an OS-call seam;
  concurrent successful creation is covered.
- Concurrent console buffer reads/writes and append-versus-shutdown require
  a buffer synchronization contract; the current tests do not introduce
  data races to characterize them.
- Symlink permissions, filesystem relative-path failures, allocation
  failures and compiler-generated exception landing pads are not fully
  exercised.

Phase 5 is implemented. The current unit matrix includes SL16xx public flows
through scripted USB; the shared request loop, fastboot protocol, and callback
worker are covered at their base or scripted boundaries. Some behaviors
described in the broader plan, including SL26xx sessions, manager registries,
concrete transport workers, and OS adapters, are not directly exercised by
these unit suites. Hardware qualification remains a separate phase 6 task.
Fastboot's greater-than-4-GiB file rejection is not exercised with a sparse
large-file fixture on every supported filesystem. Flash-command generation
tests do not qualify firmware behavior, physical flashing, or numeric address
ranges. Core value API tests must not be mistaken for full device-flow coverage.
