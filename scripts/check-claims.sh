#!/usr/bin/env bash
# Verify that the numbers this repository claims are the numbers its
# binaries actually produce. See scripts/check-claims.ps1 for the full
# rationale; this is the same check for the Linux CI runner.
#
# Why it exists: the README stated "783 checks" for as long as the
# binaries printed 1,412, and nothing noticed. A number typed by hand
# into prose has nothing to disagree with it.
#
# Usage: check-claims.sh [build_dir]
# Exits non-zero on any mismatch.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname -- "$SCRIPT_DIR")"
BUILD="${1:-$ROOT/build}"

# The five suites that print an "N checks, M failures" summary line. The
# differential suites assert correctness via exit status and are already
# covered by ctest.
SUITES="unit:hft_test risk_oms:hft_risk_oms strategy:hft_strategy concurrent:hft_concurrent shards:hft_shards"

echo "claim check"
echo "  build ......... $BUILD"

problems=()
declare -A actual
total=0

for entry in $SUITES; do
    key="${entry%%:*}"
    name="${entry##*:}"
    bin="$BUILD/$name"

    if [[ ! -x "$bin" ]]; then
        problems+=("missing binary: $bin -- build the tree first")
        continue
    fi

    out="$("$bin" 2>&1 || true)"
    line="$(printf '%s\n' "$out" | grep -Eo '[0-9]+ checks?, [0-9]+ failures?' | tail -n 1 || true)"

    if [[ -z "$line" ]]; then
        problems+=("$key: no summary line -- the output format changed")
        continue
    fi

    checks="${line%% checks*}"
    failures="${line##*, }"
    failures="${failures%% *}"
    actual[$key]="$checks"
    total=$((total + checks))

    printf '  %-12s %6s checks, %s failures\n' "$key" "$checks" "$failures"
    if [[ "$failures" -ne 0 ]]; then
        problems+=("$key reports $failures failures")
    fi
done

ctest_count=0
if command -v ctest >/dev/null 2>&1; then
    ctest_count="$(ctest --test-dir "$BUILD" -N 2>&1 | grep -cE '^[[:space:]]*Test[[:space:]]+#[0-9]+:' || true)"
    printf '  %-12s %6s suites\n' "ctest" "$ctest_count"
else
    echo "  ctest not found, suite count unchecked"
fi

# The <!-- claims --> block is the machine-readable source of truth. The
# prose is checked separately below, because a count that is correct in the
# block and stale in the README is still a wrong README.
block_total=""
block_ctest=""
while IFS= read -r line; do
    case "$line" in
        checks_total:*) block_total="${line#*:}" ;;
        ctest_tests:*)   block_ctest="${line#*:}" ;;
    esac
done < <(sed -n '/<!--[[:space:]]*claims/,/-->/p' "$ROOT/README.md")

if [[ -z "$block_total" ]]; then
    problems+=("no <!-- claims --> block found in README.md")
else
    block_total="$(printf '%s' "$block_total" | tr -d '[:space:]')"
    printf '  %-12s %6s\n' "claimed total" "$block_total"

    if [[ "$block_total" -ne "$total" ]]; then
        problems+=("README claims $block_total checks; the binaries print $total")
    fi
    if [[ -n "$block_ctest" && "$ctest_count" -gt 0 ]]; then
        block_ctest="$(printf '%s' "$block_ctest" | tr -d '[:space:]')"
        if [[ "$block_ctest" -ne "$ctest_count" ]]; then
            problems+=("README claims $block_ctest CTest suites; ctest reports $ctest_count")
        fi
    fi

    # Strip the invisible block, then require the visible prose to state the
    # same total with a thousands separator, as the README does.
    formatted="$(printf '%d' "$block_total" | sed -e ':a' -e 's/\(.*[0-9]\)\([0-9]\{3\}\)\(,\|$\)/\1,\2\3/;ta')"
    if ! sed '/<!--[[:space:]]*claims/,/-->/d' "$ROOT/README.md" | grep -qF "$formatted"; then
        problems+=("README prose never states '$formatted' checks outside the claims block")
    fi
fi

if [[ ${#problems[@]} -eq 0 ]]; then
    echo
    echo "OK: every published count matches what the binaries print."
    exit 0
fi

echo
echo "FAILED:"
for p in "${problems[@]}"; do echo "  - $p"; done
exit 1