set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MYINIT="$SCRIPT_DIR/myinit"
CONF1="/tmp/myinit_test.conf"
CONF_ONE="/tmp/myinit_one.conf"
LOG="/tmp/myinit.log"
PID_FILE="/tmp/myinit.pid"
RESULT="$SCRIPT_DIR/result.txt"

> "$RESULT"

log() { echo "$@" | tee -a "$RESULT"; }
pass() { log "  [PASS] $*"; }
fail() { log "  [FAIL] $*"; }
sep() { log ""; log "----------------------------------------------------------"; }

sep
log "build"
cd "$SCRIPT_DIR"
make clean && make
if [ $? -ne 0 ]; then
    log "ERROR: build failed, aborting test"
    exit 1
fi
log "Build succeeded"

sep
log "stub programs"

for N in 1 2 3; do
    cat > "/tmp/myinit_proc${N}" <<'EOF'
#!/bin/sh
while true; do
    sleep 2
done
EOF
    chmod +x "/tmp/myinit_proc${N}"
    log "  /tmp/myinit_proc${N} created"
done

touch /tmp/myinit_in1 /tmp/myinit_in2 /tmp/myinit_in3

cat > "$CONF1" << EOF
/tmp/myinit_proc1 /tmp/myinit_in1 /tmp/myinit_out1
/tmp/myinit_proc2 /tmp/myinit_in2 /tmp/myinit_out2
/tmp/myinit_proc3 /tmp/myinit_in3 /tmp/myinit_out3
EOF
log "  Config (3 processes): $CONF1"

echo "/tmp/myinit_proc1 /tmp/myinit_in1 /tmp/myinit_out1" > "$CONF_ONE"
log "  Config (1 process): $CONF_ONE"

if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE" 2>/dev/null || true)
    if [ -n "$OLD_PID" ]; then
        kill "$OLD_PID" 2>/dev/null || true
        sleep 0.5
    fi
fi

pkill -f "myinit -c" 2>/dev/null || true
sleep 0.5

sep
log "TEST 1: Start three child processes"
log "  Expected: myinit_proc1, myinit_proc2, myinit_proc3 are running"

"$MYINIT" -c "$CONF1"
sleep 1.5

log ""
log "  ps output (looking for myinit_proc[1-3]):"
PS_OUT=$(ps aux | grep "[m]yinit_proc[123]" || true)
echo "$PS_OUT" | tee -a "$RESULT"

COUNT=$(echo "$PS_OUT" | grep -c "[m]yinit_proc" || true)
log "  Process count: $COUNT (expected 3)"

if [ "$COUNT" -ge 3 ]; then
    pass "Three child processes running"
else
    fail "Expected 3, found $COUNT"
fi

sep
log "TEST 2: Kill proc2, wait for auto-restart"
log "  Expected: after proc2 dies, three processes are running again"

pkill -f "myinit_proc2" 2>/dev/null || true
sleep 2

log ""
log "  ps output after restart:"
PS_OUT=$(ps aux | grep "[m]yinit_proc[123]" || true)
echo "$PS_OUT" | tee -a "$RESULT"

COUNT=$(echo "$PS_OUT" | grep -c "[m]yinit_proc" || true)
log "  Process count: $COUNT (expected 3)"

if [ "$COUNT" -ge 3 ]; then
    pass "After proc2 restart, three processes again"
else
    fail "Expected 3, found $COUNT"
fi

sep
log "TEST 3: SIGHUP, config swap, expect 1 process"
log "  Expected: only myinit_proc1 is running"

cp "$CONF_ONE" "$CONF1"
log "  Config replaced with a single line"

MYINIT_PID=$(cat "$PID_FILE" 2>/dev/null || true)
if [ -n "$MYINIT_PID" ]; then
    kill -HUP "$MYINIT_PID"
    log "  SIGHUP sent to PID $MYINIT_PID"
else
    log "  WARNING: PID file not found, trying pkill"
    pkill -HUP -f "myinit -c" || true
fi
sleep 2

log ""
log "  ps output after SIGHUP:"
PS_OUT=$(ps aux | grep "[m]yinit_proc[123]" || true)
echo "$PS_OUT" | tee -a "$RESULT"

COUNT=$(echo "$PS_OUT" | grep -c "[m]yinit_proc" || true)
log "  Process count: $COUNT (expected 1)"

if [ "$COUNT" -eq 1 ]; then
    pass "Exactly one process after SIGHUP"
else
    fail "Expected 1, found $COUNT"
fi

if echo "$PS_OUT" | grep -q "myinit_proc1"; then
    pass "proc1 is running (expected)"
else
    fail "proc1 not found among running processes"
fi

if echo "$PS_OUT" | grep -qE "myinit_proc[23]"; then
    fail "proc2 or proc3 still running (should not be)"
else
    pass "proc2 and proc3 terminated"
fi

sep
log "log file: $LOG"
log ""
if [ -f "$LOG" ]; then
    cat "$LOG" | tee -a "$RESULT"
else
    log "  WARNING: log file not found"
fi

sep
log "grep log"

check_log() {
    local desc="$1"
    local pattern="$2"
    if grep -q "$pattern" "$LOG" 2>/dev/null; then
        pass "$desc"
    else
        fail "$desc (pattern: '$pattern')"
    fi
}

check_log "Start proc1"              "Started proc\[0\].*myinit_proc1"
check_log "Start proc2"              "Started proc\[1\].*myinit_proc2"
check_log "Start proc3"              "Started proc\[2\].*myinit_proc3"
check_log "proc2 death and restart"  "proc\[1\].*myinit_proc2.*restart"
check_log "SIGHUP handling"          "SIGHUP"
check_log "SIGHUP child teardown"    "SIGHUP: terminating all child processes"
check_log "proc1 restart after SIGHUP" "Started proc\[0\].*myinit_proc1"

sep
log "stop myinit"
MYINIT_PID=$(cat "$PID_FILE" 2>/dev/null || true)
if [ -n "$MYINIT_PID" ]; then
    kill "$MYINIT_PID" 2>/dev/null || true
    log "  myinit (pid=$MYINIT_PID) stopped"
else
    pkill -f "myinit -c" 2>/dev/null || true
    log "  myinit stopped via pkill"
fi
sleep 0.5

rm -f /tmp/myinit_proc1 /tmp/myinit_proc2 /tmp/myinit_proc3
rm -f /tmp/myinit_in1   /tmp/myinit_in2   /tmp/myinit_in3
rm -f /tmp/myinit_out1  /tmp/myinit_out2  /tmp/myinit_out3
rm -f /tmp/myinit_test.conf /tmp/myinit_one.conf
rm -f /tmp/myinit.pid

sep
log "done, see $RESULT"
