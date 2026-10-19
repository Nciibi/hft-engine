#!/usr/bin/env bash
# Fetch a slice of REAL Nasdaq TotalView-ITCH 5.0 market data.
#
# The exchange publishes full-day files, free and with no registration:
#
#   https://emi.nasdaq.com/ITCH/Nasdaq ITCH/
#
# A full day is several gigabytes, which is more than a test needs and
# more than a CI runner should pull. The server honours HTTP Range
# requests, so this fetches the first N COMPRESSED bytes, which
# decompress to the first N' bytes of the session.
#
# The result is a RAW ITCH stream: message after message, each a
# two-byte length followed by a body, with no envelope. It starts at a
# message boundary, because the gzip stream starts at the file start and
# a prefix of a decompressed stream is a prefix of the messages.
#
# The output is the exchange's data and is NOT committed to this
# repository. It is fetched on demand, and the tests that use it skip
# themselves when it is not there.
#
# Usage: fetch-itch.sh [compressed_bytes] [output_path]
#
# The default is 4 MiB compressed, which is about 10 MiB of messages:
# enough to get past the session's directory and system messages and
# into the trading.

set -euo pipefail

BYTES="${1:-4194304}"
OUT="${2:-itch_raw.itch}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"

if [[ -s "$OUT" ]]; then
    echo "already present: $OUT ($(wc -c < "$OUT") bytes)"
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required" >&2
    exit 1
fi

TMP="${OUT}.gz"
echo "fetching $BYTES compressed bytes from the Nasdaq ITCH archive"
if ! curl --silent --show-error --location --fail \
        --range "0-$((BYTES - 1))" --output "$TMP" "$URL"; then
    echo "the download failed" >&2
    rm -f "$TMP"
    exit 1
fi

GOT=$(wc -c < "$TMP")
if [[ "$GOT" -eq 0 ]]; then
    echo "the download produced nothing" >&2
    rm -f "$TMP"
    exit 1
fi

# A truncated gzip stream is expected: we asked for a prefix. gzip
# complains about the missing tail and still writes everything it
# decoded, which is exactly the prefix we want.
gzip -dc "$TMP" > "$OUT" 2>/dev/null || true
rm -f "$TMP"

SIZE=$(wc -c < "$OUT")
if [[ "$SIZE" -eq 0 ]]; then
    echo "the archive decompressed to nothing" >&2
    rm -f "$OUT"
    exit 1
fi

echo "wrote $OUT ($SIZE bytes)"
