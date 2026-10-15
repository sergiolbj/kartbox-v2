@echo off
setlocal
cd /d "%~dp0.."
echo Restaurando versao FUNCIONANDO (2026-10-15, com correcoes + OTA ok)...
copy /Y "_backup_v2_funcionando_2026-10-15\main\*.c" "main\" >nul
copy /Y "_backup_v2_funcionando_2026-10-15\main\*.h" "main\" >nul
copy /Y "_backup_v2_funcionando_2026-10-15\main\CMakeLists.txt" "main\" >nul
copy /Y "_backup_v2_funcionando_2026-10-15\kartbox-telemetry.html" "." >nul
copy /Y "_backup_v2_funcionando_2026-10-15\sdkconfig.defaults" "." >nul
copy /Y "_backup_v2_funcionando_2026-10-15\version.txt" "." >nul
echo Pronto. Recompile (idf.py build) e envie por OTA.
echo OBS: se mudou sdkconfig.defaults, apague "sdkconfig" e rode idf.py reconfigure.
pause
