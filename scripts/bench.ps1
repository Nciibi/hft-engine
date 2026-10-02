<#
.SYNOPSIS
Reproduce the committed benchmark numbers on Windows.

.DESCRIPTION
The Windows counterpart to scripts/bench.sh. It exists because the
development machine is Windows: bench.sh cannot run here, so the one
command that reproduces the numbers was not actually available on the
platform the work happens on. A reproduce script you cannot run is a
claim rather than a tool.

The ORDER is the point, and it matches bench.sh:

  1. environment, before any number, so a reader never sees a latency
     figure without the machine it came from;
  2. build, from a clean configure;
  3. correctness, before results -- the differential test and the
     concurrency equivalence gates, which must pass or the numbers below
     them mean nothing;
  4. the benchmarks, in the order a reader wants them.

A correctness failure exits non-zero immediately. A benchmark that
reports message loss or a book mismatch exits zero but says so loudly in
its output, because "faster but wrong" is the failure mode this
repository cares most about.

.PARAMETER Messages
Message count for the benchmarks. Default 5,000,000, which is what the
committed runs use.

.PARAMETER Toolchain
Directory to prepend to PATH before configuring. Defaults to
D:\w64devkit\bin if that exists, then to whatever is already on PATH.

.EXAMPLE
./scripts/bench.ps1
./scripts/bench.ps1 -Messages 2000000
#>
[CmdletBinding()]
param(
    [long] $Messages = 5000000,
    [string] $Toolchain = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# ---- Toolchain -------------------------------------------------------
# Prepended rather than assumed, so the compiler recorded in the output
# is the compiler that actually built the binaries.
if (-not $Toolchain) {
    $candidate = 'D:\w64devkit\bin'
    if (Test-Path $candidate) { $Toolchain = $candidate }
}
if ($Toolchain -and (Test-Path $Toolchain)) {
    $env:PATH = "$Toolchain;$env:PATH"
}

function Fail([string] $Message) {
    Write-Host ''
    Write-Host "FAILED: $Message" -ForegroundColor Red
    exit 1
}

function Run([string] $Exe, [string[]] $Arguments, [string] $What) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "$What exited with $LASTEXITCODE"
    }
}

# ---- Environment, printed first --------------------------------------
Write-Host '=== environment ==='
Write-Host ("date        : {0}" -f (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))

$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1

Write-Host ("os          : {0} {1} ({2})" -f $os.Caption, $os.Version, $os.OSArchitecture)
Write-Host ("cpu         : {0}" -f $cpu.Name.Trim())
Write-Host ("cores       : {0} physical / {1} logical, {2:N1} GB RAM" -f `
    $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors, ($cs.TotalPhysicalMemory / 1GB))

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
Write-Host ("cmake       : {0}" -f $(if ($cmake) { (& cmake --version | Select-Object -First 1) } else { '(not on PATH)' }))

# The compiler identity matters more than the cmake version, because it
# is what determines whether the numbers are comparable to anything.
$compilerVersion = '(unknown)'
if ($env:CXX) { $compilerVersion = $env:CXX }
elseif (Get-Command g++ -ErrorAction SilentlyContinue) { $compilerVersion = 'g++' }
elseif (Get-Command clang++ -ErrorAction SilentlyContinue) { $compilerVersion = 'clang++' }
elseif (Get-Command cl -ErrorAction SilentlyContinue) { $compilerVersion = 'msvc' }
Write-Host ("compiler    : {0}" -f $compilerVersion)
if ($compilerVersion -eq 'g++') {
    Write-Host ("compiler ver: {0}" -f ((& g++ --version | Select-Object -First 1)))
}
if ($cmake) {
    Write-Host ("native arch : {0}" -f $(if ($env:HFT_NATIVE_ARCH) { $env:HFT_NATIVE_ARCH } else { 'off' }))
}

# SMT occupancy, because a busy sibling core inflates a tail and a
# reader cannot tell that from the number alone. The same reason the
# Linux script prints siblings-per-core.
Write-Host ("smt         : {0} logical per physical core" -f `
    $(if ($cpu.NumberOfCores) { [math]::Round($cpu.NumberOfLogicalProcessors / $cpu.NumberOfCores, 0) } else { '?' }))

