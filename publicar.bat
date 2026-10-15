@echo off
rem Rode a partir do terminal do ESP-IDF (precisa de IDF_PATH).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publicar.ps1" %*
