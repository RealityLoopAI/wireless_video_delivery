[CmdletBinding()]
param(
    [string]$Config = "..\06_configs\receiver.windows.default.json",
    [switch]$ForceRestart
)

$ErrorActionPreference = "Stop"

$ToolsDir = Split-Path -Parent $PSCommandPath
$checkScript = Join-Path $ToolsDir "check_receiver_windows_env.ps1"
$startScript = Join-Path $ToolsDir "start_receiver_windows_easy.ps1"

& $checkScript -Config $Config
& $startScript -Config $Config -ForceRestart:$ForceRestart
