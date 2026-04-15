[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $PSCommandPath
$DeliveryRoot = Split-Path -Parent $ToolsDir
$ReceiverDir = Join-Path $DeliveryRoot "02_receiver_windows"
$StopScript = Join-Path $ReceiverDir "stop_receiver.ps1"

if (-not (Test-Path -LiteralPath $StopScript)) {
    throw "[receiver-windows] stop script not found: $StopScript"
}

& $StopScript -Force:$Force
