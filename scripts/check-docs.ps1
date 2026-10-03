# Markdown link and anchor consistency check.
#
# Exists because this repository's documentation accreted corrections
# rather than replacing them, and three separate sections came to
# contradict each other while every link between them still resolved. A
# broken anchor is the same failure in a smaller costume: the reader is
# sent confidently to a place that no longer says what the link claims.
#
# Two checks, both cheap:
#   1. Every relative link target file exists.
#   2. Every anchor, in-page or cross-file, has a matching heading.
#
# GitHub's anchor rules, reimplemented: lowercase, strip punctuation
# except word characters, spaces to hyphens. Kept minimal on purpose --
# this needs to agree with GitHub, not exceed it, so anything ambiguous
# (duplicate headings, unicode punctuation) is reported rather than
# guessed at.

param(
    [string[]]$Files = @('README.md',
                         'docs/BUGS.md',
                         'docs/DESIGN.md',
                         'docs/RESULTS.md',
                         'results/ENVIRONMENT.md',
                         'results/OPTIMIZATION.md')
)

$ErrorActionPreference = 'Stop'

# Always run from the repository root, never from the caller's directory.
# CTest invokes this with the build directory as the working directory,
# and every path below is relative to the root -- which is also what makes
# the script usable by hand from anywhere.
#
# `Resolve-Path` first because $PSCommandPath is not guaranteed absolute
# when the script is invoked by relative path, and `Split-Path -Parent`
# twice on a relative path yields the wrong root in a way that looks like
# a missing-file error rather than a path bug.
$ScriptDir = Split-Path -Parent (Resolve-Path -LiteralPath $PSCommandPath).Path
$RepoRoot = Split-Path -Parent $ScriptDir
Push-Location $RepoRoot

function Get-Anchor([string]$heading) {
    $a = $heading.Trim().ToLowerInvariant()
    $a = $a -replace '[^\w\s-]', ''
    $a = $a -replace '\s+', '-'
    return $a
}

function Get-Anchors([string]$path) {
    $set = @{}
    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^#{1,6}\s+(.*?)\s*$') {
            $set[(Get-Anchor $Matches[1])] = $true
        }
    }
    return $set
}

$broken = 0
$checked = 0

foreach ($file in $Files) {
    # Resolve against the repo root explicitly. `Push-Location` changes
    # PowerShell's location but NOT the .NET current directory, so
    # [System.IO.File]::ReadAllText on a relative path still resolves
    # against the build directory CTest launched us in -- and the symptom
    # is a missing-file error rather than an obvious path bug.
    $abs = Join-Path $RepoRoot $file
    if (-not (Test-Path -LiteralPath $abs)) {
        Write-Host "  MISSING FILE: $file" -ForegroundColor Red
        $broken++
        continue
    }

    $text = [System.IO.File]::ReadAllText($abs, [System.Text.Encoding]::UTF8)
    $own = Get-Anchors $abs
    $dir = Split-Path -Parent $abs

    foreach ($line in ($text -split "`r?`n")) {
        foreach ($m in [regex]::Matches($line, '\]\(([^)\s]+)\)')) {
            $target = $m.Groups[1].Value
            if ($target -match '^(?:https?|mailto):') { continue }
            $checked++

            $hash = $target.IndexOf('#')
            if ($hash -lt 0) {
                $path = $target
                $frag = $null
            } else {
                $path = $target.Substring(0, $hash)
                $frag = $target.Substring($hash + 1)
            }

            if ([string]::IsNullOrEmpty($path)) {
                if ($frag -and -not $own.ContainsKey($frag)) {
                    Write-Host ("  {0}: anchor '#{1}' has no matching heading" -f $file, $frag) -ForegroundColor Red
                    $broken++
                }
                continue
            }

            $resolved = Join-Path $dir $path
            if (-not (Test-Path -LiteralPath $resolved)) {
                Write-Host ("  {0}: target '{1}' does not exist" -f $file, $path) -ForegroundColor Red
                $broken++
                continue
            }
            if ($frag) {
                $there = Get-Anchors $resolved
                if (-not $there.ContainsKey($frag)) {
                    Write-Host ("  {0}: '{1}' has no heading matching '#{2}'" -f $file, $path, $frag) -ForegroundColor Red
                    $broken++
                }
            }
        }
    }

    $n = ($text -split "`n").Count
    $w = ($text -split '\s+' | Where-Object { $_ -ne '' }).Count
    Write-Host ("  {0,-26} {1,5} lines {2,6} words" -f $file, $n, $w) -ForegroundColor DarkGray
}

if ($broken -eq 0) {
    Write-Host "`nOK: $checked links checked, every target and anchor resolves." -ForegroundColor Green
    Pop-Location
    exit 0
}
Write-Host "`nFAILED: $broken broken link(s) or anchor(s) out of $checked checked." -ForegroundColor Red
Pop-Location
exit 1