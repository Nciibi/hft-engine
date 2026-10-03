# A/B throughput harness for optimisation work.
#
# Why this exists
# ---------------
# An optimisation that cannot be measured is not an optimisation, it is a
# guess with a build attached. This repository already has that bug in its
# history: a concurrency benchmark once reported a confident 60x speedup
# that was two threads racing on one queue index, and another reported a
# 20% slowdown that was a drain loop exiting early.
#
# The reason this harness is necessary rather than convenient is that the
# measurement is genuinely noisy. Measured on the development host, seven
# identical `hft_bench` runs of 800,000 messages spanned 17.5% -- 2.13M to
# 2.50M msg/s -- with a single obvious outlier. A naive before/after pair
# would call a 10% "improvement" a win roughly half the time.
#
# So the statistics here are chosen for robustness rather than
# convenience:
#
#   * MEDIAN, not mean. One run at 2.13M would drag a mean far enough to
#     invert the verdict.
#   * MAD (median absolute deviation), not standard deviation. The outlier
#     inflates a standard deviation by exactly the square of its distance,
#     which would inflate the noise threshold until every real effect
#     looked insignificant.
#   * A significance threshold, and a verdict of NO CHANGE when the delta
#     falls inside it. Reporting "no measurable difference" is the whole
#     point; a harness that always declares a winner is how a regression
#     gets committed.
#
# Usage
# -----
#   scripts/ab.ps1 -Label flatmap-interleave -Save        # becomes the baseline
#   scripts/ab.ps1 -Label flatmap-interleave              # compare, do not save
#   scripts/ab.ps1 -Label check -BuildDir build-clang      # other compiler
#   scripts/ab.ps1 -List
#
# Throughput is the metric, deliberately, and not latency percentiles:
# `hft_tsc_bench` establishes that this host's clock has 100ns
# granularity, which makes a per-stage p50 or p999 unmeasurable here.
# Throughput is computed through a correct conversion and converges, so it
# is the one performance figure that can be trusted on the development
# machine. Absolute latency waits for the benchmark host.

param(
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$BuildDir = 'build',
    [int]$Messages = 800000,
    [int]$Runs = 9,
    [switch]$Save,
    [switch]$List,
    [string]$Store = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent (Resolve-Path -LiteralPath $PSCommandPath).Path)

# Paths are resolved against the repo root explicitly. `Push-Location`
# changes PowerShell's location but NOT the .NET current directory, and
# [System.IO.File] resolves relative paths against that instead -- the
# same trap scripts/check-docs.ps1 documents.
Push-Location $RepoRoot

if ([string]::IsNullOrEmpty($Store)) {
    $Store = Join-Path $RepoRoot 'results/optimization.json'
}

function Get-Stats([double[]]$values) {
    $s = @($values | Sort-Object)
    $n = [int]$s.Count
    if ($n -eq 0) { throw 'no samples' }
    $half = [int][math]::Floor($n / 2)
    $median = if ($n % 2 -eq 1) { $s[$half] } else { ($s[$half - 1] + $s[$half]) / 2.0 }
    $devs = @($s | ForEach-Object { [math]::Abs($_ - $median) } | Sort-Object)
    $mad = if ($n % 2 -eq 1) { $devs[$half] } else { ($devs[$half - 1] + $devs[$half]) / 2.0 }
    # A floor of 1.5% so a suspiciously quiet machine cannot produce a
    # threshold so tight that rounding decides every verdict.
    $noisePct = if ($median -gt 0) { 100.0 * (2.0 * $mad) / $median } else { 100.0 }
    if ($noisePct -lt 1.5) { $noisePct = 1.5 }
    return [pscustomobject]@{
        median   = [double]$median
        mad      = [double]$mad
        noisePct = [double]$noisePct
        min      = [double]$s[0]
        max      = [double]$s[$n - 1]
    }
}

if ($List) {
    if (Test-Path -LiteralPath $Store) {
        $all = Get-Content -LiteralPath $Store -Raw -Encoding UTF8 | ConvertFrom-Json
        $all.PSObject.Properties | ForEach-Object {
            '{0,-28} {1,10:N0} msg/s   noise {2,5:N2}%   {3}' -f $_.Name,
            $_.Value.median, $_.Value.noisePct, $_.Value.note
        }
    } else {
        Write-Host 'no store yet'
    }
    Pop-Location
    exit 0
}

