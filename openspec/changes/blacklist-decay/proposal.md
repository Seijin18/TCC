## Why

`dynamic-seed-blacklist` (seções 1-3 já implementadas) blacklista canais
com falha severa e persistente, mas nunca os remove — é um Non-Goal
explícito daquele change ("não faz o mestre remover canais da blacklist
automaticamente quando eles 'melhoram'... pode virar um change futuro se
a banca pedir",
[design.md](../dynamic-seed-blacklist/design.md)). Isso é aceitável para
demonstrar mitigação pontual, mas quebra em qualquer sessão mais longa ou
com um jammer que varre/muda de alvo: canais marcados ruins durante um
pico de ataque ficam permanentemente fora de uso mesmo depois do jammer
sair dali, encolhendo o espectro disponível de forma monotônica até o
piso de segurança de "nunca esvaziar todos os 16 canais"
(`compute_candidate_mask`, `mestre/src/node.h:417-428`) — o sistema nunca
recupera a banda cheia sozinho, mesmo com o ataque há muito tempo
encerrado.

Há também uma lacuna estrutural que agrava isso: hoje
`maybe_enqueue_control()` só reavalia a máscara de blacklist enquanto
`g_alert_active` está ligado (`mestre/src/node.h:446`). Se o alerta cair
(ataque percebido como encerrado pelo analisador) antes de qualquer
avaliação de recuperação acontecer, a máscara fica congelada no estado em
que o alerta terminou — para sempre, até o próximo ataque. Fechar esse
non-goal exige desacoplar "reavaliar a máscara" de "há um alerta ativo
agora".

## What Changes

- `compute_candidate_mask()` passa a considerar dois sentidos: excluir
  canais com falha severa e persistente (comportamento já existente) e
  **reincluir** canais antes blacklisted cujo histórico recente mostra
  recuperação sustentada — não apenas ausência momentânea de falha, para
  evitar flapping (voltar e sair da blacklist repetidamente).
- Reavaliação da máscara deixa de depender de `g_alert_active`: passa a
  rodar continuamente (a cada N hops, não a cada hop, para não competir
  demais com o tráfego de dados) independente do alerta estar ativo ou
  não — o alerta continua sendo o único gatilho de troca de **seed**
  (`MUDAR_SEED`), que é uma ação mais disruptiva e deve continuar restrita
  a um ataque detectado; blacklist vira uma correção contínua de baixo
  custo, como o retry por hop já é.
- Introduz um critério de "cooldown de recuperação" simétrico ao
  `CHANNEL_FAIL_THRESHOLD` existente — um canal blacklisted só volta a ser
  candidato a reinclusão depois de N hops (ou um intervalo mínimo de
  tempo) sem nenhuma nova falha computada nele, mesmo sem tráfego de dados
  ali (que não recebe nenhum fragmento, então não gera nem falha nem
  sucesso — precisa de uma forma de "testar" o canal periodicamente para
  ter evidência de que ele de fato recuperou, não só que ficou quieto).
- Reaproveita o pacote de controle já existente (`BLACKLIST_UPDATE` /
  `MSG_CTRL_BLACKLIST`, `ControlAck`) e o mesmo mecanismo de
  confirmação-antes-de-aplicar — nenhum tipo de pacote novo.
- Telemetria: a linha `blacklist` já emitida ganha contexto suficiente
  para distinguir uma atualização que *adiciona* canais de uma que
  *remove* (ex.: máscara antes/depois, ou um campo `delta`), para o log
  ficar auditável nos dois sentidos.

## Capabilities

### New Capabilities
(nenhuma)

### Modified Capabilities
- `adaptive-mitigation` (de `dynamic-seed-blacklist`): adiciona
  requisitos de recuperação automática de canais blacklisted (critério de
  elegibilidade, cooldown anti-flapping, e reavaliação de máscara
  desacoplada de `g_alert_active`). Não altera nenhum requisito existente
  de blacklisting/troca de seed já aprovado — só estende o
  comportamento.

## Impact

- `mestre/src/node.h`: `compute_candidate_mask()` ganha lógica de
  reinclusão; `maybe_enqueue_control()` perde a guarda `g_alert_active`
  no ramo de blacklist (mantém a guarda no ramo de reseed); precisa de
  algum estado adicional por canal para o cooldown de recuperação (ex.:
  contador de hops-desde-última-falha, ou reaproveitar/estender
  `g_channel_fail[]`).
- `escravo/src/node.h`: nenhuma mudança esperada — já aplica qualquer
  máscara recebida via `BLACKLIST_UPDATE` de forma agnóstica a
  crescer/encolher.
- `shared/protocol.h`: nenhum tipo de pacote novo.
- Telemetria (`tlm_printf`/evento `blacklist`): formato da linha ganha
  contexto de adição vs. remoção; consumidores existentes
  (`tools/logger`, `tools/validation/plot_scenario.py`) precisam
  continuar parseando o formato atual ou ser atualizados junto.
- Depende de `dynamic-seed-blacklist` (ainda não arquivado — este change
  estende a capability `adaptive-mitigation` antes dela existir na
  baseline de `openspec/specs/`; documentar essa dependência explicitamente
  até `dynamic-seed-blacklist` ser arquivado).
- Não depende da calibração pendente do analisador
  (`spectrum-attack-classification` tasks 3.1-3.3, ver
  `openspec/ROADMAP.md`) — como a reavaliação de blacklist deixa de exigir
  `g_alert_active`, este mecanismo pode ser validado em bancada usando só
  a perda de fragmentos causada pelo jammer, sem depender do alerta do
  analisador disparar.
