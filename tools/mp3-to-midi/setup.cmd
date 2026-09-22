@echo off
rem Runs setup.ps1 with -ExecutionPolicy Bypass for this one invocation, so it
rem works without changing the system execution policy.
rem   setup.cmd -Nvidia   installs the CUDA build (NVIDIA GPUs only).
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
