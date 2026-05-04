# Build + flash the monmix project (from the checkout this script lives in)
# to COM3.
# Usage:  .\tools\flash_monmix.ps1
#
# Sources the EIM v6.0.1 IDF profile, switches into the project root, then
# runs idf.py -p COM3 flash. Output is plain idf.py output -- the env setup
# is silenced so logs only show actual build/flash actions.

. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' *> $null
$env:PYTHONIOENCODING = 'utf-8'
Set-Location (Join-Path $PSScriptRoot '..')
idf.py -p COM3 flash
