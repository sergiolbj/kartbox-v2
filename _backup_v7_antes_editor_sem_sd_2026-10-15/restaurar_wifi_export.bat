@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando wifi_export.c de ANTES de ignorar o editor.html do SD (2026-10-15)...
copy /Y "_backup_v7_antes_editor_sem_sd_2026-10-15\main\wifi_export.c" "main\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
