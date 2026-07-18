# KartBox v2 — Telemetria profissional de kart, no seu painel

**Cronômetro GPS de voltas com tela touch, análise de engenheiro de pista e zero mensalidade.** Feito para quem leva kart rental a sério: chegue, prenda no volante, ande — o resto ele faz sozinho.

---

## Por que o KartBox?

Cronômetros comerciais custam caro, cobram assinatura pela análise e tratam kart rental como cidadão de segunda classe. O KartBox nasceu na pista: cada função existe porque fez falta numa bateria de verdade.

- **Tela touch IPS de 800×480** legível ao sol, com fontes gigantes pensadas para leitura com o rabo do olho a 90 km/h
- **GPS u-blox M10 de 10Hz** multi-constelação (GPS, Galileo, BeiDou, GLONASS) com A-GPS para fix em segundos
- **Sem app obrigatório, sem nuvem, sem mensalidade** — seus dados ficam no seu cartão SD, a análise roda no navegador do seu celular

---

## Na pista

**Cronometragem de precisão real.** A linha de chegada é um cruzamento vetorial calculado por interpolação entre amostras — resolução temporal melhor que o próprio GPS. Filtro de direção elimina falsos positivos de trechos paralelos.

**Delta ao vivo estilo Fórmula 1.** Barra de LEDs segmentada (linguagem de shift-light) + número: verde ganhando, vermelho perdendo, comparado ao ritmo da sua melhor volta ponto a ponto — não só no fim da volta.

**Delta de setor no instante do cruzamento.** Cruzou o S1: `S1 22.41 (-0.12)` verde na tela. Você sabe onde ganhou e onde perdeu *durante* a volta.

**Volta prevista e volta ideal.** Projeção da volta atual em tempo real + a soma dos seus melhores setores da sessão ("o tempo existe, falta juntar").

**Três layouts de tela, um toque de distância.** Completo (tudo) → **delta gigante** (caçar tempo no qualy) → **velocidade gigante**. Um tap no botão MODE ou na tela cicla; **cada modo de condução lembra seu layout preferido**.

**Modos QUALY e RACE.** Qualy cronometra desde o primeiro cruzamento; Race só arma o relógio na largada real (kart saindo da inércia). Segure MODE por 1s para alternar — com barra de progresso, sem troca acidental.

**Auto-sessão.** Andou, gravou. Parou por 2 minutos, encerrou e mostrou o resumo: voltas, best, média, consistência (± desvio) e velocidade máxima. O botão RESET continua lá — mas você não precisa mais lembrar dele.

**Setores automáticos ou manuais.** Não marcou setor? O KartBox divide a volta em terços por distância, sozinho.

---

## Suas pistas, do seu jeito

- **Editor de pista no celular** com mapa de satélite: clique na linha de chegada, aponte a direção, pronto. Setores opcionais no mesmo fluxo.
- **Marcação em campo**: botão físico SET LINE grava posição + direção onde você estiver — pista nova configurada em uma volta.
- Pistas salvas no SD, carregáveis em dois toques; raio do gate, tempo mínimo de volta e fuso configuráveis na tela.

---

## Depois da bateria: análise de engenheiro de pista

Ligue o WiFi do KartBox (ponto de acesso próprio ou na sua rede) e abra `kartbox.local` no celular:

**Análise de voltas** — gráfico velocidade × distância comparando duas voltas, com linhas de setor, **mapa do traçado sincronizado** (arraste o dedo no gráfico e veja o ponto exato na pista) e trechos de GPS fraco destacados.

**Insights automáticos** — sem IA, sem nuvem: matemática de telemetria de verdade.
> *"S2: a volta 3 perde 0.358s vs a volta 2" · "Curva aos ~420m: freia 14m mais cedo" · "Queda de rendimento a partir da volta 8 — fadiga ou pneus" · "Volta ideal: 41.870s, 0.45s abaixo do seu best"*

**Evolução entre sessões** — best, média e consistência por dia, agrupado por pista: a resposta definitiva para "estou evoluindo?".

**Relatório imprimível** em um clique, **comparação toque-a-toque** direto na tela do aparelho (com setores), **mapa com heatmap de velocidade** e visão "só a melhor volta".

**Exportação universal**: CSV bruto, **GPX**, **VBO (RaceLogic)** e **RaceChrono** — convertidos no próprio navegador.

---

## Telemetria ao vivo no celular

App web via **Bluetooth** (instalável como PWA): velocidade, delta, voltas e setores a 10Hz no seu celular ou no do seu mecânico de box. Controle remoto completo — configurações, pistas e sessões — protegido por pareamento com passkey no display + PIN de aplicação.

---

## Feito para durar (e para mexer)

- **Atualização OTA pelo navegador** com validação de imagem e rollback automático — update ruim não brica o aparelho
- **A-GPS AssistNow**: efemérides injetadas pela internet ou por arquivo — cold start de minutos vira segundos
- **Backup/restore** de toda a configuração num arquivo legível no SD
- **Autoteste na tela** (SD, GPS, rádio) + **diagnóstico de RF por constelação** — descubra problema de antena no kartódromo, sem notebook
- **Coredump automático**: se travar em campo, o diagnóstico completo vai pro SD para análise em casa
- **Modo pen drive**: o SD vira unidade USB no computador, sem tirar o cartão
- **5 temas de cor** (verde, azul, âmbar, laranja, roxo) com proteção de daltonismo funcional: alertas continuam sempre vermelho/dourado
- **Brilho ajustável**, sessões protegidas contra toque acidental, versão do firmware na tela

---

## Especificações

| | |
|---|---|
| Processador | ESP32-P4 dual-core RISC-V + coprocessador de rádio |
| Memória | 32 MB PSRAM · 16 MB flash |
| Display | IPS touch capacitivo 800×480 |
| GPS | u-blox M10, 10 Hz, GPS + Galileo + BeiDou + GLONASS, A-GPS |
| Conectividade | WiFi 2.4 GHz (AP ou cliente) · Bluetooth LE · USB |
| Armazenamento | microSD (sessões em CSV aberto) |
| Controles | 3 botões físicos com luva + touch |
| Alimentação | bateria própria (independente do kart) |
| Software | atualizável OTA · dados 100% locais · formatos abertos |

---

*KartBox v2 — porque a diferença entre você e o cara da frente está nos dados que ele não tem.*
