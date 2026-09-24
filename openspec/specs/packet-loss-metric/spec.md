# packet-loss-metric Specification

## Purpose
TBD - created by archiving change plr-metric. Update Purpose after archive.
## Requirements
### Requirement: Contagem de hops esperados vs. recebidos
O escravo DEVE (MUST) incrementar um contador de "hop esperado" a cada
`HOP_INTERVAL_MS` decorrido (mesmo ritmo usado para seguir o canal do
mestre) e um contador de "hop recebido" sempre que um `FragmentPacket`
válido for recebido dentro daquele hop.

#### Scenario: Hop com recepção bem-sucedida
- **WHEN** o escravo está no canal correto do hop atual e recebe um
  `FragmentPacket` válido antes do próximo salto
- **THEN** o contador de hops recebidos da janela atual é incrementado em 1

#### Scenario: Hop sem recepção (perda)
- **WHEN** o intervalo do hop atual termina (`HOP_INTERVAL_MS` decorrido)
  sem que nenhum `FragmentPacket` válido tenha sido recebido
- **THEN** o contador de hops esperados da janela atual é incrementado em 1
  e o contador de recebidos não é incrementado, contando como perda daquele
  hop

### Requirement: Cálculo de PLR por janela deslizante e por sessão
O escravo DEVE (MUST) calcular `plr_window = 1 - (recebidos_janela /
esperados_janela)` a cada N hops fechados (N configurável, padrão 50), e
`plr_session = 1 - (recebidos_total / esperados_total)` acumulado desde o
início da sessão de hopping atual (reiniciado a cada novo handshake/seed).

#### Scenario: Janela completa sem nenhuma perda
- **WHEN** uma janela de N hops fecha com `recebidos_janela == N`
- **THEN** `plr_window` reportado é 0%

#### Scenario: Janela completa com perda total (ataque efetivo)
- **WHEN** uma janela de N hops fecha com `recebidos_janela == 0`
- **THEN** `plr_window` reportado é 100%

### Requirement: Log serial parseável por janela
O escravo DEVE (MUST) emitir uma linha de log serial a cada janela fechada, em
formato estável e parseável (chave=valor ou CSV), contendo no mínimo:
timestamp local (ms), `plr_window`, `plr_session`, `recebidos_janela` e
`esperados_janela`.

#### Scenario: Fechamento de janela durante operação normal
- **WHEN** uma janela de N hops fecha
- **THEN** uma linha de log é emitida na serial contendo todos os campos
  exigidos, sem interromper o loop de hopping (não bloqueante)

### Requirement: PLR sob link inativo
O escravo NÃO DEVE (MUST NOT) emitir linhas de PLR antes de completar o handshake/recebimento de seed (sem sessão de hopping ativa) — a métrica só é significativa depois que o link está ativo.

#### Scenario: Antes do handshake completar
- **WHEN** o escravo ainda está aguardando a seed do mestre
- **THEN** nenhuma linha de log de PLR é emitida