# Not available without privileges on Windows, and saying so is better
# than printing nothing.
Write-Host ("governor    : {0}" -f $(try {
        (Get-CimInstance -ClassName Win32_Processor).PowerManagementSupported.ToString()
    } catch { 'unavailable on Windows' }))
Write-Host ("isolation   : none (no core isolation, no SMT pinning, no hugepages)")
Write-Host ("messages    : {0}" -f $Messages)
Write-Host ''

if (-not $cmake) { Fail 'cmake not found on PATH' }

# ---- Build -----------------------------------------------------------
Write-Host '=== build ==='
$BuildDir = Join-Path $Root 'build'
$configureArgs = @('-S', $Root, '-B', $BuildDir, '-DCMAKE_BUILD_TYPE=Release')
if ($env:CXX) { $configureArgs += "-DCMAKE_CXX_COMPILER=$env:CXX" }
Run 'cmake' $configureArgs 'cmake configure'
Run 'cmake' @('--build', $BuildDir, '--parallel') 'cmake build'
Write-Host ''

# ---- Correctness before numbers --------------------------------------
Write-Host '=== correctness ==='
Write-Host 'These must pass before any figure below is worth reading.' -ForegroundColor Yellow

$Diff = Join-Path $BuildDir 'hft_differential.exe'
if (-not (Test-Path $Diff)) { Fail "missing $Diff" }
& $Diff
if ($LASTEXITCODE -ne 0) { Fail 'differential test' }

# The concurrency gates are regex-matched on their own output, exactly
# as CTest runs them. hft_shards is the routing differential: a
# multi-symbol feed routed through the fast reference index must produce
# books identical to a std::map oracle.
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if ($ctest) {
    & ctest --test-dir $BuildDir --output-on-failure `
        -R 'concurrent|shards|pipeline_threaded_equivalence|ring_transfer_integrity|determinism'
    if ($LASTEXITCODE -ne 0) { Fail 'concurrency and sharding tests' }
} else {
    Write-Host 'WARNING: ctest not found; running the binaries directly.' -ForegroundColor Yellow
    foreach ($pair in @(
            @('hft_concurrent.exe',    @()),
            @('hft_shards.exe',        @()),
            @('hft_pipeline_bench.exe', @('50000')),
            @('hft_ring_bench.exe',    @('200000')))) {
        $exe = Join-Path $BuildDir $pair[0]
        if (Test-Path $exe) {
            & $exe @($pair[1])
            if ($LASTEXITCODE -ne 0) { Fail $pair[0] }
        }
    }
}
Write-Host ''

# ---- Benchmarks ------------------------------------------------------
Write-Host '=== latency and throughput: add-only ingest ==='
Run (Join-Path $BuildDir 'hft_bench.exe') @("$Messages") 'hft_bench'

Write-Host ''
Write-Host '=== per-stage latency: shallow vs deep book ==='
Run (Join-Path $BuildDir 'hft_stage_bench.exe') @("$Messages") 'hft_stage_bench'

Write-Host ''
Write-Host '=== concurrency: ring vs mutex baseline ==='
Run (Join-Path $BuildDir 'hft_ring_bench.exe') @("$Messages") 'hft_ring_bench'

Write-Host ''
Write-Host '=== concurrency: split pipeline vs symbol sharding ==='
Run (Join-Path $BuildDir 'hft_pipeline_bench.exe') @("$Messages") 'hft_pipeline_bench'

Write-Host ''
Write-Host 'Paste the environment block above into results/ENVIRONMENT.md' -ForegroundColor Yellow
Write-Host 'before filling any [MEASURED] placeholder in README.md. A number'
Write-Host 'without its host specification is an anecdote.'
exit 0
