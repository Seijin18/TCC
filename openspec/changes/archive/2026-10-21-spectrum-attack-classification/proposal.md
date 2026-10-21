## Why

A Fase 3 do guia pede que o analisador não só varra o espectro, mas
distinga *ruído aleatório* (Wi-Fi, Bluetooth) de *ataque direcionado*
(energia concentrada nos 16 canais da tabela de hopping) e avise o mestre.
Hoje `analisador/src/main.cpp` só mede atividade por canal (0-100%) e
imprime um CSV cru pela serial — não existe nenhuma lógica de classificação
nem qualquer canal de comunicação entre o analisador e o mestre
(`grep -i "classif\|alerta"` não retorna nada no firmware). Essa é a peça
central de "inteligência" do TCC: sem ela, a Fase 4 (mitigação) não tem
gatilho automático para agir, e o projeto fica limitado a defesa
manual/observação humana.

## What Changes

- `analisador/src/main.cpp` passa a comparar, a cada ciclo de varredura, a
  atividade média nos 16 canais de `HOPPING_CHANNELS` contra a atividade
  média no restante do espectro, mantendo um histórico curto (últimos M
  ciclos) para exigir persistência antes de declarar ataque (evita alarme
  falso por um pico isolado).
- Ao classificar "ataque direcionado" sustentado, o analisador ativa um pino
  digital de alerta (nível lógico HIGH enquanto o ataque persistir) — canal
  de hardware entre as duas placas, conforme sugerido pelo guia, evitando
  contenção com o rádio NRF24 do analisador (que já está ocupado
  escaneando).
- `mestre/src/main.cpp` ganha leitura desse pino (polling ou interrupção) e
  loga a transição de estado ("ALERTA: ataque detectado" / "alerta
  encerrado") pela serial. Este change **não** implementa reação (troca de
  seed/blacklist) — isso é `dynamic-seed-blacklist`, que consome esse
  sinal.

## Capabilities

### New Capabilities
- `attack-detection`: heurística de classificação de ataque direcionado
  vs. ruído aleatório no analisador, com sinalização de alerta por pino
  digital consumida pelo mestre.

### Modified Capabilities
(nenhuma)

## Impact

- `analisador/src/main.cpp`: novo estado de histórico de ciclos, nova
  função de classificação, novo pino de saída digital.
- `mestre/src/main.cpp`: novo pino de entrada, novo log de transição de
  estado de alerta. Nenhuma mudança na lógica de hopping/seed neste
  change.
- Hardware: requer um fio de sinal (GPIO) entre a placa do analisador e a
  do mestre, além do GND comum já necessário — documentar no `README.md`
  como parte da implementação (fora do escopo deste `proposal.md`, tratado
  em `tasks.md`).
