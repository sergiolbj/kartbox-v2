@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando gps.c, gps.h e ui.c de ANTES da auto-deteccao de pinos (2026-10-15)...
copy /Y "_backup_v4_antes_autopinos_2026-10-15\main\gps.c" "main\" >nul
copy /Y "_backup_v4_antes_autopinos_2026-10-15\main\gps.h" "main\" >nul
copy /Y "_backup_v4_antes_autopinos_2026-10-15\main\ui.c" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
