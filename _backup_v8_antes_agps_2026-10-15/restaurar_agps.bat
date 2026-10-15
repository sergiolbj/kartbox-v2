@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos de ANTES do A-GPS ZTP (2026-10-15)...
copy /Y "_backup_v8_antes_agps_2026-10-15\main\*.*" "main\" >nul
copy /Y "_backup_v8_antes_agps_2026-10-15\tests\test_pure.c" "tests\" >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
pause
