#!/bin/sh
# Benchmark suite: runs every bench/*.apx (except setup.apx) on the custom
# engine and, when the binary has SQLite, on the SQLite backend, and prints
# the median wall time of $RUNS runs per workload. A workload whose first line
# is `// needs: setup.apx` gets that fixture loaded first, untimed.
#
#   bench/run.sh [binary]          default binary: bin/auspex
#   RUNS=5 BENCH_DIR=dir bench/run.sh
#
# BENCH_DIR is where the databases live (default: a fresh directory under the
# system temp dir). Put it on a real disk to see fsync costs; tmpfs hides them.
set -eu

BIN=$(cd "$(dirname "${1:-bin/auspex}")" && pwd)/$(basename "${1:-bin/auspex}")
SRC=$(cd "$(dirname "$0")" && pwd)
RUNS=${RUNS:-5}
WORK=${BENCH_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/auspex-bench.XXXXXX")}
mkdir -p "$WORK"
cd "$WORK"

now_us() {
    # date +%s%N is GNU-only; fall back to python3 elsewhere (macOS).
    t=$(date +%s%N 2>/dev/null)
    case "$t" in
        *N) python3 -c 'import time; print(int(time.time() * 1e6))' ;;
        *) echo $((t / 1000)) ;;
    esac
}

engines="custom"
if "$BIN" --db "$WORK/probe.db" "$SRC/setup.apx" >/dev/null 2>&1; then
    engines="custom sqlite"
fi
rm -f "$WORK/probe.db"

reset_db() {
    rm -f auspex.db auspex.db.packages auspex.db.programs s.db s.db-journal
}

run_one() { # engine file
    if [ "$1" = sqlite ]; then "$BIN" --db s.db "$2"; else "$BIN" "$2"; fi
}

printf '%-14s %12s %12s\n' workload custom sqlite
for wl in "$SRC"/*.apx; do
    name=$(basename "$wl" .apx)
    [ "$name" = setup ] && continue
    needs_setup=0
    head -n 1 "$wl" | grep -q '^// needs: setup.apx' && needs_setup=1
    line=$(printf '%-14s' "$name")
    for engine in custom sqlite; do
        case " $engines " in *" $engine "*) ;; *) line="$line $(printf '%12s' '-')"; continue ;; esac
        # The VM-only workload does not touch SQL; one engine is enough.
        if [ "$name" = vm_loop ] && [ "$engine" = sqlite ]; then
            line="$line $(printf '%12s' '-')"; continue
        fi
        times=""
        r=0
        while [ $r -lt "$RUNS" ]; do
            reset_db
            [ $needs_setup = 1 ] && run_one "$engine" "$SRC/setup.apx" >/dev/null
            t0=$(now_us)
            if ! run_one "$engine" "$wl" >/dev/null 2>bench.err; then
                echo "$name on $engine failed:" >&2; cat bench.err >&2; exit 1
            fi
            t1=$(now_us)
            times="$times $((t1 - t0))"
            r=$((r + 1))
        done
        median=$(echo $times | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END {printf "%.1f ms", a[int((NR+1)/2)]/1000}')
        line="$line $(printf '%12s' "$median")"
    done
    echo "$line"
done
reset_db
rm -f bench.err
[ -z "${BENCH_DIR:-}" ] && rmdir "$WORK" 2>/dev/null || true
