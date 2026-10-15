@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos de ANTES do teste sem log/SD 20MHz (2026-10-15)...
copy /Y "_backup_v14_antes_bisect_sd_log_2026-10-15\main\*.*" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
