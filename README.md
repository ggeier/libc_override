# Alloc Return Override

This project builds a preload library that overrides higher-level libc functions which return caller-owned heap memory, plus a mock allocator used to verify that those allocations flow through the expected allocator path.

Current coverage includes:
- `strdup`, `strndup`, `wcsdup`
- `asprintf`, `vasprintf`
- `getdelim`, `__getdelim`, `getline`
- `open_memstream`, `open_wmemstream`
- `scanf` `%m` allocation paths across narrow and wide entry points, including `__isoc99_*` aliases
- `getcwd(NULL, 0)`, `get_current_dir_name`, `realpath(path, NULL)`, `tempnam`
- `scandir` and, on libcs that expose it separately, `scandir64`

The project produces two shared libraries:
- `liballoc_return_override.so`: overrides the higher-level allocation-returning APIs
- `libmock_allocator.so`: test-only allocator shim that forwards to `RTLD_NEXT` and records allocation traffic

The current tests validate preload behavior on a Linux PC and can also cross-build and run on an OHOS phone through `hdc`.

## Repository Layout

- `CMakeLists.txt`: build and test entry point
- `src/`: preload libraries
- `tests/`: phase 1A test executables
- `scanf_percent_m_hybrid.md`: scanf `%m` hybrid design notes and limitations
- `cmake/run_ohos_test.cmake`: host-side launcher for OHOS device tests
- `ohos-sdk/`: supplied OHOS SDK and toolchain
- `third_party_musl/`: musl source tree used for implementation reference

## What The Tests Prove

- `phase1a_resolution_smoke`: the overridden symbols resolve from `liballoc_return_override.so`
- `phase1a_allocator_validation`: the returned objects are tracked by `libmock_allocator.so`
- `phase1a_mock_only_diagnostic`: baseline behavior with only the mock allocator preloaded
- `phase1a_mock_only_strict_negative`: strict mock-only negative assertions; this is meaningful on the target musl/OHOS environment, not generally on host glibc

The positive tests preload libraries in this order:

```bash
LD_PRELOAD="liballoc_return_override.so:libmock_allocator.so"
```

That gives symbol precedence to the higher-level override library while routing its internal allocator calls into the mock allocator.

## scanf `%m` Notes

- The scanf `%m` override is hybrid: it lets libc perform the actual scan and then rehomes successful `%m` results into the preloaded allocator when possible.
- The allocator bridge ABI used by that path is declared in `include/allocator_bridge.h`.
- Embedded NUL data in string-like `%m` results truncates during rehome because the post-scan pointer no longer carries the original logical length.
- Wide-input `%mc` cannot currently be rehomed safely and may remain libc-owned.
- The detailed behavior, limitations, and `MEMORY LEAK` fallback cases are documented in `scanf_percent_m_hybrid.md`.

## Build And Test On A Linux PC

Run these commands from the repository root:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This builds:
- `build/liballoc_return_override.so`
- `build/libmock_allocator.so`
- `build/phase1a_smoke`
- `build/phase1a_validation`
- `build/phase1a_mock_only`

The mock-only diagnostic report is written to:

```text
build/phase1a_mock_only_diagnostic.txt
```

If you want the optional strict negative test registered as well, configure with:

```bash
cmake -S . -B build-strict \
  -DALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS=ON
cmake --build build-strict
ctest --test-dir build-strict --output-on-failure
```

Note that host glibc often already routes many of these APIs through interposable allocator entry points, so the mock-only run on a PC is mainly diagnostic. It is useful for comparison, but it is not proof that `liballoc_return_override.so` is unnecessary on the musl target.

## Build And Test On An OHOS Phone

The supplied OHOS toolchain file is:

```text
ohos-sdk/linux/native-linux-x64-6.0.0.48-Release/native/build/cmake/ohos.toolchain.cmake
```

The supplied `hdc` executable is:

```text
ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc
```

Before running tests on the phone, confirm the device is visible:

```bash
./ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc list targets
```

If the output is empty, connect the phone first. The default target architecture is `arm64-v8a`.

Configure, build, and run the phone test suite like this:

