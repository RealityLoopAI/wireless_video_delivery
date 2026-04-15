[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSCommandPath

& (Join-Path $Root "05_tools\stop_receiver_windows.ps1") -Force:$Force
