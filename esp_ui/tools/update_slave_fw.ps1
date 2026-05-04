# Build slave fw, bundle it into the OTA project, flash to P4, wait for OTA.
# After this script, the C6 has the latest slave fw and the P4 needs to be
# reflashed with monmix (see flash_monmix.ps1).
#
# Sibling-project paths are resolved relative to this script (esp_ui/tools/);
# expects esp-hosted-mcu and host_performs_slave_ota as siblings of esp_ui.
#
# Pins IDF_PATH to S:\esp\v6.0.1 for both projects to avoid the drive-cache
# trap (see ../CLAUDE.md "Drive-cache trap"). Override MONMIX_IDF_PATH if
# your IDF lives elsewhere.

# 1) Source the EIM profile for tool paths, then pin IDF_PATH.
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' *> $null
if ($env:MONMIX_IDF_PATH) {
    $env:IDF_PATH = $env:MONMIX_IDF_PATH
} else {
    $env:IDF_PATH = 'S:\esp\v6.0.1\esp-idf'
}
$env:PYTHONIOENCODING = 'utf-8'

$monmixRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$slaveDir   = Join-Path $monmixRoot 'esp-hosted-mcu\slave'
$otaDir     = Join-Path $monmixRoot 'host_performs_slave_ota'
$slaveBin   = Join-Path $slaveDir   'build\network_adapter.bin'
$otaBinDest = Join-Path $otaDir     'components\ota_littlefs\slave_fw_bin\network_adapter.bin'
$idfPy      = Join-Path $env:IDF_PATH 'tools\idf.py'

# 2) Build the slave firmware.
Write-Host "==> Building slave firmware" -ForegroundColor Cyan
Set-Location $slaveDir
python $idfPy build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 3) Stage the slave bin into the OTA project. The OTA project's CMakeLists
#    has been patched in this fork to keep temp_littlefs alive across the
#    storage.bin generation step (POST_BUILD cleanup race fixed).
Write-Host "==> Staging slave bin into OTA project" -ForegroundColor Cyan
Copy-Item $slaveBin $otaBinDest -Force

# 4) Build the OTA project (which packs storage.bin from staged slave_fw_bin).
Write-Host "==> Building OTA project" -ForegroundColor Cyan
Set-Location $otaDir
python $idfPy build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 5) Flash OTA project to P4. P4 boots, OTA pushes new C6 fw via SDIO.
Write-Host "==> Flashing OTA project to P4" -ForegroundColor Cyan
python $idfPy -p COM3 flash
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Done. Use serial_tail.ps1 to watch OTA progress;" -ForegroundColor Cyan
Write-Host "    look for 'New firmware activated - slave will reboot' before" -ForegroundColor Cyan
Write-Host "    running flash_monmix.ps1 to put your real firmware back." -ForegroundColor Cyan
