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
#
# `throughput` mode, not the default, and that is the important part: the
# instrumented loop takes three QPC reads per message at ~27ns each, which
# measured as 26% of the reported throughput. Judging an optimisation
# against a number that is a quarter clock reads means every result is
# diluted by the same constant -- and it would hide a real win as easily as
# it would inflate a real loss.
param(
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$BuildDir = 'build',
    [int]$Messages = 800000,
    [int]$Runs = 11,
    [switch]$Save,
    [switch]$List,
    [string]$Mode = 'throughput',
    [string]$Store = '',
    # Paired mode. Alternate the two builds run-by-run and compare WITHIN
    # each pair, so machine drift is common-mode and cancels.
    [string]$CompareBuildDir = ''
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
    # Threshold floor of 3%. The median and MAD are robust to outliers,
    # but the *machine* is not quiet: nine runs of 800,000 messages on the
    # development host have spanned 17% raw, with occasional runs 12% below
    # the cluster and almost certainly a background process rather than the
    # engine. A threshold derived only from MAD came out at 1.5%, which
    # would call a 2% "improvement" a result whenever both measurements
    # happened to land in a quiet patch. 3% is chosen because it is above
    # the observed within-cluster variation and below any change worth
    # having made to the hot path.
    $noisePct = if ($median -gt 0) { 100.0 * (2.0 * $mad) / $median } else { 100.0 }
    if ($noisePct -lt 3.0) { $noisePct = 3.0 }
    # Trimmed spread: raw range with the single best and worst run removed.
    # The gap between this and the raw range is the size of the outliers,
    # which is the honest way to show that a machine has them. Sorted
    # ascending, so the high end is s[n-2] and the low end is s[1].
    $trim = if ($n -ge 5) { ($s[$n - 2] - $s[1]) } else { ($s[$n - 1] - $s[0]) }
    return [pscustomobject]@{
        median      = [double]$median
        mad         = [double]$mad
        noisePct    = [double]$noisePct
        min         = [double]$s[0]
        max         = [double]$s[$n - 1]
        trimmedPct  = if ($median -gt 0) { 100.0 * $trim / $median } else { 100.0 }
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

function Measure-One([string]$exePath, [int]$messages, [string]$mode) {
    $out = & $exePath $messages $mode 2>&1

    if ($mode -eq 'stage') {
        # `hft_stage_bench` prints two throughput lines: shallow then deep.
        # The DEEP one is taken, and the choice matters more than it looks.
        #
        # `hft_bench` sizes its pools from the message count and never
        # removes an order, so its book grows without bound and its cost per
        # message never converges -- measured at 180, 119 and 91 ns/message
        # at 1.6M, 3.2M and 6.4M messages, still falling. It is measuring
        # pool page-faulting, not the engine.
        #
        # `hft_stage_bench` caps live orders at the level count, so its book
        # is bounded and its throughput is flat: 2.99M, 2.86M and 2.87M
        # msg/s at the same three sizes, a 4% spread across a 16x range.
        # That is an instrument. `hft_bench` is not.
        $vals = @()
        foreach ($l in ($out | Select-String 'throughput')) {
            # The line is `throughput  2 855 380 msg/s   175.108 ms`, so the
            # number carries space separators and the line carries a second
            # number. Naively stripping non-digits concatenates them into
            # nonsense -- which is exactly what an earlier version of this
            # script's ad-hoc callers did.
            if ($l.ToString() -match 'throughput\s+([\d\s]+?)msg/s') {
                $vals += [double](($Matches[1] -replace '\D', ''))
            }
        }
        if ($vals.Count -lt 2) {
            throw "could not find both throughput lines in stage output"
        }
        return $vals[$vals.Count - 1]
    }

    $line = $out | Select-String -Pattern 'throughput\s+(\d+)' | Select-Object -First 1
    if ($null -eq $line) {
        throw "no throughput line from $exePath"
    }
    $noise = ($out | Select-String -Pattern 'WARNING|rejected\s+[1-9]')
    if ($noise) {
        throw ("run reported a warning: " + ($noise | Select-Object -First 1))
    }
    return [double]$line.Matches[0].Groups[1].Value
}

# ---- Paired mode -------------------------------------------------------
#
# Run A and B alternately and compare WITHIN each pair.
#
# This exists because the machine is not quiet enough to resolve the
# effects being looked for with independent runs. Nine unpaired runs of the
# same binary once spanned 11% raw and 8% trimmed, and a 6% improvement --
# a plausible size for `-march=native` -- landed inside that noise and was
# reported as no change.
#
# Pairing fixes it structurally rather than by taking more samples. Within
# one pair both binaries run seconds apart, so anything slow-moving on the
# host -- a thermal ramp, a background indexer, another user's process --
# is common to both members and cancels in the ratio. What survives is
# the difference between the two builds, which is what was being measured
# in the first place. Eleven pairs then resolve an effect several times
# smaller than the run-to-run spread, which eleven unpaired runs never can.
if ($CompareBuildDir -ne '') {
    $exeB = Join-Path (Join-Path $RepoRoot $CompareBuildDir) 'hft_bench.exe'
    if (-not (Test-Path -LiteralPath $exeB)) {
        Write-Host "missing $exeB -- build it first" -ForegroundColor Red
        Pop-Location
        exit 1
    }

    Write-Host "A/B PAIRED: $Label"
    Write-Host "  A = $BuildDir (portable ISA)   B = $CompareBuildDir"
    Write-Host "  $Runs pairs x $Messages messages, $Mode mode"
    Write-Host '---------------------------------------------------------------'

    $deltas = @()
    $aVals = @()
    $bVals = @()
    for ($i = 0; $i -lt $Runs; $i++) {
        # A then B, every pair, so neither build is systematically first
        # against a machine whose load is drifting in one direction.
        $a = Measure-One $exe $Messages $Mode
        $b = Measure-One $exeB $Messages $Mode
        $d = 100.0 * ($b - $a) / $a
        $deltas += $d
        $aVals += $a
        $bVals += $b
        Write-Host ('  pair {0,2}   A {1,10:N0}   B {2,10:N0}   {3:+0.00;-0.00;0.00}%' -f ($i + 1), $a, $b, $d)
    }

    $ds = Get-Stats $deltas
    $as = Get-Stats $aVals
    $bs = Get-Stats $bVals

    # Threshold for a PAIRED delta, in absolute percentage points.
    #
    # Get-Stats' noisePct is `2 * MAD / median`, which is correct for a
    # metric whose median is the quantity being compared against a stored
    # baseline -- and wrong here. For a paired delta the median IS the
    # effect, so dividing its spread by the effect measures the effect's
    # own size rather than the noise. A +5% median with two +15%
    # excursions produced a "threshold" of 133%, which would have retired
    # the experiment rather than reporting it.
    #
    # For paired data the spread of the deltas is the noise, in percentage
    # points, with no division.
    $deltaNoise = 2.0 * $ds.mad
    $thresh = [math]::Max($deltaNoise, 2.0)

    # Sign test. On a machine this noisy, no threshold derived from spread
    # is trustworthy, but the *direction* of every pair is. Eleven pairs
    # all favouring one build is 2^-11 under a null of no difference --
    # about one chance in two thousand -- and that statement does not
    # depend on the magnitude of any single run. It is the strongest
    # evidence available here, so it is reported rather than left implicit.
    $wins = @($deltas | Where-Object { $_ -gt 0 }).Count
    $losses = $deltas.Count - $wins
    # Two-sided binomial tail probability: P(X >= max(wins,losses)) under a
    # fair coin. C(n,k) is computed by hand because [math]::Comb does not
    # exist in .NET Framework 4.x, which is what PowerShell 5.1 runs on --
    # and a helper script that only works on PowerShell 7 is a helper
    # script that does not run here.
    $tail = [math]::Max($wins, $losses)
    $signP = 0.0
    for ($k = $tail; $k -le $deltas.Count; $k++) {
        $c = 1.0
        for ($j = 1; $j -le $k; $j++) {
            $c = $c * ($deltas.Count - $k + $j) / $j
        }
        $signP += $c * [math]::Pow(0.5, $deltas.Count)
    }
    $signP = [math]::Min(1.0, $signP)

    Write-Host ''
    Write-Host ('  A median  {0,10:N0} msg/s   (raw {1:N2}%, trimmed {2:N2}%)' -f $as.median,
        (100.0 * ($as.max - $as.min) / $as.min), $as.trimmedPct)
    Write-Host ('  B median  {0,10:N0} msg/s   (raw {1:N2}%, trimmed {2:N2}%)' -f $bs.median,
        (100.0 * ($bs.max - $bs.min) / $bs.min), $bs.trimmedPct)
    Write-Host ('  paired delta (median)  {0:+0.00;-0.00;0.00} percentage points' -f $ds.median)
    Write-Host ('  delta spread           {0:N2} pp (2 x MAD);  threshold {1:N2} pp' -f $deltaNoise, $thresh)
    Write-Host ('  sign test              {0} of {1} pairs favour B;  p = {2:N5} under a null of no difference' -f
        $wins, $deltas.Count, $signP)

    # The verdict separates two questions that are genuinely different:
    # WHICH build is faster, and BY HOW MUCH.
    #
    # On a host whose run-to-run spread is 6-8%, the direction can be
    # established far more confidently than the magnitude: 14 of 15 pairs
    # favouring one build gives p = 0.0005, while the same data leaves the
    # median delta sitting just inside the spread. Collapsing both into one
    # verdict would either retire real improvements or overstate their
    # size, so they are reported separately.
    #
    # Significance, not unanimity, is the sign criterion. An earlier
    # version demanded every pair agree, which silently discarded a
    # 14/15 result as inconclusive on a technicality rather than on the
    # evidence.
    $bySize = [math]::Abs($ds.median) -ge $thresh
    $bySign = $signP -lt 0.05

    if ($bySize -and $bySign -and $ds.median -gt 0) {
        $verdict = 'IMPROVEMENT'
    } elseif ($bySize -and $bySign) {
        $verdict = 'REGRESSION'
    } elseif ($bySign) {
        $verdict = $(if ($ds.median -gt 0) { 'IMPROVEMENT -- direction significant, size inside noise' }
        else { 'REGRESSION -- direction significant, size inside noise' })
    } else {
        $verdict = 'NO MEASURABLE CHANGE'
    }
    Write-Host ''
    Write-Host "  VERDICT: $verdict" -ForegroundColor $(if ($verdict -like 'REGRESSION*') { 'Red' } elseif ($verdict -like 'IMPROVEMENT*') { 'Green' } else { 'Yellow' })

    if ($Save) {
        Write-Host '  (paired results are not written to the store; the store holds'
        Write-Host '   unpaired medians, which are not comparable to a paired delta)'
    }
    Pop-Location
    exit 0
}

Write-Host "A/B: $Label   ($Runs runs x $Messages messages, $Mode mode, $BuildDir)"
Write-Host '---------------------------------------------------------------'

$samples = @()
for ($i = 0; $i -lt $Runs; $i++) {
    $out = & $exe $Messages $Mode 2>&1
    $line = $out | Select-String -Pattern 'throughput\s+(\d+)' | Select-Object -First 1
    if ($null -eq $line) {
        Write-Host "run $i produced no throughput line" -ForegroundColor Red
        Pop-Location
        exit 1
    }
    $v = [double]$line.Matches[0].Groups[1].Value
    $samples += $v
    Write-Host ('  run {0,2}  {1,10:N0} msg/s' -f ($i + 1), $v)
    $errors = ($out | Select-String -Pattern 'WARNING|rejected\s+[1-9]|truncat').Count
    if ($errors -gt 0) {
        Write-Host "  WARNING: run $i reported a warning; the numbers above are not clean" -ForegroundColor Yellow
    }
}

$st = Get-Stats $samples
Write-Host ''
Write-Host ('  median  {0,10:N0} msg/s' -f $st.median)
Write-Host ('  range   {0,10:N0} .. {1:N0}  ({2:N2}% raw, {3:N2}% trimmed)' -f $st.min, $st.max,
    (100.0 * ($st.max - $st.min) / $st.min), $st.trimmedPct)
Write-Host ('  noise   {0,10:N2}%  (max of 2xMAD and a 3% floor; a delta smaller than this is not a result)' -f $st.noisePct)

# ---- history ----------------------------------------------------------
#
# Named `$history`, never `$store`: PowerShell variable names are
# case-insensitive, so a `$store` here would overwrite the `[string]$Store`
# path parameter and every later use of it would be an array. This is not
# hypothetical -- it is the second time this script hit that trap.
#
# Always a PSCustomObject, never a Hashtable. `ConvertFrom-Json` returns
# one, but an empty `@{}` does not, and `.PSObject.Properties` over a
# Hashtable yields its CLR properties (Count, Keys, ...) rather than its
# entries -- so a Hashtable silently reports a "baseline" of zero and every
# verdict after it is meaningless.
$history = [pscustomobject]@{}
if (Test-Path -LiteralPath $Store) {
    $loaded = Get-Content -LiteralPath $Store -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -ne $loaded) { $history = $loaded }
}

# Compare against the most recently *stored* label, which is the intended
# workflow: record a baseline, then measure the next change against it.
$baselineName = $null
$baseline = $null
foreach ($prop in $history.PSObject.Properties) {
    $baselineName = $prop.Name
    $baseline = $prop.Value
}

$verdict = 'BASELINE RECORDED'
if ($null -ne $baseline -and $baselineName -ne $Label) {
    $baseMedian = [double]$baseline.median
    if ($baseMedian -le 0) {
        Write-Host '  stored baseline is unusable (median <= 0); ignoring it' -ForegroundColor Yellow
    } else {
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
}

if ($Save) {
    $history | Add-Member -NotePropertyName $Label -NotePropertyValue ([pscustomobject]@{
            median   = $st.median
            noisePct = $st.noisePct
            min      = $st.min
            max      = $st.max
            runs     = $Runs
            messages = $Messages
            buildDir = $BuildDir
            note     = "$(Get-Date -Format 'yyyy-MM-dd') $verdict"
        }) -Force
    $ordered = [ordered]@{}
    foreach ($prop in $history.PSObject.Properties) { $ordered[$prop.Name] = $prop.Value }
    [System.IO.File]::WriteAllText($Store, ($ordered | ConvertTo-Json -Depth 5),
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "  saved to $([System.IO.Path]::GetFileName($Store))"
}

Pop-Location
exit 0