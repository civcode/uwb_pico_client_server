# Build guide

## Host dependencies (Ubuntu 24.04)

| Package | Used for |
|---|---|
| `g++` 13, `cmake` 3.28, `ninja-build` | host build |
| `catch2` 3.4 | unit/integration/HIL tests (pinned FetchContent fallback: Catch2 v3.16.0) |
| `libasio-dev` 1.28 | standalone Asio for the simulator and client TCP transport |
| `libcrc` | vendored pinned snapshot in `external/libcrc` (see `external/README.md`) |

## Presets

| Preset | Purpose |
|---|---|
| `host-debug` | normal development build + tests |
| `host-release` | optimized build |
| `host-asan` | ASan/UBSan build for parser, replayer, and boundary tests |
| `pico-w` | Pico W firmware cross-build (needs `PICO_SDK_PATH`) |

## Options

| Option | Default | Notes |
|---|---|---|
| `UWB_BUILD_TESTS` | `ON` | host tests only; never enabled for firmware builds |
| `UWB_BUILD_SIMULATOR` | `ON` | host simulator |
| `UWB_BUILD_CLI` | `ON` | `uwbctl` |
| `UWB_BUILD_PICO_FIRMWARE` | `OFF` | only `ON` with the Pico SDK toolchain preset |
| `UWB_ENABLE_SANITIZERS` | `OFF` | ASan/UBSan for host targets |
| `UWB_STRICT_WARNINGS` | `ON` | `-Werror` project-wide |
| `UWB_ENABLE_HIL_TESTS` | `OFF` | adds `uwb_hil_tests` with CTest label `hil` |
| `HIL_SERIAL_DEVICE` | `""` | serial device path for HIL runs |
| `HIL_PICO_HOST` | `""` | Pico address for HIL runs |

## Test labels

```bash
ctest --preset host-debug -L unit        # unit tests
ctest --preset host-debug -L integration # simulator-driven integration tests
ctest --preset host-debug -L hil         # real hardware, opt-in only
```

## Dependency policy

* Standalone Asio (not Boost.Asio) — `design_decisions.md`.
* CRC32 uses libcrc `crc_32()` / `update_crc_32()` (standard reflected CRC-32,
  `"123456789"` → `0xCBF43926`); `crc_32_pure()` is never used.
* `uwb_protocol` links no third-party library at all (not even libcrc).
* No UI or transport library may be linked into `shared/` or `server-core/`.
