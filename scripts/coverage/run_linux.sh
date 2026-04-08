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

mkdir -p "${REPORT_DIR}"

# ── 1. Run all SDK tests ────────────────────────────────────────────────────
echo "--- Running SDK tests ---"
ctest --test-dir "${BUILD_DIR}" -R sdk_ --output-on-failure

# ── 2. Capture raw coverage data ────────────────────────────────────────────
echo "--- Capturing coverage data ---"
lcov --capture \
    --directory "${BUILD_DIR}" \
    --output-file "${REPORT_DIR}/raw.info" \
    --rc lcov_branch_coverage=0

# ── 3. Remove test / 3rdparty / build paths ─────────────────────────────────
echo "--- Filtering non-business paths ---"
lcov --remove "${REPORT_DIR}/raw.info" \
    '*/sdk/test/*' \
    '*/sdk/3rdparty/*' \
    '*/build/*' \
    --output-file "${REPORT_DIR}/filtered.info" \
    --rc lcov_branch_coverage=0

# ── 4. Keep only sdk business compilation units (.cpp/.cc/.cxx) ─────────────
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

lcov --extract "${REPORT_DIR}/filtered.info" \
    "${SDK_SOURCES[@]}" \
    --output-file "${REPORT_DIR}/business.info" \
    --rc lcov_branch_coverage=0

# ── 5. Generate HTML report ─────────────────────────────────────────────────
echo "--- Generating HTML report ---"
genhtml "${REPORT_DIR}/business.info" \
    --output-directory "${REPORT_DIR}/html" \
    --title "SoundBridge SDK Coverage" \
    --show-details \
    --legend

# ── 6. Summary + threshold gate ─────────────────────────────────────────────
echo "--- Coverage summary ---"
# lcov --summary writes to stderr; redirect to tee via stdout
lcov --summary "${REPORT_DIR}/business.info" \
    --rc lcov_branch_coverage=0 \
    2>&1 | tee "${REPORT_DIR}/summary.txt"

# Parse "  lines......: XX.X%" from summary
LINES_PCT=$(grep -oP 'lines\.{6}:\s+\K[0-9]+(\.[0-9]+)?' "${REPORT_DIR}/summary.txt" || true)

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
