[CmdletBinding()]
param(
    [string]$Config = ".\06_configs\receiver.windows.default.json",
    [switch]$ForceRestart
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSCommandPath
$HelperScript = Join-Path $Root "05_tools\check_and_start_receiver_windows.ps1"

if (-not (Test-Path -LiteralPath $HelperScript)) {
    throw "[receiver-windows] helper script not found: $HelperScript"
}

$configPath = $Config
if (-not [System.IO.Path]::IsPathRooted($configPath)) {
    $configPath = [System.IO.Path]::GetFullPath((Join-Path $Root $configPath))
}

if (-not (Test-Path -LiteralPath $configPath)) {
    throw "[receiver-windows] config not found: $configPath"
}

& $HelperScript -Config $configPath -ForceRestart:$ForceRestart
