# Histórico do Projeto — KartBox v2

Registro cronológico das mudanças no repositório, com base no histórico de commits do Git.

---

## [2026-07-18] feat: bateria, editor de pista, case 3D e novas telas de UI
**Commit:** `40c1a8b`

- Monitoramento de bateria (`main/battery.c`, `main/battery.h`)
- Editor de traçado de pista via navegador (`track_editor.html`, `editor.html`)
- Modelos 3D do case impresso (`case/` — arquivos `.scad`, `.stl`, `.3mf`)
- Nova fonte de UI `font_kartbox_2xl`
- Exportação de sessões simuladas para testes (`sessions_simuladas/20260704_143000.csv`)
- Atualizações extensas em GPS, SD logger, BLE, Wi-Fi export, settings e UI
- Documentação de produto (`PRODUTO_KARTBOX.md`) e materiais de marca (`LapWise_Manual_de_Marca.pdf`)
- Novo `partitions.csv` e `version.txt` (`1.0.0-beta`)

---

## [2026-06-20] feat: BLE security - SMP pairing + application PIN auth
**Commit:** `8cf1fb3`

- **Parte B (SMP):** NimBLE configurado com `DISP_ONLY`, bonding, MITM e Secure Connections. Passkey de 6 dígitos exibido em overlay LVGL (`ui_show_ble_passkey()` / `ui_hide_ble_passkey()`).
- **Parte A (PIN de aplicação):** comando `0x00 AUTH` obrigatório antes de qualquer outro comando; `s_authed` resetado a cada conexão. PIN vazio em NVS desativa a verificação de aplicação (SMP continua exigindo pareamento).
- `ble_pin[8]` persistido em NVS (`settings.c/h`).
- `kartbox-telemetry.html`: tela de PIN pós-conexão, comando `CMD.AUTH`, payload de settings de 82 bytes, campo de PIN na aba Config.

---

## [2026-06-20] feat(web): responsive white theme, map editor, lap history
**Commit:** `d996183`

- Tema claro em toda a interface web (fundo claro, acentos verdes)
- Layout responsivo: tela cheia no mobile, card centralizado de 440px no desktop
- Aba PISTAS: mapa Leaflet/OSM para posicionar pinos de largada/setor arrastáveis, usando GPS + bússola do dispositivo
- Aba TELEM: histórico de voltas abaixo do contador, mais recente no topo, melhor volta destacada com estrela
- Delta congelado a partir da última volta completa (negativo = novo recorde)

---

## [2026-06-20] feat: BLE bidirecional — controle remoto + download de sessões
**Commit:** `4f78538`

- Expansão significativa de `main/ble_telemetry.c/h` para suportar comandos bidirecionais
- Controle remoto via BLE e download de sessões gravadas
- Refatoração grande do app web (`kartbox-telemetry.html`)

---

## [2026-06-20] feat: BLE telemetry web app
**Commit:** `8d1376f`

- Primeira versão do app web de telemetria via BLE (`kartbox-telemetry.html`)

---

## [2026-06-20] feat: KartBox v2 initial commit
**Commit:** `aeb3e38`

Commit inicial do projeto, já com a base funcional completa:

- Hardware: ESP32-P4 + ESP32-C6, display LVGL v9, MIPI-DSI 480x320
- Cronometragem de voltas via GPS com detecção de linha de chegada (posição + heading)
- Sector timing configurável (até 2 setores)
- Aba PISTA: salvar/carregar pistas `.trk` no cartão SD
- Aba CORRIDA: velocidade, delta e best lap em tempo real
- Aba VOLTAS: histórico de sessões com tempos
- Aba CONFIG: fuso horário, raio de gate, BLE, exportação Wi-Fi
- Persistência de settings e última pista usada via NVS
- `track_manager`: CRUD de pistas em `/sdcard/tracks/`

---

## Notas

- Arquivos como `sdkconfig.old` e imagens com nome de hash aleatório (ex.: `S5fdd0f6015b14e1e9c3c0b1b755ffa70C.webp`) foram deliberadamente deixados fora do controle de versão por parecerem artefatos temporários.
- Este arquivo é mantido manualmente a partir do `git log`; para o detalhamento exato de cada alteração, consulte `git log --stat` ou `git show <hash>`.
