# Vendored third-party sources

## libcrc

Snapshot of <https://github.com/lammertb/libcrc> used for CRC-32 of the persistent
Pico configuration file (specification §51.4, `design_decisions.md`).

| Item | Value |
|---|---|
| Upstream | https://github.com/lammertb/libcrc |
| Tag | `v2.0` |
| Commit | `32aacc558ed599d8e5d38bc8d7dd85e8701f1` (see `tools/refresh_libcrc.sh` output) |
| License | MIT (`libcrc/LICENSE`) |
| Files vendored | `libcrc/include/checksum.h`, `libcrc/src/crc32.c`, `libcrc/tab/gentab32.inc` |

### Why vendored instead of fetched

`libcrc` has no CMake build system and generates its CRC-32 lookup table with its own
`prc` tool (`bin/prc --crc32 tab/gentab32.inc`) before compiling `src/crc32.c`.
Running that generator inside a cross-compiled Pico SDK build is awkward, so the
generated table is checked in and the three source files above are used directly.

### Function used

`crc_32(const unsigned char *input_str, size_t num_bytes)` and
`update_crc_32(uint32_t crc, unsigned char c)`.

These implement the standard reflected CRC-32 (polynomial `0xEDB88320`,
initial value `CRC_START_32 = 0xFFFFFFFF`, final complement `^ 0xFFFFFFFF`), i.e. the
same result as zlib's `crc32()`. Golden vector: `"123456789"` → `0xCBF43926`.

Do **not** use `crc_32_pure()` — that is the non-reflected variant and would not match
host-side test vectors.

### Refreshing the snapshot

```bash
../tools/refresh_libcrc.sh v2.0
```
