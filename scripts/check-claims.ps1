# Verify that the numbers this repository *claims* are the numbers its
# binaries actually *produce*.
#
# Why this exists. The README stated "783 checks" for as long as the
# binaries printed 1,412. Nothing was wrong with the code, nothing was
# wrong with the tests, and the doc-link check passed: a number typed by
# hand into prose has nothing to disagree with it. Every other guardrail in
# this repository exists because something was verified against something
# external, and this is the one place where the claim and its evidence were
# both inside the repository and neither was checking the other.
#
# It is the same shape as the check-docs script: cheap, mechanical, and run
# in CI. The interesting property is that adding a test now requires
# updating a number, and forgetting is a red build rather than a stale
# sentence.
#
# Three properties, all deliberate:
#   1. The counts are read from the binaries' own output, not from a
#      constant in this file.
#   2. Each binary must report zero failures as well as a count, so a
#      suite that fails loudly cannot also quietly deflate the total.
#   3. The visible prose is checked against the same numbers as the
#      machine-readable block, so "updated the block, forgot the README"
#      is caught as readily as the reverse.

param(
    [string]$BuildDir,
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

# Resolve the repository root from this script's own location rather than
# the working directory. CTest launches this with the *build* directory as
# cwd, so a relative path would resolve against the wrong tree and fail as
# a missing file instead of an obvious path bug. Same reasoning, and the
# same comment, as check-docs.ps1.
if (-not $RepoRoot) {
    $ScriptDir = Split-Path -Parent (Resolve-Path -LiteralPath $PSCommandPath).Path
    $RepoRoot = Split-Path -Parent $ScriptDir
}
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build' }
# Resolve if it exists, but do not fail here: a missing build directory is
# reported below as a missing binary, which is a clearer diagnostic than an
# exception from Resolve-Path. Written without ?. so it runs under
# PowerShell 5.1, which is what `powershell.exe` on Windows is.
$resolved = Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue
if ($resolved) { $BuildDir = $resolved.Path }

# The five suites that print a "N checks, M failures" summary line. The
# differential suites are excluded on purpose: they assert correctness via
# process exit status and take seconds each, and ctest already runs them.
$suites = [ordered]@{
    'unit'        = 'hft_test'
    'risk_oms'    = 'hft_risk_oms'
    'strategy'    = 'hft_strategy'
    'concurrent'  = 'hft_concurrent'
    'shards'      = 'hft_shards'
}

# Try the bare name and the .exe name rather than branching on the host.
# `$IsWindows` is PowerShell 6+; under 5.1 it is simply undefined, so an
# OS test written that way silently evaluates to false and appends no
# extension -- which looks like every binary being missing rather than like
# a version problem. Probing for both is shorter and cannot be wrong.
function Find-Binary([string]$name) {
    foreach ($candidate in @($name, ($name + '.exe'))) {
        $p = Join-Path $BuildDir $candidate
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

Write-Host "claim check"
Write-Host ("  build ......... {0}" -f $BuildDir)

# ---- 1. What the binaries say -----------------------------------------
$actual = @{}
$total = 0
$missing = @()
$failed = @()

foreach ($key in $suites.Keys) {
    $bin = Find-Binary $suites[$key]
    if (-not $bin) { $missing += $suites[$key]; continue }

    $out = & $bin 2>&1 | Out-String
    $m = [regex]::Match($out, '(\d+)\s+checks?,\s*(\d+)\s+failures?')
    if (-not $m.Success) {
        Write-Host ("  {0,-12} NO SUMMARY LINE -- the output format changed" -f $key) -ForegroundColor Red
        $failed += $key
        continue
    }
    $checks = [int]$m.Groups[1].Value
    $failures = [int]$m.Groups[2].Value
    $actual[$key] = $checks
    $total += $checks
    if ($failures -ne 0) { $failed += $key }

    Write-Host ("  {0,-12} {1,6} checks, {2} failures" -f $key, $checks, $failures)
}

$problems = @()

if ($missing.Count -gt 0) {
    $problems += ("missing binaries: " + ($missing -join ', ') +
                  " -- build the tree first (cmake --build $BuildDir)")
}

# ---- 2. How many CTest suites there are --------------------------------
$ctestCount = 0
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if ($ctest) {
    $list = & ctest --test-dir $BuildDir -N 2>&1 | Out-String
    $ctestCount = ([regex]::Matches($list, '(?m)^\s*Test\s+#\d+:')).Count
    Write-Host ("  ctest suites .. {0,6}" -f $ctestCount)
} else {
    Write-Host "  ctest ......... not found, suite count unchecked" -ForegroundColor Yellow
}

# ---- 3. What the documents claim --------------------------------------
$readmePath = Join-Path $RepoRoot 'README.md'
$envPath = Join-Path $RepoRoot 'results/ENVIRONMENT.md'

$blockTotal = $null
$blockCtest = $null
$blockUnit = @{}

foreach ($p in @($readmePath, $envPath)) {
    if (-not (Test-Path -LiteralPath $p)) { continue }
    $text = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
    $b = [regex]::Match($text, '(?s)<!--\s*claims(?<body>.*?)-->')
    if (-not $b.Success) { continue }

    foreach ($line in ($b.Groups['body'].Value -split "`r?`n")) {
        $kv = [regex]::Match($line.Trim(), '^(?<k>checks_[a-z_]+|checks_total|ctest_tests)\s*:\s*(?<v>\d+)\s*$')
        if (-not $kv.Success) { continue }
        $k = $kv.Groups['k'].Value
        $v = [int]$kv.Groups['v'].Value
        if ($k -eq 'checks_total') { $blockTotal = $v }
        elseif ($k -eq 'ctest_tests') { $blockCtest = $v }
        else { $blockUnit[$k.Substring('checks_'.Length)] = $v }
    }
}

if ($null -eq $blockTotal) {
    $problems += "no <!-- claims --> block found in README.md"
} else {
    Write-Host ("  claimed total . {0,6}" -f $blockTotal)

    if ($blockTotal -ne $total) {
        $problems += ("README claims $blockTotal checks; the binaries print $total")
    }
    foreach ($key in $blockUnit.Keys) {
        if ($actual.ContainsKey($key) -and $actual[$key] -ne $blockUnit[$key]) {
            $problems += ("README claims $key = {0}; {1} prints {2}" -f `
                          $blockUnit[$key], $suites[$key], $actual[$key])
        }
    }
    if ($null -ne $blockCtest -and $ctest -and $blockCtest -ne $ctestCount) {
        $problems += ("README claims $blockCtest CTest suites; ctest reports $ctestCount")
    }

    # The prose is what a reader sees. The block above is machine-readable
    # and invisible, so a number that is right in one place and stale in
    # the other is still a wrong README.
    $readme = [System.IO.File]::ReadAllText($readmePath, [System.Text.Encoding]::UTF8)
    $prose = [regex]::Replace($readme, '(?s)<!--.*?-->', '')
    $formatted = '{0:N0}' -f $blockTotal
    if ($prose -notmatch [regex]::Escape($formatted)) {
        $problems += ("README prose never states '$formatted' checks outside the claims block")
    }
}

if ($failed.Count -gt 0) {
    $problems += ("suites reporting failures: " + ($failed -join ', '))
}

if ($problems.Count -eq 0) {
    Write-Host "`nOK: every published count matches what the binaries print." -ForegroundColor Green
    exit 0
}

Write-Host "`nFAILED:" -ForegroundColor Red
foreach ($p in $problems) { Write-Host ("  - " + $p) -ForegroundColor Red }
exit 1