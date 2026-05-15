# Build the stage_ui PC simulator. Bootstraps MSVC v143 from VS 2026
# Enterprise, uses the cmake/ninja that ship with the IDF tools install.
# Mirrors esp_ui/pc_sim/build.ps1 but resolves cmake/ninja dynamically so
# minor IDF version bumps don't break the script.
$ErrorActionPreference = 'Stop'

function Resolve-IdfTool {
    param([string]$Name, [string]$BinSubpath)
    $root = Join-Path 'C:\Espressif\tools' $Name
    if (-not (Test-Path $root)) { throw "$Name dir not found at $root" }
    # Pick the highest-version subdir present. IDF installs land at e.g.
    # C:\Espressif\tools\cmake\3.30.2\ or 4.0.3\; either works.
    $vers = Get-ChildItem $root -Directory | Sort-Object Name -Descending
    foreach ($v in $vers) {
        $candidate = Join-Path $v.FullName $BinSubpath
        if (Test-Path $candidate) { return $candidate }
    }
    throw "$Name binary not found under $root (looked for $BinSubpath)"
}

$VS_VCVARS = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$CMAKE     = Resolve-IdfTool -Name 'cmake' -BinSubpath 'bin\cmake.exe'
$NINJA     = Resolve-IdfTool -Name 'ninja' -BinSubpath 'ninja.exe'

if (-not (Test-Path $VS_VCVARS)) { throw "vcvars64.bat not at $VS_VCVARS" }

$envDump = & cmd /c "`"$VS_VCVARS`" >nul && set" 2>&1
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

$buildDir = Join-Path $PSScriptRoot 'build-windows'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

& $CMAKE -S $PSScriptRoot -B $buildDir -G Ninja `
    "-DCMAKE_C_COMPILER=cl" `
    "-DCMAKE_MAKE_PROGRAM=$NINJA" `
    "-DCMAKE_BUILD_TYPE=Debug"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (rc=$LASTEXITCODE)" }

& $CMAKE --build $buildDir --config Debug
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (rc=$LASTEXITCODE)" }

$exe = Join-Path $buildDir 'stage_ui_sim.exe'
Write-Output "OK: $exe"

if ($args -contains '--run') {
    & $exe
}

