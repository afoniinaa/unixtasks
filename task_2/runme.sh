#!/bin/bash

STATS_FILE="stats.txt"
RESULT_FILE="result.txt"
TEST_FILE="testfile"
PROCESS_COUNT=10
TEST_DURATION_SEC=300

rm -f "$RESULT_FILE" "$STATS_FILE" "${TEST_FILE}.lck" "$TEST_FILE"
make -f Makefile clean 2>/dev/null
make -f Makefile

if [ ! -f file_lock ]; then
    echo "Build failed: file_lock binary was not created." | tee "$RESULT_FILE"
    exit 1
fi

touch "$TEST_FILE"
echo "========================================"
echo "Starting lock stress test"
echo "Processes: $PROCESS_COUNT"
echo "Duration : ${TEST_DURATION_SEC}s"
echo "========================================"

pids=()
for ((i = 1; i <= PROCESS_COUNT; i++)); do
    ./file_lock "$TEST_FILE" &
    pids+=($!)
    sleep 0.05
done

sleep "$TEST_DURATION_SEC"

echo "Sending SIGINT to worker processes..."
for pid in "${pids[@]}"; do
    kill -INT "$pid" 2>/dev/null
done

echo "Waiting for all workers to exit..."
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null
done

ZERO_COUNT=0
MIN=0
MAX=0
LINES=0

if [ -f "$STATS_FILE" ]; then
    LINES=$(grep -c '\[PID ' "$STATS_FILE" 2>/dev/null || echo 0)

    ZERO_COUNT=$(grep '^PID ' "$STATS_FILE" \
                 | awk -F': ' '$2 == 0 {count++} END {print count+0}')

    MIN=$(grep '^PID ' "$STATS_FILE" \
          | awk -F': ' '{print $2}' | sort -n | head -1)
    MAX=$(grep '^PID ' "$STATS_FILE" \
          | awk -F': ' '{print $2}' | sort -n | tail -1)
else
    LINES=0
fi

{
    echo "========== Test Summary =========="
    echo "Stats file          : $STATS_FILE"
    echo "Result rows         : $LINES (expected $PROCESS_COUNT)"
    echo "Processes with zero : $ZERO_COUNT"
    echo "Lock count range    : $MIN - $MAX"
} > "$RESULT_FILE"

if [ -f "${TEST_FILE}.lck" ]; then
    echo "Lock cleanup        : FAILED (lock file still exists)" >> "$RESULT_FILE"
else
    echo "Lock cleanup        : OK"                              >> "$RESULT_FILE"
fi

if [ "$LINES" -eq "$PROCESS_COUNT" ] \
   && [ "$ZERO_COUNT" -eq 0 ]        \
   && [ ! -f "${TEST_FILE}.lck" ]; then
    echo "Overall status      : OK"   >> "$RESULT_FILE"
else
    echo "Overall status      : FAIL" >> "$RESULT_FILE"
fi

echo ""
echo "========================================"
echo "Stats ($STATS_FILE):"
echo "========================================"
cat "$STATS_FILE"

echo ""
echo "========================================"
echo "Full report ($RESULT_FILE):"
echo "========================================"
cat "$RESULT_FILE"