# Build + optionally run the esp_pp_display PC sim.
# Bootstraps MSVC from VS, sources the ESP-IDF v6.0.1 PowerShell profile
# to get cmake/ninja on PATH.
#
# NOTE: $ErrorActionPreference is deliberately NOT set to 'Stop' because
# cmake 4.x emits FetchContent warnings to stderr that PowerShell would
# otherwise treat as fatal. Exit codes are the source of truth.

$VS_VCVARS = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $VS_VCVARS)) {
    $alt = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $alt) { $VS_VCVARS = $alt }
    else { $alt = 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
           if (Test-Path $alt) { $VS_VCVARS = $alt } }
}
if (-not (Test-Path $VS_VCVARS)) { Write-Error "no vcvars64.bat found"; exit 1 }

$idfProfile = 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
if (Test-Path $idfProfile) { . $idfProfile *> $null }

$cmakeCmd = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$ninjaCmd = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $cmakeCmd) { Write-Error "cmake.exe not on PATH"; exit 1 }
if (-not $ninjaCmd) { Write-Error "ninja.exe not on PATH"; exit 1 }

$envDump = & cmd /c "`"$VS_VCVARS`" >nul && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

$buildDir = Join-Path $PSScriptRoot 'build-windows'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

# Run cmake via cmd.exe so PowerShell's stderr-as-error trap doesn't fire.
$configCmd = "`"$cmakeCmd`" -S `"$PSScriptRoot`" -B `"$buildDir`" -G Ninja " +
             "-DCMAKE_C_COMPILER=cl " +
             "-DCMAKE_MAKE_PROGRAM=`"$ninjaCmd`" " +
             "-DCMAKE_BUILD_TYPE=Debug"
& cmd /c $configCmd
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed (rc=$LASTEXITCODE)"; exit 1 }

$buildCmd = "`"$cmakeCmd`" --build `"$buildDir`" --config Debug"
& cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) { Write-Error "cmake build failed (rc=$LASTEXITCODE)"; exit 1 }

$exe = Join-Path $buildDir 'esp_pp_sim.exe'
Write-Output "OK: $exe"

if ($args -contains '--run') {
    & $exe
}
