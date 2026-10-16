#!/usr/bin/env bash
# fizzbuzz-arena scoring pipeline.
#
# Usage: run_score.sh <submission-dir> [result-json-out]
#
# Env knobs (CI sets these):
#   SCORE_PORT      port to serve on          (default 8080)
#   SCORE_CONNS     loadgen connections       (default 5000)
#   SCORE_SECONDS   measured duration         (default 30)
#   SCORE_THREADS   loadgen threads           (default 2)
#   PIN_SERVER      cores for the server      (default: unset)
#   PIN_LOADGEN     cores for the loadgen     (default: unset)
#
# Steps:
#   1. star pledge check (README must contain "I starred")
#   2. build baseline + loadgen
#   3. baseline gets benched first, same runner, same parameters
#   4. submission: install.sh (sudo, optional), build.sh (optional),
#      start.sh (foreground, PORT env), readiness probe, correctness gate,
#      then the same bench
#   5. ratio = submission rps / baseline rps, verdict, result json
#
# Exit codes: 0 = scored (may still be disqualified, see json),
#             2 = could not build/start, 3 = internal error.

set -u

SUB="$1"
OUT="${2:-result.json}"
PORT="${SCORE_PORT:-8080}"
CONNS="${SCORE_CONNS:-5000}"
SECONDS_="${SCORE_SECONDS:-30}"
THREADS="${SCORE_THREADS:-2}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$SUB" ]; then
    echo "score: no such submission dir: $SUB"
    exit 2
fi

pin_server()  { if [ -n "${PIN_SERVER:-}" ];  then taskset -c "$PIN_SERVER"  "$@"; else "$@"; fi; }
pin_loadgen() { if [ -n "${PIN_LOADGEN:-}" ]; then taskset -c "$PIN_LOADGEN" "$@"; else "$@"; fi; }

# ulimit headroom for the loadgen
ulimit -n 65536 2>/dev/null || true

fail_early() {
    echo "score: $1"
    exit 2
}

# ---------- 1. star pledge ----------

if ! grep -qi "I starred" "$SUB/README.md" 2>/dev/null; then
    fail_early "submission README.md must contain the line: I starred fizzbuzz-arena (see the rules, it is the law)"
fi
if [ ! -x "$SUB/start.sh" ] && [ ! -f "$SUB/start.sh" ]; then
    fail_early "submission must have a start.sh"
fi

# ---------- 2. build baseline + loadgen ----------

cc -O2 -o "$ROOT/baseline/server" "$ROOT/baseline/server.c" || fail_early "baseline build failed"
cc -O2 -o "$ROOT/loadgen/loadgen" "$ROOT/loadgen/loadgen.c" -lpthread || fail_early "loadgen build failed"

BENCH() { # $1 = out json
    pin_loadgen "$ROOT/loadgen/loadgen" -h 127.0.0.1 -p "$PORT" \
        -c "$CONNS" -t "$SECONDS_" -T "$THREADS" -o "$1"
}

# ---------- 3. baseline bench ----------

echo "== benching the baseline =="
setsid "$ROOT/baseline/server" "$PORT" &
BASE_PID=$!
sleep 0.5
python3 "$ROOT/checker/checker.py" "$PORT" || true   # sanity, baseline must pass its own checker
BENCH /tmp/base.json
BASE_RC=$?
kill -TERM -- -"$BASE_PID" 2>/dev/null
wait "$BASE_PID" 2>/dev/null
if [ ! -f /tmp/base.json ]; then
    fail_early "baseline bench produced no result, internal error"
fi

# ---------- 4. submission ----------

echo "== building and running submission: $SUB =="
if [ -f "$SUB/install.sh" ]; then
    (cd "$SUB" && sudo bash install.sh) || fail_early "install.sh failed"
fi
if [ -f "$SUB/build.sh" ]; then
    (cd "$SUB" && bash build.sh) || fail_early "build.sh failed"
fi

PORT="$PORT" setsid bash -c "cd '$SUB' && exec bash start.sh" &
SUB_PID=$!

# readiness probe: /3 must say fizz within 90 seconds
READY=0
for i in $(seq 1 90); do
    GOT=$(curl -s -m 2 "http://127.0.0.1:$PORT/3" 2>/dev/null)
    if [ "$GOT" = "fizz" ]; then READY=1; break; fi
    sleep 1
done
if [ "$READY" != "1" ]; then
    kill -TERM -- -"$SUB_PID" 2>/dev/null
    fail_early "server never answered GET /3 with 'fizz' within 90 seconds"
fi

echo "== correctness gate =="
if ! python3 "$ROOT/checker/checker.py" "$PORT"; then
    kill -TERM -- -"$SUB_PID" 2>/dev/null
    echo '{ "verdict": "FAILED_CORRECTNESS" }' > "$OUT"
    echo "score: correctness gate FAILED"
    exit 0
fi

echo "== benching the submission =="
BENCH /tmp/sub.json
SUB_RC=$?
kill -TERM -- -"$SUB_PID" 2>/dev/null
wait "$SUB_PID" 2>/dev/null

if [ ! -f /tmp/sub.json ]; then
    fail_early "submission bench produced no result"
fi

# ---------- 5. verdict ----------

python3 - "$SUB" "$OUT" << 'PYEOF'
import json, sys, datetime

sub_dir, out_path = sys.argv[1], sys.argv[2]

base = json.load(open("/tmp/base.json"))
sub = json.load(open("/tmp/sub.json"))

ratio = sub["rps"] / base["rps"] if base["rps"] else 0.0
qualified = (
    sub.get("incorrect", 1) == 0
    and sub.get("error_pct", 100) <= 0.01
    and sub.get("total", 0) > 0
)

result = {
    "submission": sub_dir.rstrip("/").split("/")[-1],
    "verdict": "QUALIFIED" if qualified else "DISQUALIFIED",
    "ratio": round(ratio, 3),
    "rps": round(sub["rps"], 1),
    "baseline_rps": round(base["rps"], 1),
    "p50_ms": sub.get("p50_ms"),
    "p95_ms": sub.get("p95_ms"),
    "p99_ms": sub.get("p99_ms"),
    "error_pct": sub.get("error_pct"),
    "incorrect": sub.get("incorrect"),
    "total": sub.get("total"),
    "conns": sub.get("conns"),
    "duration_s": sub.get("duration_s"),
    "scored_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%d"),
}
json.dump(result, open(out_path, "w"), indent=2)
print(json.dumps(result, indent=2))
PYEOF

echo "score: done"
exit 0
