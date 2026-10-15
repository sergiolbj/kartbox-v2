@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando gps.c/kb_pure.h de ANTES da config GNSS (2026-10-15)...
copy /Y "_backup_v10_antes_gnss_cfg_2026-10-15\main\*.*" "main\" >nul
copy /Y "_backup_v10_antes_gnss_cfg_2026-10-15\tests\test_pure.c" "tests\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
