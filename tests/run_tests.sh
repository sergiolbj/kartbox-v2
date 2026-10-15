#!/bin/sh
# Roda os testes das funcoes puras no PC (Linux/macOS/WSL/MSYS2).
cd "$(dirname "$0")" || exit 1
CC=${CC:-cc}
$CC -std=c11 -Wall -Wextra -O1 -o /tmp/kb_test_pure test_pure.c -lm || exit 1
/tmp/kb_test_pure
