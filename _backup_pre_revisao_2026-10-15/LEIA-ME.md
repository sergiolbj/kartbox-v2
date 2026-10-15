# Backup anterior a revisão de 2026-10-15

Estes são os arquivos **exatamente como estavam** antes da revisão (idênticos ao commit `82f8e74` do git, branch `main`, e à versão que estava funcionando no aparelho).
Os tamanhos em bytes foram conferidos contra a listagem da pasta feita antes da primeira alteração.

Arquivos: main/wifi_export.c, ble_telemetry.c, sd_logger.c, gps.c, gps.h, main.c, track_manager.c, ui.c e kartbox-telemetry.html.

## Como voltar tudo ao original

**Opção A — dois cliques:** execute `restaurar_originais.bat` (copia os arquivos desta pasta de volta por cima dos atuais).

**Opção B — git (se não tiver commitado as mudanças novas):**
```
git restore --source=82f8e74 -- main kartbox-telemetry.html
```
(ou `git checkout -- main kartbox-telemetry.html`)

**Opção C — git, depois de commitar:**
```
git revert <commit-da-revisao>
```

Depois de restaurar, rode `idf.py build flash` normalmente.
Pode apagar esta pasta quando tiver validado a versão nova.
