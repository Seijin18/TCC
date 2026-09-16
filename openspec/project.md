# TCC: Sistema de Comunicação Anti-Jamming (NRF24L01 + ESP32)

## Contexto

TCC de graduação: sistema de comunicação em RF (2.4 GHz) resiliente a jamming
intencional, usando módulos NRF24L01+ e ESP32, com FHSS adaptativo (troca
dinâmica de seed + blacklisting de canais sob ataque).

O roadmap original do projeto está documentado em
`Guia_Desenvolvimento_TCC_AntiJamming.md` (5 fases) e no `README.md`. Esses
dois arquivos continuam sendo a referência de alto nível do projeto para a
banca; os changes do OpenSpec detalham e rastreiam a execução de cada lacuna
de implementação identificada dentro dessas fases.

## Arquitetura (nós do ecossistema)

- **mestre/** (`mestre/src/main.cpp`) — transmissor confiável: handshake,
  seed, FHSS, comando de mudanças táticas.
- **escravo/** (`escravo/src/main.cpp`) — receptor confiável: acompanha os
  saltos do mestre, deveria contabilizar QoS/PLR (ainda não implementado).
- **jammer/** (`jammer/src/main.cpp`) — atacante: hoje só sweep jammer
  dual-rádio; falta o modo targeted (perseguir a tabela de 16 canais).
- **analisador/** (`analisador/src/main.cpp`) — varre o espectro (até 126
  canais) e emite CSV cru pela serial; falta heurística de classificação de
  ataque e canal de alerta para o mestre.
- **shared/hopping.h** — tabela fixa de 16 canais, `get_next_channel`,
  struct `BeaconPacket` (definida mas não usada).
- **validacao/** (`validacao/src/main.cpp`) — harness que liga/desliga o
  jammer sob controle serial enquanto o analisador varre; usado pela
  ferramenta Python de espectro/gravação.
- **tools/spectrum/main.py** — GUI Tkinter: leitura serial, gravação de CSV,
  heatmap de espectro, controle remoto do jammer durante gravação.
- **Tests/** — `TestJammer` é um teste de hardware legítimo (ainda buildado
  via `env:test_jammer` do `platformio.ini`); `TestMaster`/`TestSlave` são
  protótipos antigos sem FHSS/seed, não buildados por nenhum environment
  atual — código morto, não referenciar como base para novas mudanças.

## Convenções

- Linguagem: C/C++ (Arduino core, PlatformIO) nos nós; Python (Tkinter,
  pandas, matplotlib) na ferramenta de espectro/coleta.
- Biblioteca de rádio: `RF24` (TMRh20).
- Cada environment do `platformio.ini` builda uma pasta (`mestre`, `escravo`,
  `jammer`, `analisador`, `validacao`, `test_jammer`) — ao adicionar código
  novo a um nó, confirmar que o environment correspondente aponta pra pasta
  certa antes de assumir que compila.
- `env:native_test` builda no host (gcc, `platform = native`, sem
  ESP32/Arduino/RF24) e roda testes unitários de lógica pura em `test/`
  (framework Unity, bundled no PlatformIO). Cobre hoje `shared/hopping.h`
  (`get_next_channel`/`get_hop_channel`). Rodar com `pio test -e
  native_test`. Ao extrair nova lógica pura de um `main.cpp` (ex.: cálculo
  de PLR, classificação de ataque) para um header compartilhado, adicionar
  o teste correspondente aqui em vez de depender só da validação manual em
  bancada.
- Mensagens de commit e fluxo de trabalho seguem as instruções globais do
  usuário (commits locais automáticos ao fechar uma unidade coerente de
  trabalho; nunca `push`).

## Estado em 2026-09-01 (linha de base para os changes abaixo)

Levantamento completo comparando código vs. as 5 fases do guia está no
histórico da conversa que originou estes changes; resumo por fase:

- **Fase 1** (protocolo/FHSS): handshake, tabela de 16 canais, seed e
  hopping ✅ implementados. Beacon dedicado 🟡 (struct existe, não usada —
  sincronismo é via fragmento+ACK por hop). **QoS/PLR ❌ não implementado.**
- **Fase 2** (jammer): sweep dual-rádio ✅. **Targeted (mira nos 16 canais
  do FHSS) ❌ não implementado.**
- **Fase 3** (analisador): varredura de espectro ✅ (mas limitada a 2-80
  por padrão via `FAST_BT_SCAN=1`). **Heurística de classificação de ataque
  ❌ e alerta Analisador→Mestre ❌ não implementados.**
- **Fase 4** (mitigação): **nada implementado** — seed é fixada uma vez
  antes do hopping começar, `HOPPING_CHANNELS` é `const`, sem blacklist.
- **Fase 5** (dados/gráficos): coleta CSV ✅, heatmap/live plot ✅. Falta
  automação dos 3 cenários comparativos do guia e inclusão de PLR no
  pipeline (depende da Fase 1).

Nota histórica: o commit `8188180` ("Clear files") removeu 7 documentos de
planejamento sobre uma proposta de refactor do jammer (perfis de potência,
comando `targeted`, `stats`, health-check) que nunca chegou a ser aplicada
ao código em nenhum commit. O conteúdo ainda é recuperável via
`git show 97fca79:<arquivo>.md` e pode servir de referência de design para
o change `targeted-jammer`, mas não deve ser tratado como já implementado.

## Changes ativos e ordem de dependência

Cinco lacunas foram convertidas em changes (`openspec/changes/<slug>/`), um
por lacuna, para poder arquivar cada um ao fechar a fase correspondente:

1. **`plr-metric`** (Fase 1) — sem dependências. Pré-requisito de métrica
   para `dynamic-seed-blacklist` (validar self-healing) e
   `scenario-automation` (comparar cenários por PLR).
2. **`targeted-jammer`** (Fase 2) — sem dependências, pode rodar em paralelo
   com `plr-metric`. Pré-requisito de sinal de ataque realista para
   `spectrum-attack-classification` (a heurística precisa de um ataque
   dirigido real para validar a distinção ruído-aleatório vs. ataque).
3. **`spectrum-attack-classification`** (Fase 3) — depende de
   `targeted-jammer` para validação empírica da heurística. Pré-requisito
   do sinal de alerta que `dynamic-seed-blacklist` consome.
4. **`dynamic-seed-blacklist`** (Fase 4) — depende de `plr-metric` (mede a
   recuperação) e `spectrum-attack-classification` (dispara a mitigação).
5. **`scenario-automation`** (Fase 5) — depende de todos os anteriores;
   fecha o projeto rodando os 3 cenários comparativos do guia com PLR.

Ordem recomendada de execução: `plr-metric` e `targeted-jammer` em paralelo
→ `spectrum-attack-classification` → `dynamic-seed-blacklist` →
`scenario-automation`.

## Bancada real: 2 placas, não 4 (2026-09-16)

A bancada disponível tem **2 placas ESP32**, não as 4 do cenário de
referência descrito acima. `docs/pinout.md` já documentava esse arranjo
alternativo (Placa A: mestre + analisador; Placa B: escravo + jammer) como
hipotético; o change `two-board-topology`
(`openspec/changes/two-board-topology/`) implementa o firmware combinado
necessário (duas tasks FreeRTOS pinadas a núcleos por placa, alerta de
`spectrum-attack-classification` via flag compartilhada em vez de GPIO
físico na Placa A). É aditivo — não substitui nem quebra os environments
standalone de 4 placas — e roda em paralelo aos changes de fase acima,
consumindo a lógica de cada um deles (`node.h` extraído de
`mestre`/`analisador`/`escravo`/`jammer`) sem duplicá-la. Deve ser mantido
em dia conforme cada change de fase evolui, já que ele depende do código
mais recente de todos os 4 nós.

## Auditoria do projeto de IC de referência (2026-09-02)

O TCC partiu do projeto de IC `github.com/Seijin18/IC` (pasta `RFmodule/`,
firmware `espReceiver.ino` + `tools/spectrum/main.py` deste repo derivam
dele — ver comentário em `analisador/src/main.cpp:5`). Esse projeto de IC
continuou evoluindo depois que o TCC divergiu e hoje inclui um artigo
publicado (`Artigo/conference_101719.tex`) com dados empíricos de detecção
de jamming no mesmo hardware, além de uma refatoração de precisão de
timing. Auditoria feita clonando o repo e comparando com o estado atual
deste projeto; achados:

- **Refatoração de timing (ESP32 `millis()` → Python `time.perf_counter()`
  no recebimento serial)**: já presente neste projeto de forma equivalente
  (`analisador/src/main.cpp` não envia timestamp;
  `tools/spectrum/main.py:198` já usa `time.perf_counter()`) — nenhuma ação
  necessária, os dois projetos convergiram nessa solução independentemente.
- **Metodologia de calibração de detecção de jamming** (métrica de energia
  acumulada por sub-banda, latência de detecção alcançada, protocolo de
  captura contínua com múltiplos ciclos liga/desliga) — documentada como
  referência nos `design.md` de
  [`spectrum-attack-classification`](changes/spectrum-attack-classification/design.md)
  e [`scenario-automation`](changes/scenario-automation/design.md), a
  considerar na calibração empírica (tasks 3.x) e na orquestração (task
  3.1) desses changes quando forem implementados — não altera nenhuma
  decisão de design já tomada.
- **Sem novidade nos demais changes**: `plr-metric`, `targeted-jammer` e
  `dynamic-seed-blacklist` cobrem a Fase 1/2/4, que não têm equivalente no
  projeto de IC (ele só faz varredura de espectro passiva/GUI, não
  implementa o par mestre/escravo com FHSS nem jamming targeted) — nada a
  incorporar deles nesses três changes.
- Nenhum código foi alterado por esta auditoria, só os `design.md` acima e
  esta seção.
