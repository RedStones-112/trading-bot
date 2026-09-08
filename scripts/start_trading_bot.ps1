# Starts trading_bot.exe automatically when regular market hours are open (weekdays 09:00-15:30).
# Registered in Windows Task Scheduler to run at logon + daily at 09:00
# (see scripts/register_scheduled_tasks.ps1). No-op if already running (avoids duplicate launch).
#
# Log messages are kept in English on purpose: Windows PowerShell 5.1 parses a BOM-less UTF-8
# .ps1 file using the system ANSI codepage, so literal Korean text embedded in this script
# gets mangled before it's even written to the log.

$ErrorActionPreference = 'Stop'
$root = "C:\Users\wogur\OneDrive\Desktop\project\trading"
$exe = Join-Path $root "build\trading_bot.exe"
$logFile = Join-Path $root "scripts\autostart.log"

function Write-Log($msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    Add-Content -Path $logFile -Value $line -Encoding utf8
}

try {
    $now = Get-Date
    $isWeekend = $now.DayOfWeek -eq [DayOfWeek]::Saturday -or $now.DayOfWeek -eq [DayOfWeek]::Sunday
    if ($isWeekend) {
        Write-Log "Weekend ($($now.DayOfWeek)) -- skipping auto-start"
        exit 0
    }

    $marketOpen = Get-Date -Hour 9 -Minute 0 -Second 0
    $marketClose = Get-Date -Hour 15 -Minute 30 -Second 0
    if ($now -lt $marketOpen -or $now -gt $marketClose) {
        Write-Log "Outside regular market hours ($($now.ToString('HH:mm'))) -- skipping auto-start"
        exit 0
    }

    $running = Get-Process -Name "trading_bot" -ErrorAction SilentlyContinue
    if ($running) {
        Write-Log "Already running (PID $($running.Id)) -- skipping"
        exit 0
    }

    if (-not (Test-Path $exe)) {
        Write-Log "Executable not found: $exe"
        exit 1
    }

    Start-Process -FilePath $exe -WorkingDirectory $root -WindowStyle Hidden
    Write-Log "Started trading_bot.exe (market hours, $($now.ToString('HH:mm')))"
}
catch {
    Write-Log "ERROR: $($_.Exception.Message)"
    exit 1
}
