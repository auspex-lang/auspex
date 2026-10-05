#!/usr/bin/env bash
# Smoke-test every runnable example in examples/ (top level and phases/):
# each must exit 0.
#
# Usage: tests/run_examples.sh [path/to/auspex] [example.apx...]
#
# Each example runs in its own temporary copy of examples/, so database files
# and imports never leak between runs. An example with no directives runs
# once with no arguments. Directives are `// smoke:` comment lines:
#
#   // smoke: args --db :memory:      run with these arguments; one run per
#                                     args line (e.g. a second -DDEBUG run)
#   // smoke: setup phase7_setup.apx run another example first, same dir
#   // smoke: requires sqlite         skip unless auspex was built with SQLite
#   // smoke: requires linux          skip unless running on Linux
#   // smoke: skip <reason>           never run (e.g. an imported module)

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
AUSPEX=${1:-$ROOT/bin/auspex}
shift $(( $# > 0 ? 1 : 0 ))
case $AUSPEX in /*) ;; *) AUSPEX=$PWD/$AUSPEX ;; esac
TIMEOUT_SECONDS=${EXAMPLE_TIMEOUT:-30}

if [ ! -x "$AUSPEX" ]; then
    echo "run_examples: $AUSPEX is not executable (run make first)" >&2
    exit 2
fi

have_sqlite=1
if "$AUSPEX" --help 2>&1 | grep -q 'disabled in this build'; then
    have_sqlite=0
fi

timeout_cmd=""
if command -v timeout >/dev/null 2>&1; then
    timeout_cmd="timeout $TIMEOUT_SECONDS"
elif command -v gtimeout >/dev/null 2>&1; then
    timeout_cmd="gtimeout $TIMEOUT_SECONDS"
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/auspex-examples.XXXXXX")
trap 'rm -rf "$work"' EXIT

if [ $# -gt 0 ]; then
    examples=("$@")
else
    # Top-level examples/ and the phases/ walkthroughs. Subdirectories that
    # hold imported modules (examples/modules/) are deliberately not globbed.
    examples=("$ROOT"/examples/*.apx "$ROOT"/examples/phases/*.apx)
fi

passed=0
failed=0
skipped=0
failures=""

# Print the value of each "// smoke: <key> <value>" line in a file, one per
# line (an empty line for a bare "// smoke: <key>").
directive() { # file key
    awk -v key="$2" '
        match($0, /^[ \t]*\/\/[ \t]*smoke:[ \t]*/) {
            rest = substr($0, RLENGTH + 1)
            word = rest; sub(/[ \t].*$/, "", word)
            if (word != key) next
            sub(/^[^ \t]*[ \t]*/, "", rest); sub(/[ \t]+$/, "", rest)
            print rest
        }' "$1"
}

has_directive() { # file key
    [ "$(directive "$1" "$2" | wc -l | tr -d ' ')" -gt 0 ]
}

for example in "${examples[@]}"; do
    name=$(basename "$example" .apx)
    # Path of the example inside the copied examples/ tree, e.g.
    # examples/phases/phase7.apx — setup files resolve next to it.
    rel=${example#"$ROOT"/}
    rel_dir=$(dirname "$rel")

    if has_directive "$example" skip; then
        echo "SKIP  $name ($(directive "$example" skip | head -n 1))"
        skipped=$((skipped + 1))
        continue
    fi
    reason=""
    for req in $(directive "$example" requires); do
        case $req in
            sqlite) [ $have_sqlite -eq 1 ] || reason="needs a SQLite build" ;;
            linux)  [ "$(uname -s)" = Linux ] || reason="needs Linux" ;;
            *)      reason="unknown requirement '$req'" ;;
        esac
    done
    if [ -n "$reason" ]; then
        echo "SKIP  $name ($reason)"
        skipped=$((skipped + 1))
        continue
    fi

    runs=$(directive "$example" args)
    [ -n "$runs" ] || runs=" "
    while IFS= read -r args; do
        label=$name
        [ -n "${args// /}" ] && label="$name $args"
        dir=$work/$name.$((passed + failed))
        mkdir -p "$dir"
        cp -R "$ROOT/examples" "$dir/"

        ok=1
        for setup in $(directive "$example" setup); do
            if ! (cd "$dir" && $timeout_cmd "$AUSPEX" "$rel_dir/$setup" </dev/null >"$dir/setup.out" 2>&1); then
                ok=0
                cp "$dir/setup.out" "$dir/out"
                break
            fi
        done
        if [ $ok -eq 1 ]; then
            # shellcheck disable=SC2086 # args is a word list from the directive
            (cd "$dir" && $timeout_cmd "$AUSPEX" $args "$rel" </dev/null >"$dir/out" 2>&1) || ok=0
        fi

        if [ $ok -eq 1 ]; then
            echo "PASS  $label"
            passed=$((passed + 1))
        else
            echo "FAIL  $label"
            sed 's/^/      | /' "$dir/out" | tail -n 15
            failed=$((failed + 1))
            failures="$failures $label;"
        fi
    done <<EOF
$runs
EOF
done

echo "examples: $passed passed, $failed failed, $skipped skipped"
if [ $failed -gt 0 ]; then
    echo "failed:$failures" >&2
    exit 1
fi
