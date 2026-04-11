#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# SoundBridge SDK coverage runner (Linux only)
#
# Called by the CMake "coverage" target which sets:
#   SB_BUILD_DIR            - the cmake build tree
#   SB_COVERAGE_MIN_PERCENT - threshold for the gate
#
# All other env vars have sane defaults for standalone invocation.
# ---------------------------------------------------------------------------

ROOT_DIR="${SB_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
BUILD_DIR="${SB_BUILD_DIR:-${ROOT_DIR}/build}"
REPORT_DIR="${SB_COVERAGE_REPORT_DIR:-${BUILD_DIR}/coverage}"
MIN_PERCENT="${SB_COVERAGE_MIN_PERCENT:-35}"
LCOV_RC_ARGS=(--rc branch_coverage=0 --rc derive_function_end_line=0)
LCOV_IGNORE_INCONSISTENT=(--ignore-errors inconsistent)
LCOV_IGNORE_UNUSED=(--ignore-errors unused)
LCOV_IGNORE_CORRUPT=(--ignore-errors corrupt)

rm -rf "${REPORT_DIR}"
find "${BUILD_DIR}" -name "*.gcda" -delete
mkdir -p "${REPORT_DIR}"

# ── 1. Run all SDK tests ────────────────────────────────────────────────────
echo "--- Running SDK tests ---"
ctest --test-dir "${BUILD_DIR}" -R sdk_ --output-on-failure

# ── 2. Capture raw coverage data ────────────────────────────────────────────
echo "--- Capturing coverage data ---"
lcov --capture \
    --directory "${BUILD_DIR}" \
    --output-file "${REPORT_DIR}/raw.info" \
    "${LCOV_IGNORE_INCONSISTENT[@]}" \
    "${LCOV_RC_ARGS[@]}"

# ── 3. Keep only sdk business compilation units (.cpp/.cc/.cxx) ─────────────
echo "--- Extracting SDK business sources ---"
SDK_SOURCES=()
while IFS= read -r src; do
    SDK_SOURCES+=("${src}")
done < <(find "${ROOT_DIR}/sdk" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    ! -path "${ROOT_DIR}/sdk/test/*" \
    ! -path "${ROOT_DIR}/sdk/3rdparty/*")

if [ ${#SDK_SOURCES[@]} -eq 0 ]; then
    echo "ERROR: no SDK business sources found under ${ROOT_DIR}/sdk" >&2
    exit 1
fi

lcov --extract "${REPORT_DIR}/raw.info" \
    "${SDK_SOURCES[@]}" \
    --output-file "${REPORT_DIR}/business.info" \
    "${LCOV_IGNORE_INCONSISTENT[@]}" \
    "${LCOV_IGNORE_UNUSED[@]}" \
    "${LCOV_RC_ARGS[@]}"

# ── 4. Generate HTML report ─────────────────────────────────────────────────
echo "--- Generating HTML report ---"
genhtml "${REPORT_DIR}/business.info" \
    "${LCOV_IGNORE_INCONSISTENT[@]}" \
    --output-directory "${REPORT_DIR}/html" \
    --title "SoundBridge SDK Coverage" \
    --show-details \
    --legend

# ── 5. Summary + threshold gate ─────────────────────────────────────────────
echo "--- Coverage summary ---"
# lcov --summary writes to stderr; redirect to tee via stdout
lcov --summary "${REPORT_DIR}/business.info" \
    "${LCOV_IGNORE_INCONSISTENT[@]}" \
    "${LCOV_IGNORE_CORRUPT[@]}" \
    "${LCOV_RC_ARGS[@]}" \
    2>&1 | tee "${REPORT_DIR}/summary.txt"

# Parse "  lines......: XX.X%" / "  lines.......: XX.X%" from summary
LINES_PCT=$(grep -oP 'lines\.+:\s+\K[0-9]+(\.[0-9]+)?' "${REPORT_DIR}/summary.txt" || true)

if [ -z "${LINES_PCT}" ]; then
    echo "ERROR: failed to parse lines coverage from ${REPORT_DIR}/summary.txt" >&2
    exit 1
fi

echo ""
echo "Lines coverage : ${LINES_PCT}%"
echo "Minimum threshold: ${MIN_PERCENT}%"

if awk "BEGIN { exit (${LINES_PCT} < ${MIN_PERCENT}) ? 1 : 0 }"; then
    echo "PASS: coverage meets threshold"
else
    echo "FAIL: coverage ${LINES_PCT}% is below threshold ${MIN_PERCENT}%" >&2
    exit 1
fi