```bash
cmake -S . -B build-ohos \
  -DCMAKE_TOOLCHAIN_FILE=ohos-sdk/linux/native-linux-x64-6.0.0.48-Release/native/build/cmake/ohos.toolchain.cmake \
  -DOHOS_ARCH=arm64-v8a \
  -DALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON \
  -DHDC_EXECUTABLE=ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc
cmake --build build-ohos
ctest --test-dir build-ohos --output-on-failure
```

This cross-build produces OHOS binaries such as:
- `build-ohos/liballoc_return_override.so`
- `build-ohos/libmock_allocator.so`
- `build-ohos/phase1a_smoke`
- `build-ohos/phase1a_validation`
- `build-ohos/phase1a_mock_only`

When `ALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON` is set, CTest does not run target binaries directly on the host. Instead, `cmake/run_ohos_test.cmake`:
- pushes the test executable and preload DSOs to the phone
- creates the device test directory
- sets `LD_LIBRARY_PATH`
- sets `LD_PRELOAD`
- runs the test through `hdc shell`
- pulls back diagnostic output when needed

By default, device-side artifacts are placed under:

```text
/data/local/tmp/alloc_override_tests
```

The mock-only diagnostic report is pulled back to:

```text
build-ohos/phase1a_mock_only_diagnostic.txt
```

If you also want the strict negative test on the phone, use:

```bash
cmake -S . -B build-ohos-strict \
  -DCMAKE_TOOLCHAIN_FILE=ohos-sdk/linux/native-linux-x64-6.0.0.48-Release/native/build/cmake/ohos.toolchain.cmake \
  -DOHOS_ARCH=arm64-v8a \
  -DALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON \
  -DALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS=ON \
  -DHDC_EXECUTABLE=ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc
cmake --build build-ohos-strict
ctest --test-dir build-ohos-strict --output-on-failure
```

## Useful CMake Options

- `ALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS`: register the strict mock-only negative test
- `ALLOC_OVERRIDE_RUN_DEVICE_TESTS`: register OHOS device-run CTest entries when cross-compiling
- `HDC_EXECUTABLE`: path to `hdc`
- `ALLOC_OVERRIDE_DEVICE_DIR`: target directory on the device; default is `/data/local/tmp/alloc_override_tests`
- `ALLOC_OVERRIDE_TEST_TMPDIR`: temporary directory used by the tests on the target; defaults to `ALLOC_OVERRIDE_DEVICE_DIR`
- `OHOS_ARCH`: target architecture for the OHOS toolchain; default is `arm64-v8a`

Example with a custom device directory:

```bash
cmake -S . -B build-ohos \
  -DCMAKE_TOOLCHAIN_FILE=ohos-sdk/linux/native-linux-x64-6.0.0.48-Release/native/build/cmake/ohos.toolchain.cmake \
  -DOHOS_ARCH=arm64-v8a \
  -DALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON \
  -DHDC_EXECUTABLE=ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc \
  -DALLOC_OVERRIDE_DEVICE_DIR=/data/local/tmp/my_alloc_tests \
  -DALLOC_OVERRIDE_TEST_TMPDIR=/data/local/tmp/my_alloc_tests
```

## Inspect Registered OHOS Device Tests

To see the exact CTest commands that will be used for the phone run:

```bash
ctest --test-dir build-ohos -N -V
```

This is useful for debugging `hdc`, paths, preload order, and pulled report locations.

## Troubleshooting

- `hdc list targets` shows no devices:
  - connect the phone and make sure `hdc` can see it before running `ctest`
- OHOS build succeeds but no device tests are registered:
  - configure with `-DALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON`
- need a different device architecture:
  - change `-DOHOS_ARCH=...`
- host mock-only results look too positive:
  - expected on glibc; use the OHOS strict negative test for target-relevant negative coverage
- need to inspect the mock allocator counters without the override library:
  - read `build/phase1a_mock_only_diagnostic.txt` on host or `build-ohos/phase1a_mock_only_diagnostic.txt` after a device run

## Current Scope

This README covers the current host/device test structure for the implemented override set. The same pattern can be reused for later additions such as `__sched_cpualloc`.
