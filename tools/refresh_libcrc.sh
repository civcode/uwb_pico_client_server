#!/usr/bin/env bash
# Re-vendor the pinned libcrc snapshot used by uwb-system/external/libcrc.
#
# Usage: tools/refresh_libcrc.sh [tag]
set -euo pipefail

TAG="${1:-v2.0}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${SCRIPT_DIR}/../external/libcrc"
TMP="$(mktemp -d)"

trap 'rm -rf "${TMP}"' EXIT

git clone --quiet --depth 1 https://github.com/lammertb/libcrc "${TMP}/libcrc"
git -C "${TMP}/libcrc" fetch --quiet --depth 1 origin "tag/${TAG}"
git -C "${TMP}/libcrc" checkout --quiet "${TAG}"

mkdir -p "${TMP}/libcrc/bin" "${TMP}/libcrc/tab"
make -C "${TMP}/libcrc" bin/prc >/dev/null
"${TMP}/libcrc/bin/prc" --crc32 "${TMP}/libcrc/tab/gentab32.inc"

mkdir -p "${DEST}/include" "${DEST}/src" "${DEST}/tab"
cp "${TMP}/libcrc/LICENSE" "${DEST}/LICENSE"
cp "${TMP}/libcrc/include/checksum.h" "${DEST}/include/checksum.h"
cp "${TMP}/libcrc/src/crc32.c" "${DEST}/src/crc32.c"
cp "${TMP}/libcrc/tab/gentab32.inc" "${DEST}/tab/gentab32.inc"

echo "Vendored libcrc ${TAG} ($(git -C "${TMP}/libcrc" rev-parse HEAD)) into ${DEST}"
