@echo off
setlocal EnableExtensions

:: COM5 entry point.
::
:: The whole build/flash flow lives in ..deploy.bat, so both ports run the
:: same code: same tree selection (first argument, %FW_DIR%, or the worktree
:: menu), same verify step, same flash history. This file only picks the port
:: and opts in to writing the checkpoint.
::
:: deploy.bat refuses COM5 unless ALLOW_COM5=1, which only this file sets, so
:: an ordinary deploy.bat run still cannot overwrite the official firmware.
::
:: Usage is identical to the root script:
::   deploy_com5.bat                  menu (double-click, or cmd /k)
::   deploy_com5.bat <tree path>      that worktree, WSL path or UNC
::   set PLAN_ONLY=1                  print the resolved plan, flash nothing

set "ALLOW_COM5=1"
set "COM_PORT=COM5"

call "%~dp0..\deploy.bat" %*
exit /b %ERRORLEVEL%
