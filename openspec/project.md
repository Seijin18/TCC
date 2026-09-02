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
