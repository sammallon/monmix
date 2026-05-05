# Build + run the native unit tests (Windows).
$ErrorActionPreference = 'Stop'

$VS_VCVARS = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$CMAKE     = 'C:\Espressif\tools\cmake\3.30.2\bin\cmake.exe'
$NINJA     = 'C:\Espressif\tools\ninja\1.12.1\ninja.exe'

# Reflect MSVC env into this shell so cmake finds cl.
$envDump = cmd.exe /c "`"$VS_VCVARS`" >nul 2>nul && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

$buildDir = Join-Path $PSScriptRoot 'build-windows'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }

& $CMAKE -S $PSScriptRoot -B $buildDir -G Ninja `
    "-DCMAKE_C_COMPILER=cl" "-DCMAKE_MAKE_PROGRAM=$NINJA" `
    "-DCMAKE_BUILD_TYPE=Debug"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $CMAKE --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Push-Location $buildDir
& $CMAKE -E env ctest --output-on-failure
$rc = $LASTEXITCODE
Pop-Location
exit $rc
