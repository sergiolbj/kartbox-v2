# Backup: versao FUNCIONANDO (2026-10-15)
Estado com as correcoes da revisao + OTA testado e funcionando.
Antes das proximas mudancas (precisao da volta, rollback OTA, testes).

Voltar: rode `restaurar_versao_funcionando.bat` (nesta pasta), depois `idf.py build` e OTA.
Ainda mais antigo (original pre-revisao): `..\_backup_pre_revisao_2026-10-15\`.

Nota: depois desta rodada existe `main\kb_pure.h` e a pasta `tests\`. Restaurar este backup NAO apaga eles, mas os
arquivos restaurados nao os usam (kb_pure.h fica sem uso, sem problema).
