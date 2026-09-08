# Stops trading_bot.exe automatically shortly after regular market hours close (15:30).
# Registered in Windows Task Scheduler to run daily at 15:35 (see scripts/register_scheduled_tasks.ps1).
# No-op if it's a weekend or the process isn't running.
#
# trading_bot.exe has no graceful-shutdown handler (no SIGINT/console-ctrl hook), so this is a
# hard kill -- consistent with how this process has always been stopped in this project (see
# PROGRESS.md 2026-07-23/2026-08-14/2026-09-02 sessions): a restart is safe because the startup
# routine auto-cancels any pending orders and restores holdings from the broker.
#
# Log messages are kept in English on purpose: Windows PowerShell 5.1 parses a BOM-less UTF-8
# .ps1 file using the system ANSI codepage, so literal Korean text embedded in this script
# gets mangled before it's even written to the log.

$ErrorActionPreference = 'Stop'
$root = "C:\Users\wogur\OneDrive\Desktop\project\trading"
$logFile = Join-Path $root "scripts\autostart.log"

function Write-Log($msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    Add-Content -Path $logFile -Value $line -Encoding utf8
}

try {
    $now = Get-Date
    $isWeekend = $now.DayOfWeek -eq [DayOfWeek]::Saturday -or $now.DayOfWeek -eq [DayOfWeek]::Sunday
    if ($isWeekend) {
        Write-Log "Weekend ($($now.DayOfWeek)) -- skipping auto-stop"
        exit 0
    }

    $running = Get-Process -Name "trading_bot" -ErrorAction SilentlyContinue
    if (-not $running) {
        Write-Log "Not running -- nothing to stop"
        exit 0
    }

    Stop-Process -Id $running.Id -Force
    Write-Log "Stopped trading_bot.exe (PID $($running.Id), after market close, $($now.ToString('HH:mm')))"
}
catch {
    Write-Log "ERROR: $($_.Exception.Message)"
    exit 1
}
