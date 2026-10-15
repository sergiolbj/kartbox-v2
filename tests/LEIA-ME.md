# Testes automaticos (rodam no PC, sem placa)

Testam o codigo de `main/kb_pure.h` - o MESMO codigo que o firmware usa
(gps.c, track_manager.c e ui.c incluem esse header).

## Como rodar
- Windows: de dois cliques em `run_tests.bat` (precisa de um compilador C de PC;
  se faltar, o proprio script diz como instalar).
- Linux/macOS/WSL: `./run_tests.sh`

Resultado esperado no fim: `... 0 falhas -> TUDO OK`

## O que cobre
- NMEA: checksum, coordenadas, campos vazios (bug do COG vazio)
- Geometria: distancia, diferenca de rumo, lado da linha
- Cruzamento da linha de chegada + interpolacao + filtro de raio
- Hora UTC do GPS (bissexto, virada de dia/ano, entradas ruins)
- Relogio GPS x local (anti-jitter) e erro do tempo de volta
- Nome de pista (caracteres proibidos, tamanho)
- Rotulo de sessao na aba VOLTAS (data legivel a partir do nome do arquivo)

## Quando rodar
Depois de mexer em `kb_pure.h` ou em qualquer regra de GPS/nome de pista.
Se falhar, o teste mostra a linha e o valor esperado.

## Limite
NAO testa tela, SD, WiFi, BLE nem o GPS real. Isso continua sendo teste na bancada/pista.
Para uma regra nova ser testavel, ponha ela em `kb_pure.h` (sem incluir nada do IDF).
