# Once per day, if trading_bot.exe is not currently running, runs headless Claude in fully
# autonomous mode to: analyze recent performance -> improve the strategy code -> verify via
# build+tests -> record findings in PROGRESS.md -> git commit/push. No human approval in the
# loop. Hard rules (never switch mode to live, never kill a running trading process, never
# commit secrets, etc.) live in scripts/daily_improvement_prompt.md -- that file is the real
# instructions; this script is just the trigger/guard.
#
# Registered in Windows Task Scheduler to run at logon + daily at 08:40
# (see scripts/register_scheduled_tasks.ps1).
#
# Log messages are kept in English on purpose: Windows PowerShell 5.1 parses a BOM-less UTF-8
# .ps1 file using the system ANSI codepage, so literal Korean text embedded in this script
# gets mangled before it's even written to the log.

$ErrorActionPreference = 'Stop'
$root = "C:\Users\wogur\OneDrive\Desktop\project\trading"
$logFile = Join-Path $root "scripts\claude_daily.log"
$guardFile = Join-Path $root "scripts\.last_claude_review_date"
$promptFile = Join-Path $root "scripts\daily_improvement_prompt.md"
$claudeExe = "C:\Users\wogur\.local\bin\claude.exe"

function Write-Log($msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    Add-Content -Path $logFile -Value $line -Encoding utf8
}

try {
    $now = Get-Date
    $isWeekend = $now.DayOfWeek -eq [DayOfWeek]::Saturday -or $now.DayOfWeek -eq [DayOfWeek]::Sunday
    if ($isWeekend) {
        Write-Log "Weekend ($($now.DayOfWeek)) -- skipping daily review (no new trading data)"
        exit 0
    }

    $today = Get-Date -Format "yyyy-MM-dd"
    if ((Test-Path $guardFile) -and ((Get-Content $guardFile -Raw).Trim() -eq $today)) {
        Write-Log "Already ran today ($today) -- skipping"
        exit 0
    }

    $running = Get-Process -Name "trading_bot" -ErrorAction SilentlyContinue
    if ($running) {
        Write-Log "trading_bot.exe is running (PID $($running.Id)) -- skipping this run to avoid a build/exe-lock conflict"
        exit 0
    }

    if (-not (Test-Path $claudeExe)) {
        Write-Log "claude.exe not found: $claudeExe"
        exit 1
    }
    if (-not (Test-Path $promptFile)) {
        Write-Log "Prompt file not found: $promptFile"
        exit 1
    }

    Set-Location $root
    Write-Log "Starting daily analysis/improvement session"

    # Passing a multi-KB Korean prompt as a single command-line ARGUMENT to a native exe
    # got silently mangled (PowerShell<->native-process argument marshalling issue observed
    # 2026-09-07 -- the prompt arrived at claude.exe corrupted/truncated). Piping it via stdin
    # instead avoids that; claude -p with no prompt argument reads the prompt from stdin.
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8

    Get-Content -Path $promptFile -Raw -Encoding UTF8 |
        & $claudeExe -p --dangerously-skip-permissions --max-budget-usd 5 *>> $logFile
    $exitCode = $LASTEXITCODE
    Write-Log "Daily analysis/improvement session ended (exit=$exitCode)"

    # Always record today's date, even if the session errored or hit the budget cap, so a
    # crash loop doesn't retry endlessly within the same day.
    Set-Content -Path $guardFile -Value $today
}
catch {
    Write-Log "ERROR: $($_.Exception.Message)"
    exit 1
}
