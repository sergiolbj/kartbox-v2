@echo off
setlocal
cd /d "%~dp0"
where gcc >nul 2>nul && ( gcc -std=c11 -Wall -Wextra -O1 -o test_pure.exe test_pure.c -lm && goto run )
where clang >nul 2>nul && ( clang -std=c11 -Wall -Wextra -O1 -o test_pure.exe test_pure.c && goto run )
echo.
echo Nao achei gcc nem clang neste terminal.
echo Instale um compilador de PC (so uma vez), por exemplo:
echo     winget install BrechtSanders.WinLibs.POSIX.UCRT
echo Feche e abra o terminal e rode run_tests.bat de novo.
echo (O terminal do ESP-IDF tem so o compilador da placa, nao serve pra isso.)
pause
exit /b 1
:run
test_pure.exe
set RC=%ERRORLEVEL%
del test_pure.exe >nul 2>nul
pause
exit /b %RC%
