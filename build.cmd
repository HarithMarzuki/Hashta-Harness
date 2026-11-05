@echo off
rem Entry point that is not blocked by the PowerShell execution policy.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
