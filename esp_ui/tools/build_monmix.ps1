# Build the monmix project from the checkout this script lives in.
# Usage:  .\tools\build_monmix.ps1

. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' *> $null
$env:PYTHONIOENCODING = 'utf-8'
Set-Location (Join-Path $PSScriptRoot '..')
idf.py build
