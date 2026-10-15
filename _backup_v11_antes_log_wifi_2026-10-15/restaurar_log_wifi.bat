@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos de ANTES do log via WiFi (2026-10-15)...
copy /Y "_backup_v11_antes_log_wifi_2026-10-15\main\*.*" "main\" >nul
if exist logview.html del logview.html
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
