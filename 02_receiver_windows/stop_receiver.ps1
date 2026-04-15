[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSCommandPath
$PidFile = Join-Path $ProjectRoot "receiver.pid"
$RuntimeFile = Join-Path $ProjectRoot "receiver.runtime.json"

function Read-PidFile {
    if (-not (Test-Path -LiteralPath $PidFile)) {
        return @()
    }

    $raw = (Get-Content -LiteralPath $PidFile -Raw).Trim()
    if (-not $raw) {
        return @()
    }

    return @(
        $raw -split "[,\s;]+" |
            Where-Object { $_ -match "^\d+$" } |
            ForEach-Object { [int]$_ } |
            Select-Object -Unique
    )
}

function Find-ReceiverPids {
    $ids = @()
    try {
        $candidates = Get-CimInstance Win32_Process -Filter "name='python.exe'" |
            Where-Object {
                $_.CommandLine -and
                $_.CommandLine -like "*run_receiver.py*" -and
                $_.CommandLine -like "*$ProjectRoot*"
            }
        $ids += $candidates.ProcessId
    } catch {
    }

    if ($ids.Count -eq 0) {
        foreach ($procId in Read-PidFile) {
            $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
            if ($proc -and $proc.ProcessName -like "python*") {
                $ids += $procId
            }
        }
    }

    return @($ids | Select-Object -Unique | Sort-Object)
}

function Cleanup-StateFiles {
    if (Test-Path -LiteralPath $PidFile) {
        Remove-Item -LiteralPath $PidFile -Force
    }
    if (Test-Path -LiteralPath $RuntimeFile) {
        Remove-Item -LiteralPath $RuntimeFile -Force
    }
}

$running = Find-ReceiverPids
if ($running.Count -eq 0) {
    Cleanup-StateFiles
    Write-Output "Receiver is not running."
    exit 0
}

foreach ($procId in $running) {
    try {
        if ($Force) {
            Stop-Process -Id $procId -Force -ErrorAction Stop
        } else {
            Stop-Process -Id $procId -ErrorAction Stop
        }
        Write-Output "Stopped receiver PID $procId"
    } catch {
        Write-Warning "Failed to stop PID ${procId}: $($_.Exception.Message)"
    }
}

Start-Sleep -Seconds 1
$remaining = Find-ReceiverPids
if ($remaining.Count -gt 0) {
    throw "Receiver still running. Remaining PID(s): $($remaining -join ', ')"
}

Cleanup-StateFiles
Write-Output "Receiver stopped."
