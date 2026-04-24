[CmdletBinding()]
param(
    [string]$Port = "COM6",
    [string]$BoardKeyword = "CH340K",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Message)
    Write-Host "[INFO] $Message" -ForegroundColor Cyan
}

function Write-Ok {
    param([string]$Message)
    Write-Host "[ OK ] $Message" -ForegroundColor Green
}

function Write-ErrorLine {
    param([string]$Message)
    Write-Host "[ERR ] $Message" -ForegroundColor Red
}

function Get-PortDevices {
    $output = & pnputil /enum-devices /class Ports 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil failed to enumerate serial ports."
    }

    $devices = @()
    $current = @{}

    foreach ($line in $output) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($current.Count -gt 0) {
                $description = [string]$current["Device Description"]
                $devices += [pscustomobject]@{
                    InstanceId  = $current["Instance ID"]
                    Description = $description
                    Status      = $current["Status"]
                    Port        = if ($description -match '\((COM\d+)\)') { $matches[1] } else { $null }
                }
                $current = @{}
            }
            continue
        }

        if ($line -match '^\s*([^:]+):\s*(.*)$') {
            $current[$matches[1].Trim()] = $matches[2].Trim()
        }
    }

    if ($current.Count -gt 0) {
        $description = [string]$current["Device Description"]
        $devices += [pscustomobject]@{
            InstanceId  = $current["Instance ID"]
            Description = $description
            Status      = $current["Status"]
            Port        = if ($description -match '\((COM\d+)\)') { $matches[1] } else { $null }
        }
    }

    return $devices | Where-Object { $_.Port }
}

function Test-PortOpenable {
    param([string]$TargetPort)

    $serialPort = New-Object System.IO.Ports.SerialPort(
        $TargetPort,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )

    try {
        $serialPort.Open()
        return $true
    } catch {
        return $false
    } finally {
        if ($serialPort.IsOpen) {
            $serialPort.Close()
        }
        $serialPort.Dispose()
    }
}

function Get-CandidateProcesses {
    param([string]$TargetPort)

    $candidates = New-Object System.Collections.Generic.List[object]
    $safeNames = @(
        "putty", "plink", "ttermpro", "coolterm", "securecrt", "arduino", "esptool"
    )

    foreach ($proc in (Get-Process -ErrorAction SilentlyContinue)) {
        $name = $proc.ProcessName.ToLowerInvariant()
        if ($safeNames -contains $name) {
            $candidates.Add([pscustomobject]@{
                Id = $proc.Id
                Name = $proc.ProcessName
                Reason = "Known serial tool"
            })
        }
    }

    try {
        $wmiProcesses = Get-CimInstance Win32_Process -ErrorAction Stop |
            Where-Object { $_.Name -in @("python.exe", "cmd.exe", "powershell.exe", "pwsh.exe") }

        foreach ($proc in $wmiProcesses) {
            $cmdLine = [string]$proc.CommandLine
            if ($cmdLine -match [regex]::Escape($TargetPort) -or
                $cmdLine -match 'idf\.py' -or
                $cmdLine -match 'monitor' -or
                $cmdLine -match 'miniterm') {
                $candidates.Add([pscustomobject]@{
                    Id = [int]$proc.ProcessId
                    Name = [string]$proc.Name
                    Reason = "Command line matches ESP-IDF/serial monitor usage"
                })
            }
        }
    } catch {
        Write-Info "Could not inspect command lines via CIM. Fallback matching will be limited."
    }

    return $candidates | Sort-Object Id -Unique
}

function Stop-CandidateProcesses {
    param([object[]]$Processes)

    foreach ($proc in $Processes) {
        if ($DryRun) {
            Write-Info "DryRun: would stop PID $($proc.Id) [$($proc.Name)] - $($proc.Reason)"
            continue
        }

        try {
            Stop-Process -Id $proc.Id -Force -ErrorAction Stop
            Write-Ok "Stopped PID $($proc.Id) [$($proc.Name)] - $($proc.Reason)"
        } catch {
            Write-ErrorLine "Failed to stop PID $($proc.Id) [$($proc.Name)]: $($_.Exception.Message)"
        }
    }
}

Write-Info "Checking serial devices..."
$devices = Get-PortDevices
$startedDevices = $devices | Where-Object { $_.Port }
$targetDevice = $startedDevices | Where-Object { $_.Port -eq $Port } | Select-Object -First 1

if (-not $startedDevices) {
    Write-ErrorLine "No serial devices with COM ports were found."
    exit 1
}

if (-not $targetDevice) {
    Write-ErrorLine "Expected board on $Port, but no serial device is using that port."
    Write-Host ""
    Write-Host "Detected serial devices:"
    $startedDevices | Format-Table Port, Description, Status -AutoSize
    exit 1
}

if (-not [string]::IsNullOrWhiteSpace($BoardKeyword) -and
    ($targetDevice.Description -notmatch [regex]::Escape($BoardKeyword))) {
    Write-ErrorLine "Device on $Port does not match board keyword '$BoardKeyword'."
    Write-Host ""
    Write-Host "Found on ${Port}: $($targetDevice.Description)"
    exit 1
}

Write-Ok "Found board on ${Port}: $($targetDevice.Description)"

if (Test-PortOpenable -TargetPort $Port) {
    Write-Ok "$Port is already free."
    exit 0
}

Write-Info "$Port is busy. Looking for common serial monitor processes..."
$candidates = Get-CandidateProcesses -TargetPort $Port

if (-not $candidates -or $candidates.Count -eq 0) {
    Write-ErrorLine "No known holder process was found automatically."
    Write-Host ""
    Write-Host "Close VS Code serial monitor, idf.py monitor, or other serial tools, then run this again."
    Write-Host "For precise PID detection, install Sysinternals handle.exe and extend this script if needed."
    exit 2
}

Stop-CandidateProcesses -Processes $candidates

Start-Sleep -Milliseconds 800

if (Test-PortOpenable -TargetPort $Port) {
    Write-Ok "$Port is free now."
    exit 0
}

Write-ErrorLine "$Port is still busy after stopping common candidates."
Write-Host ""
Write-Host "Likely causes:"
Write-Host "  1. VS Code monitor or another serial tool is still open"
Write-Host "  2. The board re-enumerated to another port"
Write-Host "  3. A non-standard process still holds the port"
Write-Host ""
Write-Host "Detected serial devices:"
$startedDevices | Format-Table Port, Description, Status -AutoSize
exit 3
