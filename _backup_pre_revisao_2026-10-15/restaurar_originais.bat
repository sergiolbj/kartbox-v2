@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando arquivos originais (pre-revisao 2026-10-15)...
copy /Y "_backup_pre_revisao_2026-10-15\main\*.c" "main\" >nul
copy /Y "_backup_pre_revisao_2026-10-15\main\*.h" "main\" >nul
copy /Y "_backup_pre_revisao_2026-10-15\kartbox-telemetry.html" "." >nul
echo Pronto. Recompile e grave o firmware (idf.py build flash).
pause
