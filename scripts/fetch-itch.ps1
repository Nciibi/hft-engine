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
# Usage: fetch-itch.ps1 [out_path] [bytes]
#
# The default is 4 MiB compressed, which is about 10 MiB of messages:
# enough to get past the session's directory and system messages and
# into the trading.

param(
    [string]$Out = "itch_raw.itch",
    [int]$Bytes = 4194304
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Url = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"

if ((Test-Path -LiteralPath $Out) -and (Get-Item -LiteralPath $Out).Length -gt 0) {
    Write-Host "already present: $Out ($((Get-Item -LiteralPath $Out).Length) bytes)"
    exit 0
}

$tmp = "$Out.gz"
Write-Host "fetching $Bytes compressed bytes from the Nasdaq ITCH archive"

try {
    $request = [System.Net.HttpWebRequest]::Create($Url)
    $request.AddRange(0, $Bytes - 1)
    $request.UserAgent = "curl/8"
    $request.Timeout = 120000
    $response = $request.GetResponse()
    $input = $response.GetResponseStream()
    $compressed = New-Object System.IO.MemoryStream
    $input.CopyTo($compressed)
    $input.Close()
    $response.Close()
} catch {
    Write-Host "the download failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue }
    exit 1
}

[System.IO.File]::WriteAllBytes($tmp, $compressed.ToArray())

# A truncated gzip stream is expected: we asked for a prefix. The
# decompressor complains about the missing tail and still writes
# everything it decoded, which is exactly the prefix we want.
$source = [System.IO.File]::OpenRead($tmp)
$gzip = New-Object System.IO.Compression.GZipStream($source, [System.IO.Compression.CompressionMode]::Decompress)
$target = [System.IO.File]::Create($Out)
try {
    $gzip.CopyTo($target)
} catch {
    # The tail is missing. Everything decoded so far is already written.
} finally {
    $target.Close()
    $gzip.Close()
    $source.Close()
    Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
}

$size = (Get-Item -LiteralPath $Out).Length
if ($size -eq 0) {
    Write-Host "the archive decompressed to nothing" -ForegroundColor Red
    Remove-Item -LiteralPath $Out -ErrorAction SilentlyContinue
    exit 1
}

Write-Host "wrote $Out ($size bytes)"

