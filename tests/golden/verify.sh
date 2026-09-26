#!/usr/bin/env bash
# Verify that the committed golden vectors match a fresh regeneration from the
# specification tables (implementation plan §10.3).
#
# Fails if the committed file is stale, hand-edited, or truncated.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMITTED="${HERE}/../unit/gen/golden_vectors.hpp"

if [ ! -f "${COMMITTED}" ]; then
    echo "missing golden vectors: ${COMMITTED}" >&2
    exit 1
fi

tmp="$(mktemp "${TMPDIR:-/tmp}/uwb_golden.XXXXXX" 2>/dev/null || echo "${COMMITTED}.tmp.$$")"
trap 'rm -f "${tmp}"' EXIT
python3 "${HERE}/gen_golden_vectors.py" > "${tmp}"

committed_vectors=$(grep -c '^inline constexpr std::array<std::uint8_t' "${COMMITTED}")
fresh_vectors=$(grep -c '^inline constexpr std::array<std::uint8_t' "${tmp}")

if [ "${committed_vectors}" != "${fresh_vectors}" ]; then
    echo "golden vector count mismatch: committed=${committed_vectors} regenerated=${fresh_vectors}" >&2
    exit 1
fi

if ! diff -u "${COMMITTED}" "${tmp}"; then
    echo "golden vectors are stale: run tests/golden/regenerate.sh" >&2
    exit 1
fi

echo "golden vectors up to date (${committed_vectors} vectors)"
