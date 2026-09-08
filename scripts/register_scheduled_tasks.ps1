# Registers three Windows Task Scheduler tasks:
#  - TradingBot-AutoStart: at logon + daily at 09:00 -> start_trading_bot.ps1
#      (starts trading_bot.exe if it's regular market hours 09:00-15:30, no-op otherwise)
#  - TradingBot-AutoStop: daily at 15:35 -> stop_trading_bot.ps1
#      (stops trading_bot.exe shortly after regular market hours close, no-op on weekends or
#       if it isn't running)
#  - TradingBot-ClaudeDailyReview: at logon + daily at 08:40 -> run_daily_claude_review.ps1
#      (once a day, only if the bot isn't running, runs headless Claude to analyze/improve/
#       commit/push fully autonomously)
#
# MUST be run from an elevated ("Run as Administrator") PowerShell window -- registering a
# scheduled task requires admin rights in this environment even for per-user AtLogOn triggers.
# Safe to re-run (uses -Force to overwrite any existing registration).
#
# To remove:
#   Unregister-ScheduledTask -TaskName "TradingBot-AutoStart" -Confirm:$false
#   Unregister-ScheduledTask -TaskName "TradingBot-AutoStop" -Confirm:$false
#   Unregister-ScheduledTask -TaskName "TradingBot-ClaudeDailyReview" -Confirm:$false

$ErrorActionPreference = 'Stop'
$root = "C:\Users\wogur\OneDrive\Desktop\project\trading"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: this script must be run from an elevated (Run as Administrator) PowerShell window." -ForegroundColor Red
    exit 1
}

$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -StartWhenAvailable `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

# --- TradingBot-AutoStart ---
$actionStart = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$root\scripts\start_trading_bot.ps1`""
$triggersStart = @(
    New-ScheduledTaskTrigger -AtLogOn
    New-ScheduledTaskTrigger -Daily -At 9:00AM
)
Register-ScheduledTask -TaskName "TradingBot-AutoStart" `
    -Action $actionStart -Trigger $triggersStart -Settings $settings `
    -Description "Starts trading_bot.exe automatically during regular market hours (weekdays 09:00-15:30)" `
    -Force | Out-Null
Write-Host "Registered TradingBot-AutoStart"

# --- TradingBot-AutoStop ---
$actionStop = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$root\scripts\stop_trading_bot.ps1`""
$triggerStop = New-ScheduledTaskTrigger -Daily -At 3:35PM
Register-ScheduledTask -TaskName "TradingBot-AutoStop" `
    -Action $actionStop -Trigger $triggerStop -Settings $settings `
    -Description "Stops trading_bot.exe shortly after regular market hours close (weekdays 15:35)" `
    -Force | Out-Null
Write-Host "Registered TradingBot-AutoStop"

# --- TradingBot-ClaudeDailyReview ---
$actionReview = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$root\scripts\run_daily_claude_review.ps1`""
$triggersReview = @(
    New-ScheduledTaskTrigger -AtLogOn
    New-ScheduledTaskTrigger -Daily -At 8:40AM
)
Register-ScheduledTask -TaskName "TradingBot-ClaudeDailyReview" `
    -Action $actionReview -Trigger $triggersReview -Settings $settings `
    -Description "Daily (once): analyze performance -> improve strategy code -> verify via build/tests -> update PROGRESS.md -> git commit/push (fully autonomous)" `
    -Force | Out-Null
Write-Host "Registered TradingBot-ClaudeDailyReview"

Get-ScheduledTask -TaskName "TradingBot-*" | Format-Table TaskName, State
