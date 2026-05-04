# Tail COM3 to a log file. Designed to run in the background while another
# task drives traffic to the device.
# Usage:  .\tools\serial_tail.ps1 [baud] [outfile]
#   baud:    921600 (default; matches our REPL) or 115200 for default IDF
#   outfile: defaults to tmp\logs\serial-tail.log

param(
    [int]$Baud   = 921600,
    [string]$Out = (Join-Path $PSScriptRoot '..\tmp\logs\serial-tail.log')
)

$pyExe = 'C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$tail  = Join-Path $PSScriptRoot 'serial_tail.py'
& $pyExe -u $tail COM3 $Baud > $Out 2>&1
