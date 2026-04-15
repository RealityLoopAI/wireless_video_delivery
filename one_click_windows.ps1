[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSCommandPath

function Invoke-RootScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptName
    )

    & (Join-Path $Root $ScriptName)
}

while ($true) {
    Write-Host ""
    Write-Host "===== 无线视频 Windows 一键菜单 ====="
    Write-Host "1) 启动 Windows 接收端"
    Write-Host "2) 查看接收端状态"
    Write-Host "3) 停止接收端"
    Write-Host "4) 退出"

    $choice = (Read-Host "请选择 [1-4]").Trim()
    switch ($choice) {
        "1" {
            Invoke-RootScript -ScriptName "start_receiver_windows.ps1"
        }
        "2" {
            Invoke-RootScript -ScriptName "status_receiver_windows.ps1"
        }
        "3" {
            Invoke-RootScript -ScriptName "stop_receiver_windows.ps1"
        }
        "4" {
            exit 0
        }
        default {
            Write-Warning "无效选项: $choice"
        }
    }
}

