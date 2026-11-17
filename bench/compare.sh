#!/bin/sh
# Turns two bench/run.sh outputs (base and head) into a markdown table of
# before/after times, flagging workloads that got slower than a threshold.
#
#   bench/compare.sh base.txt head.txt [base_instructions head_instructions]
#   BENCH_THRESHOLD=10 bench/compare.sh ...     flag changes > 10% (default)
#
# The optional instruction counts (vm_loop under cachegrind) get their own
# row: wall time on shared CI runners is noisy, instruction counts are not.
set -eu

if [ $# -ne 2 ] && [ $# -ne 4 ]; then
    echo "usage: $0 base.txt head.txt [base_instructions head_instructions]" >&2
    exit 2
fi
BASE=$1
HEAD=$2
BASE_IR=${3:-}
HEAD_IR=${4:-}
THRESHOLD=${BENCH_THRESHOLD:-10}

# Percentage change from $1 to $2, with a warning mark past the threshold;
# empty when either side is not a number.
change() {
    awk -v b="$1" -v h="$2" -v t="$THRESHOLD" 'BEGIN {
        if (b !~ /^[0-9.]+$/ || h !~ /^[0-9.]+$/ || b + 0 == 0) exit
        pct = (h - b) / b * 100
        printf "%+.1f%%%s", pct, (pct > t ? " :warning:" : "")
    }'
}

with_unit() {
    case "$1" in
        *[0-9]) echo "$1 ms" ;;
        *) echo "$1" ;;
    esac
}

# run.sh prints a header, then per workload: the name, then per engine
# "<ms> ms", "-" (not run) or "fail". Without the " ms" units every line is
# three fields: name, custom, sqlite.
normalize() {
    sed '1d; s/ ms//g' "$1"
}

echo "| workload | engine | base | head | change |"
echo "|---|---|---:|---:|---:|"
normalize "$HEAD" | while read -r name head_custom head_sqlite; do
    base_custom=$(normalize "$BASE" | awk -v n="$name" '$1 == n {print $2}')
    base_sqlite=$(normalize "$BASE" | awk -v n="$name" '$1 == n {print $3}')
    for engine in custom sqlite; do
        if [ $engine = custom ]; then b=$base_custom; h=$head_custom
        else b=$base_sqlite; h=$head_sqlite; fi
        [ -n "$b" ] || b=new
        [ "$h" = "-" ] && continue
        echo "| $name | $engine | $(with_unit "$b") | $(with_unit "$h") | $(change "$b" "$h") |"
    done
done

if [ -n "$BASE_IR" ] || [ -n "$HEAD_IR" ]; then
    fmt_ir() {
        [ -n "$1" ] || { echo "-"; return; }
        echo "$1" | awk '{ s = $1; out = ""
            while (length(s) > 3) { out = "," substr(s, length(s) - 2) out; s = substr(s, 1, length(s) - 3) }
            print s out }'
    }
    echo "| vm_loop | instructions | $(fmt_ir "$BASE_IR") | $(fmt_ir "$HEAD_IR") | $(change "$BASE_IR" "$HEAD_IR") |"
fi

echo
echo "Wall time is the median of ${RUNS:-5} runs on a shared runner, so expect ±10–20% noise."
if [ -n "$BASE_IR" ] || [ -n "$HEAD_IR" ]; then
    echo "The instruction count is exact."
fi
echo ":warning: marks more than ${THRESHOLD}% slower."
