# Stage 0: build CPM without CPM (docs/spec/architecture.md §7).
# Compiles every src/**/*.cpp in one compiler call. After this, use the result to build CPM with itself:
#   cpm_modules/.cache/bootstrap/cpm.exe build --release
# -Msvc builds with Visual Studio's cl.exe instead, in the environment vcvarsall.bat sets (docs/spec/build.md §1.5).
param(
    [string]$Cxx = $(if ($env:CXX) { $env:CXX } else { 'g++' }),
    [switch]$Msvc
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$out = 'cpm_modules/.cache/bootstrap'
New-Item -ItemType Directory -Force $out | Out-Null

$sources = Get-ChildItem -Path src -Recurse -File -Filter *.cpp |
    ForEach-Object { (Resolve-Path -Relative $_.FullName) -replace '^\.[\\/]', '' -replace '\\', '/' } |
    Sort-Object

$exe = "$out/cpm.exe"
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Msvc) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'bootstrap: no Visual Studio with the C++ tools found' }
    # vcvarsall.bat's environment, taken into this process.
    $vars = cmd /d /c "call `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>nul && set"
    if ($LASTEXITCODE -ne 0) { throw "bootstrap: vcvarsall.bat failed (exit $LASTEXITCODE)" }
    foreach ($line in $vars) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1)) }
    }
    # No two sources share a file name, so their objects can share a directory.
    $obj = "$out/obj"
    New-Item -ItemType Directory -Force $obj | Out-Null
    $flags = @('/nologo', '/std:c++latest', '/O1', '/EHsc', '/utf-8', '/Zc:__cplusplus', '/Zc:preprocessor', '/permissive-',
               '/bigobj', '/MT', '/MP', '/Isrc', "/Fo$obj/")
    Write-Host "bootstrap: cl $($flags -join ' ') ($($sources.Count) sources) -> $exe"
    & cl @flags @sources "/Fe$exe" /link shell32.lib
} else {
    $flags = @('-std=c++23', '-O1', '-Isrc', '-static')
    Write-Host "bootstrap: $Cxx $($flags -join ' ') ($($sources.Count) sources) -> $exe"
    & $Cxx @flags @sources -o $exe -lshell32
}
if ($LASTEXITCODE -ne 0) { throw "bootstrap compile failed (exit $LASTEXITCODE)" }
Write-Host ("bootstrap: done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
& $exe --version
