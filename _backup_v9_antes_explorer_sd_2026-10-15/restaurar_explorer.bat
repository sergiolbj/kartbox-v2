@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos de ANTES do explorer do SD (2026-10-15)...
copy /Y "_backup_v9_antes_explorer_sd_2026-10-15\main\*.*" "main\" >nul
copy /Y "_backup_v9_antes_explorer_sd_2026-10-15\tests\test_pure.c" "tests\" >nul
if exist explorer.html del explorer.html
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
