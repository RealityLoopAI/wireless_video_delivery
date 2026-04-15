[CmdletBinding()]
param(
    [string]$Config = "config/receiver.json",
    [switch]$ForceRestart,
    [switch]$NoAutoInstall
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSCommandPath
$PidFile = Join-Path $ProjectRoot "receiver.pid"
$RuntimeFile = Join-Path $ProjectRoot "receiver.runtime.json"
$LogDir = Join-Path $ProjectRoot "Log"
$StdoutLog = Join-Path $LogDir "receiver_stdout.log"
$StderrLog = Join-Path $LogDir "receiver_stderr.log"

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

function Write-PidFile([int[]]$Pids) {
    $normalized = @($Pids | Select-Object -Unique | Sort-Object)
    if ($normalized.Count -eq 0) {
        if (Test-Path -LiteralPath $PidFile) {
            Remove-Item -LiteralPath $PidFile -Force
        }
        return
    }
    ($normalized -join ",") | Set-Content -LiteralPath $PidFile -Encoding ascii
}

function Resolve-ConfigPath([string]$ConfigValue) {
    if ([System.IO.Path]::IsPathRooted($ConfigValue)) {
        return [System.IO.Path]::GetFullPath($ConfigValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $ConfigValue))
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

function Install-RequirementsWithUv([string]$UvExe, [string]$PythonExe) {
    $reqFile = Join-Path $ProjectRoot "requirements.txt"
    if (-not (Test-Path -LiteralPath $reqFile)) {
        return
    }

    & $UvExe pip install --python $PythonExe -r $reqFile
    if ($LASTEXITCODE -ne 0) {
        throw "uv pip install failed."
    }
}

function Resolve-PythonExe {
    $venvPython = Join-Path $ProjectRoot ".venv\Scripts\python.exe"
    $uvCmd = Get-Command uv -ErrorAction SilentlyContinue
    $canUseUv = (-not $NoAutoInstall) -and $uvCmd -and $uvCmd.Path

    if ($canUseUv) {
        $env:UV_CACHE_DIR = Join-Path $ProjectRoot ".uv-cache"
        $env:UV_PYTHON_INSTALL_DIR = Join-Path $ProjectRoot ".uv-python"
    }

    if (Test-Path -LiteralPath $venvPython) {
        if ($canUseUv) {
            Push-Location $ProjectRoot
            try {
                Install-RequirementsWithUv -UvExe $uvCmd.Path -PythonExe $venvPython
            } finally {
                Pop-Location
            }
        }
        return $venvPython
    }

    if ($canUseUv) {
        Push-Location $ProjectRoot
        try {
            & $uvCmd.Path venv .venv --python 3.11
            if ($LASTEXITCODE -ne 0) {
                throw "uv venv failed."
            }

            Install-RequirementsWithUv -UvExe $uvCmd.Path -PythonExe $venvPython
        } finally {
            Pop-Location
        }

        if (-not (Test-Path -LiteralPath $venvPython)) {
            throw "Virtual environment created but python executable is missing: $venvPython"
        }

        return $venvPython
    }

    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCmd -and $pythonCmd.Path) {
        try {
            & $pythonCmd.Path --version *> $null
            if ($LASTEXITCODE -eq 0) {
                return $pythonCmd.Path
            }
        } catch {
        }
    }

    if ($NoAutoInstall) {
        throw "No usable Python found and -NoAutoInstall is set."
    }

    throw "No usable Python found. Install Python or uv."
}

function Stop-Pids([int[]]$Pids) {
    foreach ($procId in $Pids) {
        try {
            Stop-Process -Id $procId -Force -ErrorAction Stop
            Write-Output "Stopped existing receiver PID $procId"
        } catch {
            Write-Warning "Failed to stop PID ${procId}: $($_.Exception.Message)"
        }
    }
}

$configPath = Resolve-ConfigPath -ConfigValue $Config
if (-not (Test-Path -LiteralPath $configPath)) {
    throw "Receiver config not found: $configPath"
}

$existing = Find-ReceiverPids
if ($existing.Count -gt 0 -and -not $ForceRestart) {
    Write-PidFile -Pids $existing
    Write-Output "Receiver is already running. PID(s): $($existing -join ', ')"
    Write-Output "Use: .\start_receiver.ps1 -ForceRestart"
    exit 0
}

if ($existing.Count -gt 0 -and $ForceRestart) {
    Stop-Pids -Pids $existing
    Start-Sleep -Seconds 1
}

$pythonExe = Resolve-PythonExe
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

$pythonNames = @("python", "pythonw")
$baselinePythonPids = @(
    Get-Process -Name $pythonNames -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Id
)

$startArgs = @("run_receiver.py", "--config", $configPath)
$process = Start-Process `
    -FilePath $pythonExe `
    -ArgumentList $startArgs `
    -WorkingDirectory $ProjectRoot `
    -RedirectStandardOutput $StdoutLog `
    -RedirectStandardError $StderrLog `
    -PassThru

Start-Sleep -Seconds 2
$process.Refresh()
$activePids = Find-ReceiverPids
if ($activePids.Count -eq 0) {
    $currentPythonPids = @(
        Get-Process -Name $pythonNames -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id
    )
    $activePids = @(
        $currentPythonPids |
            Where-Object { $_ -notin $baselinePythonPids } |
            Select-Object -Unique |
            Sort-Object
    )
}

if ($activePids.Count -eq 0 -and -not $process.HasExited) {
    $activePids = @($process.Id)
}

if ($activePids.Count -eq 0) {
    throw "Receiver exited too early. Check log: $StderrLog"
}

Write-PidFile -Pids $activePids

$runtime = [ordered]@{
    pids        = $activePids
    started_at  = (Get-Date).ToString("o")
    config      = $configPath
    python      = $pythonExe
    stdout_log  = $StdoutLog
    stderr_log  = $StderrLog
}
$runtime | ConvertTo-Json | Set-Content -LiteralPath $RuntimeFile -Encoding ascii

Write-Output "Receiver started."
Write-Output "PID(s): $($activePids -join ', ')"
Write-Output "Config: $configPath"
Write-Output "Log: $StderrLog"
