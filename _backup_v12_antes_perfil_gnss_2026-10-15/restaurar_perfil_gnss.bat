@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando gps.c de ANTES do perfil GNSS GPS+GAL+GLO (2026-10-15)...
copy /Y "_backup_v12_antes_perfil_gnss_2026-10-15\main\gps.c" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
