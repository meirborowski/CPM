# Self-hosting check (docs/spec/architecture.md §7): stage 0 -> stage 1 -> stage 2.
#   stage 0: scripts/bootstrap.ps1 (one compiler call, no CPM)
#   stage 1: stage 0 runs `cpm build --release`
#   stage 2: stage 1 rebuilds CPM from a clean build directory; stage 2 must pass `cpm test`
# -Toolchain msvc does all of it with Visual Studio's cl.exe, from the bootstrap on (--toolchain for every build).
param([switch]$SkipBootstrap, [string]$Toolchain)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$ext = if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$cache = 'cpm_modules/.cache'
$stage0 = "$cache/bootstrap/cpm$ext"
$stage1 = "$cache/stage1/cpm$ext"
$release = "build/release/cpm$ext"

function Step([string]$message) { Write-Host "`n== $message" -ForegroundColor Cyan }
function Run([string]$exe) {
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "'$exe $args' failed with exit code $LASTEXITCODE" }
}

$tc = @(if ($Toolchain) { '--toolchain', $Toolchain })

if (-not $SkipBootstrap) {
    Step 'stage 0: bootstrap'
    $bootstrapArgs = @(if ($Toolchain -eq 'msvc') { '-Msvc' })
    & pwsh -NoProfile -File scripts/bootstrap.ps1 @bootstrapArgs
    if ($LASTEXITCODE -ne 0) { throw 'bootstrap failed' }
}

Step 'stage 1: stage 0 builds CPM (release, clean)'
Run $stage0 build --release --clean @tc
New-Item -ItemType Directory -Force (Split-Path $stage1) | Out-Null
Copy-Item $release $stage1 -Force

Step 'stage 2: stage 1 rebuilds CPM from clean'
Run $stage1 build --release --clean @tc

Step 'stage 2: --version'
Run $release --version

Step 'stage 2: cpm test'
Run $release test @tc

Step 'stage 2: cpm init -y && cpm run in an empty directory'
$smoke = "$cache/smoke"
if (Test-Path $smoke) { Remove-Item -Recurse -Force $smoke }
Run $release -C (New-Item -ItemType Directory -Force $smoke).FullName init -y hello
$greeting = & $release -C "$smoke/hello" run @tc 2>$null
if ($LASTEXITCODE -ne 0 -or $greeting -ne 'Hello, world!') { throw "cpm run printed '$greeting' (exit $LASTEXITCODE)" }
Write-Host "cpm run printed: $greeting"

Step 'stage 2: no-op build'
$sw = [Diagnostics.Stopwatch]::StartNew()
Run $release build --release @tc
Write-Host ("no-op build: {0} ms (wall clock, including process start)" -f [int]$sw.Elapsed.TotalMilliseconds)

Step 'stage 2 rebuilds its own running executable'
(Get-Item src/version.hpp).LastWriteTime = Get-Date
Run $release build --release @tc
Run $release --version

Write-Host "`nself-hosting OK" -ForegroundColor Green
