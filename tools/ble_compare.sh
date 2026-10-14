#!/usr/bin/env bash
set -u

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <name> [label]" >&2
    exit 2
fi

NAME="$1"
LABEL="${2:-default}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"

LOG_DIR="${TMPDIR:-.}"
LOG_FILE="${LOG_DIR%/}/ble-compare-${LABEL}-${TIMESTAMP}.log"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

echo "=== BLE Latency & Throughput Comparison ==="
echo "Target name: ${NAME}"
echo "Run label:   ${LABEL}"
echo "Logging to:  ${LOG_FILE}"
echo ""

run_benchmarks() {
    echo "================================================================================"
    echo "BLE Benchmark Comparison: ${NAME} [${LABEL}]"
    echo "Started at: $(date)"
    echo "================================================================================"
    echo ""
    echo "--- Phase 1: Latency Benchmark ---"
    "${PYTHON}" "${SCRIPT_DIR}/ble_latency.py" --name "${NAME}" --label "${LABEL}"
    LATENCY_STATUS=$?
    echo "Phase 1 status: ${LATENCY_STATUS}"
    echo ""
    echo "--- Phase 2: Throughput Benchmark ---"
    "${PYTHON}" "${SCRIPT_DIR}/ble_throughput.py" --name "${NAME}"
    THROUGHPUT_STATUS=$?
    echo "Phase 2 status: ${THROUGHPUT_STATUS}"
    echo ""
    echo "================================================================================"
    echo "Completed at: $(date)"
    echo "================================================================================"

    if [ "${LATENCY_STATUS}" -ne 0 ] || [ "${THROUGHPUT_STATUS}" -ne 0 ]; then
        return 1
    fi
    return 0
}

# Stream output to terminal and save complete log to file
run_benchmarks 2>&1 | tee "${LOG_FILE}"
EXIT_STATUS="${PIPESTATUS[0]}"

echo ""
echo "Saved comparison log to: ${LOG_FILE}"
exit "${EXIT_STATUS}"
