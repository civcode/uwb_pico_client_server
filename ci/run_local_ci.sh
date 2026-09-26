#!/usr/bin/env bash
# Local mirror of .github/workflows/ci.yml (host part).
#
# Usage: ci/run_local_ci.sh [all|debug|asan|firmware]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

MODE="${1:-all}"

echo "==> golden vectors (specification-derived)"
"${ROOT}/tests/golden/verify.sh"

run_host() {
    local preset="$1"
    echo "==> configure ${preset}"
    cmake --preset "${preset}"
    echo "==> build ${preset}"
    cmake --build --preset "${preset}"
    echo "==> tests ${preset}"
    ctest --preset "${preset}" -L unit --output-on-failure
    # NOTE: no grep -q in the pipeline here: pipefail would turn the early exit
    # of grep into a false condition and silently skip the integration tests.
    local available
    available="$(ctest --preset "${preset}" -L integration --show-only 2>/dev/null || true)"
    if [[ "${available}" == *"Test #"* ]]; then
        echo "==> integration tests ${preset}"
        ctest --preset "${preset}" -L integration --output-on-failure
    fi
}

case "${MODE}" in
    debug) run_host host-debug ;;
    asan) run_host host-asan ;;
    firmware)
        : "${PICO_SDK_PATH:?PICO_SDK_PATH must be set for firmware builds}"
        cmake --preset pico-w
        cmake --build --preset pico-w
        ;;
    all)
        run_host host-debug
        run_host host-asan
        ;;
    *)
        echo "usage: $0 [all|debug|asan|firmware]" >&2
        exit 2
        ;;
esac

echo "==> local CI OK"