$exe = Join-Path (Join-Path $RepoRoot $BuildDir) 'hft_bench.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "missing $exe -- build first" -ForegroundColor Red
    Pop-Location
    exit 1
}

Write-Host "A/B: $Label   ($Runs runs x $Messages messages, $BuildDir)"
Write-Host '---------------------------------------------------------------'

$runs = @()
for ($i = 0; $i -lt $Runs; $i++) {
    $out = & $exe $Messages 2>&1
    $line = $out | Select-String -Pattern 'throughput\s+(\d+)' | Select-Object -First 1
    if ($null -eq $line) {
        Write-Host "run $i produced no throughput line" -ForegroundColor Red
        Pop-Location
        exit 1
    }
    $v = [double]$line.Matches[0].Groups[1].Value
    $runs += $v
    Write-Host ('  run {0,2}  {1,10:N0} msg/s' -f ($i + 1), $v)
    $errors = ($out | Select-String -Pattern 'WARNING|rejected\s+[1-9]|truncat').Count
    if ($errors -gt 0) {
        Write-Host "  WARNING: run $i reported a warning; the numbers above are not clean" -ForegroundColor Yellow
    }
}

$st = Get-Stats $runs
Write-Host ''
Write-Host ('  median  {0,10:N0} msg/s' -f $st.median)
Write-Host ('  range   {0,10:N0} .. {1:N0}  ({2:N2}% raw spread)' -f $st.min, $st.max,
    (100.0 * ($st.max - $st.min) / $st.min))
Write-Host ('  noise   {0,10:N2}%  (2 x MAD; a delta smaller than this is not a result)' -f $st.noisePct)

$store = @{}
if (Test-Path -LiteralPath $Store) {
    $store = Get-Content -LiteralPath $Store -Raw -Encoding UTF8 | ConvertFrom-Json
}

# Compare against the most recently stored label, which is the convention:
# you record a baseline, then measure the next thing against it.
$baselineName = $null
$baseline = $null
$store.PSObject.Properties | ForEach-Object {
    $baselineName = $_.Name
    $baseline = $_.Value
}

$verdict = 'BASELINE RECORDED'
if ($null -ne $baseline -and $baselineName -ne $Label) {
    $baseMedian = [double]$baseline.median
    $deltaPct = 100.0 * ($st.median - $baseMedian) / $baseMedian
    $threshold = [math]::Max($st.noisePct, [double]$baseline.noisePct)
    Write-Host ''
    Write-Host ('  baseline           {0}  {1:N0} msg/s' -f $baselineName, $baseMedian)
    Write-Host ('  delta              {0:+0.00;-0.00;0.00}%' -f $deltaPct)
    Write-Host ('  threshold          {0:N2}%' -f $threshold)

    if ([math]::Abs($deltaPct) -lt $threshold) {
        $verdict = 'NO MEASURABLE CHANGE'
    } elseif ($deltaPct -gt 0) {
        $verdict = 'IMPROVEMENT'
    } else {
        $verdict = 'REGRESSION'
    }
    Write-Host ''
    Write-Host "  VERDICT: $verdict" -ForegroundColor $(if ($verdict -eq 'REGRESSION') { 'Red' } elseif ($verdict -eq 'IMPROVEMENT') { 'Green' } else { 'Yellow' })
}

if ($Save) {
    $store | Add-Member -NotePropertyName $Label -NotePropertyValue ([pscustomobject]@{
            median   = $st.median
            noisePct = $st.noisePct
            min      = $st.min
            max      = $st.max
            runs     = $Runs
            messages = $Messages
            buildDir = $BuildDir
            note     = "$(Get-Date -Format 'yyyy-mm-dd') $verdict"
        }) -Force
    $ordered = [ordered]@{}
    $store.PSObject.Properties | ForEach-Object { $ordered[$_.Name] = $_.Value }
    [System.IO.File]::WriteAllText($Store, ($ordered | ConvertTo-Json -Depth 5),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "  saved to $([System.IO.Path]::GetFileName($Store))"
}

Pop-Location
exit 0