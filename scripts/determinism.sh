#!/usr/bin/env bash
#
# Verify that the book checksum is identical across optimisation levels.
#
# README.md claims the same capture replayed at -O0, -O2, -O3, -Os and
# -Oz produces an identical FNV-1a book checksum. The `determinism` CTest
# runs hft_replay twice from one binary, which proves the engine is
# repeatable within a build and says nothing about whether the optimiser
# changes the answer.
#
# That distinction is the point. The bug this repository is built around
# -- reading the share count at the price offset -- passed every test the
# project had, because the generator and the decoder agreed with each
# other. Self-consistency is not correctness, and the same holds for a
# determinism claim checked only against itself at one optimisation
# level.
#
# Run this on the benchmark host, not a laptop, and treat a failure as
# disqualifying for every latency figure in README.md: if the program is
# not the same program twice, the timings are not comparable.

set -euo pipefail

RECORDS="${1:-50000}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"

# -Os and -Oz are the size optimisations. They are here because they
# diverge from the speed ones in ways that have historically changed
# floating-point and vectorisation behaviour.
LEVELS=(O0 O1 O2 O3 Os Oz)

SOURCES=(
    "${ROOT}/src/replay/replay.cpp"
    "${ROOT}/src/types.cpp"
    "${ROOT}/src/itch/decode.cpp"
    "${ROOT}/src/lob/order_book.cpp"
    "${ROOT}/src/feed/generator.cpp"
    "${ROOT}/src/util/affinity.cpp"
)

OUT="$(mktemp -d)"
trap 'rm -rf "${OUT}"' EXIT

fail() {
    echo
    echo "FAILED: $1" >&2
    echo >&2
    echo "A checksum that varies with -O level means the engine depends on" >&2
    echo "something the optimiser is entitled to change: uninitialised reads," >&2
    echo "strict-aliasing violations, signed overflow, or iteration over an" >&2
    echo "unordered container. All four are real defects, and none of them" >&2
    echo "is caught by running one binary twice." >&2
    exit 1
}

echo "=== cross-optimisation determinism ==="
echo
echo "records      : ${RECORDS}"
echo "compiler     : ${CXX}"
echo "optimisation : ${LEVELS[*]}"
echo

declare -A CHECKSUM
declare -A RECORDS_SEEN
declare -A GAPS

for level in "${LEVELS[@]}"; do
    exe="${OUT}/hft_replay_${level}"
    echo "building -${level} ..."
    # -DNDEBUG at every level: the variable under test is the optimisation
    # level, not whether assertions are live, and mixing them makes a
    # failure ambiguous.
    "${CXX}" -std=c++20 "-${level}" -DNDEBUG \
        -I "${ROOT}/include" -I "${ROOT}/src" \
        "${SOURCES[@]}" -o "${exe}"

    output="$("${exe}" "${RECORDS}")" || fail "hft_replay exited non-zero at -${level}"

    checksum="$(sed -n 's/.*BOOK CHECKSUM[[:space:]]\+\([0-9a-fA-F]\{16\}\).*/\1/p' <<<"${output}" | tr 'A-Z' 'a-z')"
    [[ -n "${checksum}" ]] || { echo "${output}"; fail "no BOOK CHECKSUM line at -${level}"; }

    # The message count must match too: a run that read fewer messages
    # could agree on a checksum while having done less work.
    count="$(sed -n 's/^records[[:space:]]\+\([0-9]\+\)[[:space:]]*$/\1/p' <<<"${output}")"
    gaps="$(sed -n 's/.*sequence gaps[[:space:]]\+\([0-9]\+\).*/\1/p' <<<"${output}")"

    CHECKSUM["${level}"]="${checksum}"
    RECORDS_SEEN["${level}"]="${count:-?}"
    GAPS["${level}"]="${gaps:-?}"
    echo "  -${level} checksum ${checksum}  over ${count:-?} records, ${gaps:-?} gaps"
done

echo

BASELINE="${CHECKSUM[O2]}"
for level in "${LEVELS[@]}"; do
    if [[ "${CHECKSUM[${level}]}" != "${BASELINE}" ]]; then
        echo "checksum mismatch: -${level} = ${CHECKSUM[${level}]}, -O2 = ${BASELINE}"
        fail "the book checksum is not invariant across optimisation levels"
    fi
    if [[ "${RECORDS_SEEN[${level}]}" != "${RECORDS_SEEN[O2]}" ]]; then
        fail "message count differs between -${level} and -O2"
    fi
    if [[ "${GAPS[${level}]}" != "0" ]]; then
        fail "sequence gaps at -${level} (${GAPS[${level}]}); the capture did not replay cleanly"
    fi
done

echo "PASSED: identical book checksum ${BASELINE} across ${LEVELS[*]}"
echo
echo "This is what makes the determinism claim in README.md checkable rather"
echo "than asserted. Cross-host determinism is the same command on the"
echo "benchmark machine; the ISA is pinned so that comparison means something."
