[CmdletBinding()]
param(
    [int]$Tail = 10
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSCommandPath
$PidFile = Join-Path $ProjectRoot "receiver.pid"
$RuntimeFile = Join-Path $ProjectRoot "receiver.runtime.json"
$StdoutLog = Join-Path $ProjectRoot "Log\receiver_stdout.log"
$StderrLog = Join-Path $ProjectRoot "Log\receiver_stderr.log"

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

function Find-ReceiverProcesses {
    $items = @()
    try {
        $items = Get-CimInstance Win32_Process -Filter "name='python.exe'" |
            Where-Object {
                $_.CommandLine -and
                $_.CommandLine -like "*run_receiver.py*" -and
                $_.CommandLine -like "*$ProjectRoot*"
            } |
            Select-Object @{
                Name = "Pid"; Expression = { $_.ProcessId }
            }, @{
                Name = "Created"; Expression = { $_.CreationDate }
            }, @{
                Name = "CommandLine"; Expression = { $_.CommandLine }
            }
    } catch {
    }

    if ($items.Count -eq 0) {
        foreach ($procId in Read-PidFile) {
            $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
            if ($proc -and $proc.ProcessName -like "python*") {
                $created = $null
                try {
                    $created = $proc.StartTime
                } catch {
                    $created = "<start time unavailable>"
                }
                $items += [PSCustomObject]@{
                    Pid         = $proc.Id
                    Created     = $created
                    CommandLine = "<command line unavailable>"
                }
            }
        }
    }

    return @($items | Sort-Object Pid -Unique)
}

function Show-LogInfo([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Output "${Name}: <missing>"
        return
    }
    $item = Get-Item -LiteralPath $Path
    Write-Output ("{0}: {1} bytes, updated {2}" -f $Name, $item.Length, $item.LastWriteTime)
}

$running = Find-ReceiverProcesses
if ($running.Count -gt 0) {
    Write-Output "Receiver status: RUNNING"
    foreach ($entry in $running) {
        Write-Output "PID=$($entry.Pid) Created=$($entry.Created)"
    }
} else {
    Write-Output "Receiver status: STOPPED"
}

if (Test-Path -LiteralPath $PidFile) {
    $pidRaw = (Get-Content -LiteralPath $PidFile -Raw).Trim()
    Write-Output "receiver.pid: $pidRaw"
} else {
    Write-Output "receiver.pid: <missing>"
}

if (Test-Path -LiteralPath $RuntimeFile) {
    Write-Output "receiver.runtime.json:"
    Get-Content -LiteralPath $RuntimeFile
} else {
    Write-Output "receiver.runtime.json: <missing>"
}

Show-LogInfo -Path $StderrLog -Name "stderr log"
Show-LogInfo -Path $StdoutLog -Name "stdout log"

if (Test-Path -LiteralPath $StderrLog) {
    $lastState = Get-Content -LiteralPath $StderrLog -Tail 200 | Select-String -Pattern "state=" | Select-Object -Last 1
    if ($lastState) {
        Write-Output "Last state line: $($lastState.Line)"
    } else {
        Write-Output "Last state line: <not found>"
    }

    if ($Tail -gt 0) {
        Write-Output "Recent stderr log lines:"
        Get-Content -LiteralPath $StderrLog -Tail $Tail
    }
}
