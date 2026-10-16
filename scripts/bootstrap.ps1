# Stage 0: build CPM without CPM (docs/spec/architecture.md §7).
# Compiles every src/**/*.cpp in one compiler call. After this, use the result to build CPM with itself:
#   cpm_modules/.cache/bootstrap/cpm.exe build --release
param(
    [string]$Cxx = $(if ($env:CXX) { $env:CXX } else { 'g++' })
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$out = 'cpm_modules/.cache/bootstrap'
New-Item -ItemType Directory -Force $out | Out-Null

$sources = Get-ChildItem -Path src -Recurse -File -Filter *.cpp |
    ForEach-Object { (Resolve-Path -Relative $_.FullName) -replace '^\.[\\/]', '' -replace '\\', '/' } |
    Sort-Object

$exe = "$out/cpm.exe"
$flags = @('-std=c++23', '-O1', '-Isrc', '-static')
Write-Host "bootstrap: $Cxx $($flags -join ' ') ($($sources.Count) sources) -> $exe"
$sw = [Diagnostics.Stopwatch]::StartNew()
& $Cxx @flags @sources -o $exe -lshell32
if ($LASTEXITCODE -ne 0) { throw "bootstrap compile failed (exit $LASTEXITCODE)" }
Write-Host ("bootstrap: done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
& $exe --version
