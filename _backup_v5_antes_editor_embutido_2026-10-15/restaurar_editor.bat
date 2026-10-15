@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando editor SEM embutir (2026-10-15)...
copy /Y "_backup_v5_antes_editor_embutido_2026-10-15\main\wifi_export.c" "main\" >nul
copy /Y "_backup_v5_antes_editor_embutido_2026-10-15\main\CMakeLists.txt" "main\" >nul
copy /Y "_backup_v5_antes_editor_embutido_2026-10-15\editor.html" "." >nul
copy /Y "_backup_v5_antes_editor_embutido_2026-10-15\track_editor.html" "." >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
