' Launches a PowerShell script completely hidden -- no console window flash.
'
' powershell.exe's own -WindowStyle Hidden still briefly flashes a console window when Task
' Scheduler launches it in an Interactive-logon task: conhost.exe creates the console window
' first, and only afterwards does powershell.exe apply the hidden style, leaving a visible gap.
' wscript.exe has no console subsystem of its own, and WshShell.Run's windowStyle=0 requests
' SW_HIDE as part of process creation itself, so no window is ever created at all.
'
' Usage: wscript.exe //B run_hidden.vbs "C:\path\to\script.ps1"

Dim shell, scriptPath, cmd
Set shell = CreateObject("WScript.Shell")
scriptPath = WScript.Arguments(0)
cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File """ & scriptPath & """"
shell.Run cmd, 0, False
