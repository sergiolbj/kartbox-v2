@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando editor/wifi_export de ANTES do botao GPS do kartbox (2026-10-15)...
copy /Y "_backup_v6_antes_gps_editor_2026-10-15\main\wifi_export.c" "main\" >nul
copy /Y "_backup_v6_antes_gps_editor_2026-10-15\editor.html" "." >nul
copy /Y "_backup_v6_antes_gps_editor_2026-10-15\track_editor.html" "." >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
