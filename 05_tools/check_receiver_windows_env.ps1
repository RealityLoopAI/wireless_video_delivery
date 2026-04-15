[CmdletBinding()]
param(
    [string]$Config = "..\06_configs\receiver.windows.default.json"
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $PSCommandPath
$DeliveryRoot = Split-Path -Parent $ToolsDir
$ReceiverDir = Join-Path $DeliveryRoot "02_receiver_windows"

if (-not (Test-Path -LiteralPath $ReceiverDir)) {
    throw "[receiver-windows] project not found: $ReceiverDir"
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

Write-Output "[receiver-windows] project: $ReceiverDir"
Write-Output "[receiver-windows] config:  $configPath"

$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if ($pythonCmd) {
    Write-Output "[receiver-windows] python:  $($pythonCmd.Path)"
} else {
    Write-Output "[receiver-windows] python:  <not found, start script can auto-install via uv>"
}

Write-Output "[receiver-windows] environment check passed"
