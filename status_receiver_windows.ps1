[CmdletBinding()]
param(
    [int]$Tail = 10
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSCommandPath

& (Join-Path $Root "05_tools\status_receiver_windows.ps1") -Tail $Tail
