@echo off
setlocal EnableExtensions
chcp 65001 >nul

echo ============================================================
echo  Creating Desktop Shortcut for WQN Note4 COM5 Deploy
echo ============================================================
echo.

powershell.exe -NoProfile -Command ^
    "$wsh = New-Object -ComObject WScript.Shell; " ^
    "$desktop = [System.Environment]::GetFolderPath('Desktop'); " ^
    "$shortcutPath = Join-Path $desktop 'WQN Note4 Deploy (COM5).lnk'; " ^
    "$shortcut = $wsh.CreateShortcut($shortcutPath); " ^
    "$shortcut.TargetPath = $env:ComSpec; " ^
    "$shortcut.Arguments = '/k \"\"\\wsl.localhost\Ubuntu\home\unknow\projects\firmware\firmware\wqn-zectrix-note4\scripts\deploy_com5.bat\"\"'; " ^
    "$shortcut.WorkingDirectory = $env:USERPROFILE; " ^
    "$shortcut.Description = 'WQN Note4 Firmware Deployment for COM5 (Auto-Backup)'; " ^
    "$shortcut.Save(); " ^
    "Write-Host '[OK] Shortcut created successfully at:' $shortcutPath"

echo.
pause
endlocal
