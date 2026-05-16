# Build + run the PC simulator. Bootstraps MSVC v143 from VS 2026 Enterprise,
# uses the cmake/ninja that ship with the IDF tools install (so we don't
# need a separate cmake on PATH).
$ErrorActionPreference = 'Stop'

$VS_VCVARS = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$CMAKE     = 'C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe'
$NINJA     = 'C:\Espressif\tools\ninja\1.12.1\ninja.exe'

if (-not (Test-Path $VS_VCVARS)) { throw "vcvars64.bat not at $VS_VCVARS" }
if (-not (Test-Path $CMAKE))     { throw "cmake.exe not at $CMAKE" }
if (-not (Test-Path $NINJA))     { throw "ninja.exe not at $NINJA" }

# vcvars64.bat sets dozens of env vars that MSVC needs (PATH for cl.exe,
# INCLUDE, LIB, LIBPATH). Run it in a child cmd.exe and reflect the
# resulting environment back into this PowerShell session.
$envDump = & cmd /c "`"$VS_VCVARS`" >nul && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

# Per-host build dir so the WSL build (build-linux/) doesn't clobber this
# one. CMake records the absolute source path in the cache and refuses to
# reuse a cache configured with a different path — S:/… vs /mnt/s/… —
# so the two builds need separate trees.
$buildDir = Join-Path $PSScriptRoot 'build-windows'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

& $CMAKE -S $PSScriptRoot -B $buildDir -G Ninja -Wno-dev `
    "-DCMAKE_C_COMPILER=cl" `
    "-DCMAKE_MAKE_PROGRAM=$NINJA" `
    "-DCMAKE_BUILD_TYPE=Debug"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (rc=$LASTEXITCODE)" }

& $CMAKE --build $buildDir --config Debug
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (rc=$LASTEXITCODE)" }

$exe = Join-Path $buildDir 'monmix_sim.exe'
Write-Output "OK: $exe"

if ($args -contains '--run') {
    & $exe
}
