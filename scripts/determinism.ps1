<#
.SYNOPSIS
    Verify that the book checksum is identical across optimisation levels.

.DESCRIPTION
    README.md claims the same capture replayed at -O0, -O2, -O3, -Os and
    -Oz produces an identical FNV-1a book checksum. Until now nothing
    checked that. The `determinism` CTest runs hft_replay twice from one
    binary, which proves the engine is repeatable within a build and says
    nothing about whether the optimiser changes the answer.

    That distinction is the whole point of the claim. The bug this
    repository is built around -- reading the share count at the price
    offset -- passed every test the project had, because the generator
    and the decoder agreed with each other. Self-consistency is not
    correctness, and the same is true of a determinism claim checked only
    against itself at one optimisation level: it would pass unchanged for
    a build that fat-fingered an uninitialised read into a value the
    optimiser happened to zero at every level tried.

    So: build hft_replay at each level, replay the same generated capture
    in each, and require every checksum to match. Exits non-zero on the
    first disagreement.

    -DNDEBUG is passed at every level deliberately. The variable under
    test is the optimisation level, not whether assertions are live, and
    mixing the two would make a failure ambiguous.

.PARAMETER Records
    Messages to replay. Lower is faster; the default is enough to cover
    every mutation kind the generator produces.

.PARAMETER Compiler
    C++ compiler. Defaults to g++ on PATH.

.EXAMPLE
    .\scripts\determinism.ps1
    .\scripts\determinism.ps1 -Records 200000
#>
[CmdletBinding()]
param(
    [int]$Records = 50000,
    [string]$Compiler = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrEmpty($Compiler)) { $Compiler = 'g++' }

# -Os and -Oz are the size optimisations. They are here precisely
# because they diverge from the speed ones in ways that have historically
# changed floating-point and vectorisation behaviour.
$levels = @('O0', 'O1', 'O2', 'O3', 'Os', 'Oz')

$sources = @(
    'src/replay/replay.cpp',
    'src/types.cpp',
    'src/itch/decode.cpp',
    'src/lob/order_book.cpp',
    'src/feed/generator.cpp',
    'src/util/affinity.cpp'
)

$outDir = Join-Path $env:TEMP ('hft-determinism-' + $PID)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Write-Fail([string]$Message) {
    Write-Host ''
    Write-Host "FAILED: $Message" -ForegroundColor Red
    Write-Host ''
    Write-Host 'A checksum that varies with -O level means the engine depends on'
    Write-Host 'something the optimiser is entitled to change: uninitialised reads,'
    Write-Host 'strict-aliasing violations, signed overflow, or iteration over an'
    Write-Host 'unordered container. All four are real defects, and none of them'
    Write-Host 'is caught by running one binary twice.' -ForegroundColor Red
    exit 1
}

Write-Host '=== cross-optimisation determinism ==='
Write-Host ''
Write-Host ("records        : {0}" -f $Records)
Write-Host ("compiler       : {0}" -f $Compiler)
Write-Host ("optimisation   : {0}" -f ($levels -join ', '))
Write-Host ''

$results = @{}

foreach ($level in $levels) {
    $exe = Join-Path $outDir ("hft_replay_$level.exe")
    $args = @(
        "-std=c++20", "-$level", '-DNDEBUG',
        '-I', (Join-Path $root 'include'),
        '-I', (Join-Path $root 'src')
    ) + ($sources | ForEach-Object { Join-Path $root $_ }) + @('-o', $exe)

    Write-Host ("building -{0} ..." -f $level)
    & $Compiler @args 2>&1 | ForEach-Object {
        if ($_ -match '\S') { Write-Host "  $_" -ForegroundColor DarkGray }
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Fail "compilation failed at -$level"
    }

    $output = & $exe $Records 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($output -join "`n")
        Write-Fail "hft_replay exited non-zero at -$level"
    }

    # The checksum line, and the message count it was taken over. Both
    # must match: a run that read fewer messages could agree on a
    # checksum while having done less work.
    $checksum = ($output | Select-String -Pattern 'BOOK CHECKSUM\s+([0-9a-fA-F]+)')
    if (-not $checksum) {
        Write-Host ($output -join "`n")
        Write-Fail "no BOOK CHECKSUM line at -$level"
    }
    $hash = ($checksum.Matches[0].Groups[1].Value).ToLower()

    $records = ($output | Select-String -Pattern 'records\s+(\d+)')
    $count = if ($records) { $records.Matches[0].Groups[1].Value } else { '?' }

    $gaps = ($output | Select-String -Pattern 'sequence gaps\s+(\d+)')
    $gapCount = if ($gaps) { $gaps.Matches[0].Groups[1].Value } else { '?' }

    $results[$level] = @{ checksum = $hash; records = $count; gaps = $gapCount }
    Write-Host ("  -{0,-3} checksum {1}  over {2} records, {3} gaps" -f $level, $hash, $count, $gapCount)
}

Write-Host ''

# Baseline is O2, the level the published numbers are taken at.
$baseline = $results['O2']
foreach ($level in $levels) {
    if ($results[$level].checksum -ne $baseline.checksum) {
        Write-Host ("checksum mismatch: -{0} = {1}, -O2 = {2}" -f $level, $results[$level].checksum, $baseline.checksum)
        Write-Fail "the book checksum is not invariant across optimisation levels"
    }
    if ($results[$level].records -ne $baseline.records) {
        Write-Fail "message count differs between -$level and -O2 ($($results[$level].records) vs $($baseline.records))"
    }
    if ($results[$level].gaps -ne '0') {
        Write-Fail "sequence gaps at -$level ($($results[$level].gaps)); the capture did not replay cleanly"
    }
}

Write-Host ("PASSED: identical book checksum {0} across {1}" -f $baseline.checksum, ($levels -join ', '))
Write-Host ''
Write-Host 'This is what makes the determinism claim in README.md checkable rather'
Write-Host 'than asserted. It does not make it cross-host: that needs the same run'
Write-Host 'on the benchmark machine, and the ISA is pinned so that the comparison'
Write-Host 'means something.'

Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue
exit 0
