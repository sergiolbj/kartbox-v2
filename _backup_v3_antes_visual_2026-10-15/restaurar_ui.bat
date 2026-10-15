@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando ui.c de ANTES da mudanca visual (2026-10-15)...
copy /Y "_backup_v3_antes_visual_2026-10-15\main\ui.c" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
