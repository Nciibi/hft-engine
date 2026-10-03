#!/usr/bin/env bash
# Markdown link and anchor consistency check. POSIX counterpart to
# scripts/check-docs.ps1 -- keep the two in step.
#
# Exists because this repository's documentation accreted corrections
# rather than replacing them, and three separate sections came to
# contradict each other while every link between them still resolved. A
# broken anchor is the same failure in a smaller costume: the reader is
# sent confidently to a place that no longer says what the link claims.
set -uo pipefail

cd "$(dirname "$0")/.."

FILES=(
    README.md
    docs/BUGS.md
    docs/DESIGN.md
    docs/RESULTS.md
    results/ENVIRONMENT.md
    results/OPTIMIZATION.md
)

broken=0
checked=0

# GitHub's anchor rules: lowercase, drop punctuation except word
# characters, spaces to hyphens.
anchor_of() {
    printf '%s' "$1" \
        | tr '[:upper:]' '[:lower:]' \
        | sed -e 's/[^[:alnum:]_ -]//g' -e 's/  */-/g'
}

anchors_of() {
    # Heading anchors in one file, one per line.
    grep -E '^#{1,6} ' "$1" 2>/dev/null \
        | sed -E 's/^#{1,6}[[:space:]]+//' \
        | sed -E 's/[[:space:]]+$//' \
        | while IFS= read -r h; do anchor_of "$h"; done
}

for file in "${FILES[@]}"; do
    if [[ ! -f "$file" ]]; then
        echo "  MISSING FILE: $file"
        broken=$((broken + 1))
        continue
    fi

    dir=$(dirname "$file")
    own=$(anchors_of "$file")

    while IFS= read -r target; do
        [[ -z "$target" ]] && continue
        case "$target" in
            http:*|https:*|mailto:*) continue ;;
        esac

        checked=$((checked + 1))

        if [[ "$target" == \#* ]]; then
            frag="${target#\#}"
            if ! grep -qxF -- "$frag" <<<"$own"; then
                echo "  ${file}: anchor '#${frag}' has no matching heading"
                broken=$((broken + 1))
            fi
            continue
        fi

        path="${target%%#*}"
        frag=""
        if [[ "$target" == *#* ]]; then
            frag="${target#*#}"
        fi

        resolved="${dir}/${path}"
        if [[ ! -f "$resolved" ]]; then
            echo "  ${file}: target '${path}' does not exist"
            broken=$((broken + 1))
            continue
        fi

        if [[ -n "$frag" ]]; then
            if ! anchors_of "$resolved" | grep -qxF -- "$frag"; then
                echo "  ${file}: '${path}' has no heading matching '#${frag}'"
                broken=$((broken + 1))
            fi
        fi
    done < <(grep -oE '\]\([^)]+\)' "$file" 2>/dev/null \
             | sed -E 's/^\]\(//; s/\)$//')

    words=$(wc -w <"$file")
    lines=$(wc -l <"$file")
    printf '  %-26s %5d lines %6d words\n' "$file" "$lines" "$words"
done

if [[ "$broken" -eq 0 ]]; then
    echo
    echo "OK: ${checked} links checked, every target and anchor resolves."
    exit 0
fi

echo
echo "FAILED: ${broken} broken link(s) or anchor(s) out of ${checked} checked."
exit 1