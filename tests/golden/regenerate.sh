#!/usr/bin/env bash
# Regenerate the C++ golden vectors from the specification tables.
#
# The vectors MUST come from the specification, never from the C++
# implementation (implementation plan §10.3).  This wrapper writes to a temporary
# file first so an interrupted run can never leave a truncated header behind.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}/../unit/gen/golden_vectors.hpp"
MIN_VECTORS="${MIN_VECTORS:-80}"

tmp="$(mktemp "${TMPDIR:-/tmp}/uwb_golden.XXXXXX" 2>/dev/null || echo "${OUT}.tmp.$$")"
trap 'rm -f "${tmp}"' EXIT

python3 "${HERE}/gen_golden_vectors.py" > "${tmp}"

vectors=$(grep -c '^inline constexpr std::array<std::uint8_t' "${tmp}")
if [ "${vectors}" -lt "${MIN_VECTORS}" ]; then
    echo "golden regeneration produced only ${vectors} vectors; refusing to overwrite ${OUT}" >&2
    exit 1
fi

mkdir -p "$(dirname "${OUT}")"
mv "${tmp}" "${OUT}"
trap - EXIT
echo "wrote ${OUT} (${vectors} vectors)"
