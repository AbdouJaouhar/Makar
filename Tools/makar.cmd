@echo off
rem Makar launcher: "makar help" lists what can be opened. Works from cmd and PowerShell, from any folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0makar-launcher.ps1" %*
