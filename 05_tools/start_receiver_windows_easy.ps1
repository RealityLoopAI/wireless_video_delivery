[CmdletBinding()]
param(
    [string]$Config = "..\06_configs\receiver.windows.default.json",
    [switch]$ForceRestart
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $PSCommandPath
$DeliveryRoot = Split-Path -Parent $ToolsDir
$ReceiverDir = Join-Path $DeliveryRoot "02_receiver_windows"
$StartScript = Join-Path $ReceiverDir "start_receiver.ps1"

if (-not (Test-Path -LiteralPath $StartScript)) {
    throw "[receiver-windows] start script not found: $StartScript"
}

function Resolve-ConfigPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }

    $candidateTools = [System.IO.Path]::GetFullPath((Join-Path $ToolsDir $Value))
    if (Test-Path -LiteralPath $candidateTools) {
        return $candidateTools
    }

    $candidateRoot = [System.IO.Path]::GetFullPath((Join-Path $DeliveryRoot $Value))
    return $candidateRoot
}

$configPath = Resolve-ConfigPath -Value $Config

if (-not (Test-Path -LiteralPath $configPath)) {
    throw "[receiver-windows] config not found: $configPath"
}

& $StartScript -Config $configPath -ForceRestart:$ForceRestart
