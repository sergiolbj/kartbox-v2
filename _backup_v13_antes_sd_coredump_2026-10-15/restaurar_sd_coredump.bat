@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos de ANTES do ajuste do SD/coredump (2026-10-15)...
copy /Y "_backup_v13_antes_sd_coredump_2026-10-15\main\*.*" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
