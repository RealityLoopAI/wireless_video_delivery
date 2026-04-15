[CmdletBinding()]
param(
    [int]$Tail = 10
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $PSCommandPath
$DeliveryRoot = Split-Path -Parent $ToolsDir
$ReceiverDir = Join-Path $DeliveryRoot "02_receiver_windows"
$StatusScript = Join-Path $ReceiverDir "status_receiver.ps1"

if (-not (Test-Path -LiteralPath $StatusScript)) {
    throw "[receiver-windows] status script not found: $StatusScript"
}

& $StatusScript -Tail $Tail
