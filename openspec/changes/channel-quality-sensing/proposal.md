## Why

O analisador hoje tenta **classificar** "ataque vs. ruído" (baseline
adaptativo por canal + votos + streak de ciclos, ver
`spectrum-attack-classification`). Em 2026-10-21 isso falhou nos dois
sentidos:

- **Não alerta com o jammer**: o check de acoplamento mostrou o analisador
  vendo o jammer perfeitamente (ocupação 100% no canal travado, ~200× a
  base, bloco de ~5 canais), e o `chmap` chegou a marcar o ch10 sob `hold`
  — mas `alert_on` nunca disparou: com um jammer de um só canal a contagem
  de canais comprometidos oscila entre 0 e 2 e o streak de 2 ciclos
  seguidos não fecha.
- **"Falso positivo" do Wi-Fi**: o Wi-Fi real da bancada (ch ~26-45) degrada
  o ch34 (miss do enlace entre 7% e 68% conforme a sessão) e era tratado
  como algo a suprimir.

Do ponto de vista do enlace, um canal tomado por jammer e um canal tomado
por Wi-Fi são igualmente ruins, e a resposta é a mesma: deixar de usar o
canal. Proposta do autor (2026-10-21): usar o analisador para avaliar cada
canal individualmente e sinalizar por limiar, **independentemente da
causa**, já que jamming e congestionamento comprometem a comunicação do
mesmo jeito.

## Verificação 2026-10-21 (CONFIRMA a direção; CORRIGE a causa do não-alerta)

Bateria diagnóstica (`diag_battery.py t3-offline`/`t3-run`):

- **Causa real do "não alerta" (corrige o parágrafo acima)**: o alerta
  **não** fica sem fechar o streak. Ele **travou LIGADO antes da bateria**
  (último `alert_on` às 09:12:28 com `mask=0x0004` = ch34/Wi-Fi, sem nenhum
  `alert_off` depois) e ficou ligado a sessão inteira. Como o alerta é por
  **borda** e é um **booleano global**, um ch34 de Wi-Fi já o mantém ligado
  e nenhuma borda nova aparece quando o jammer chega — exatamente o que a
  mudança para qualidade **por canal** resolve. `chmap count ≥ 1` em todos
  os ciclos de todas as fases.
- **Confirma a direção (ocupação prediz perda, agnóstico à causa)**: com
  portadora travada no ch70 a atividade do analisador saltou de 0,45 para
  **97,0** e o miss do mestre no ch70 para **78%**; correlação
  ocupação-vs-miss nos 16 canais = **0,93** na fase com jammer, **0,70–0,75**
  nas fases só com Wi-Fi (dominada por ch34/ch46). Ou seja, um limiar de
  ocupação separa canal bom de ruim tanto para jammer quanto para Wi-Fi.
- **A blacklist reativa do mestre já funciona**: na fase `hold ch70` o mestre
  baniu o ch70 sozinho (`mask=0xFFDB`) pela própria evidência de ack. Confirma
  o papel do analisador como fonte **proativa** fundida, não substituta.

## What Changes

- **Analisador vira medidor de qualidade por canal**: para cada canal da
  tabela de hopping, ocupação = fração das varreduras recentes (janela `W`)
  com portadora detectada. Canal "degradado" quando a ocupação ≥ `T_on`;
  volta a "utilizável" quando ≤ `T_off` (histerese). Sem classificar causa.
- **Entrega uma máscara, não um booleano**: na placa combinada o analisador
  passa ao mestre a máscara de 16 bits de canais degradados (callback na
  mesma placa). Telemetria `@analisador;e=chq;mask=..;top=<canal:ocupação>`.
  Na topologia de 4 placas (fio de GPIO) continua só o booleano "há canal
  degradado".
- **Mestre funde duas fontes na mesma blacklist**: (a) a evidência reativa
  que já existe (falhas de ack por canal) e (b) a evidência proativa do
  analisador (máscara degradada). Mesmas guardas de hoje: nunca menos de 4
  canais ativos, histerese, cooldown de reinclusão.
- **Reseed só quando a blacklist não basta**: troca de seed deixa de ser
  disparada por "alerta de ataque" genérico e passa a valer quando muitos
  canais estão degradados ao mesmo tempo ou o miss continua alto depois de
  aplicada a blacklist (política em `shared/control_policy.h`).
- **Calibração guiada por dado**: `T_on`/`T_off`/`W` escolhidos cruzando a
  ocupação por canal (`spectrum.csv`) com o miss do mestre por canal nas
  sessões existentes + uma coleta dedicada (jammer `hold` em canais
  conhecidos, Wi-Fi ambiente).
- **Substitui** as tarefas pendentes 3.1 e 3.3 de
  `spectrum-attack-classification` (calibração ataque-vs-ruído), que passam
  a ser fechadas como superadas.

## Capabilities

### New Capabilities
- `channel-quality-sensing`: avaliação de qualidade por canal pelo
  analisador, agnóstica à causa, e fusão com a evidência do próprio enlace
  na blacklist do mestre.

### Modified Capabilities
(nenhuma — `spectrum-attack-classification` e `dynamic-seed-blacklist`
ainda não foram arquivados; esta change registra quais tarefas deles
substitui)

## Impact

- `analisador/src/node.h`: ocupação por canal + histerese; caminho de alerta
  passa a carregar máscara; o classificador CFAR/votos/streak sai do
  caminho de decisão (pode ficar só como telemetria até ser removido).
- `mestre/src/node.h`: recebe a máscara do analisador e funde com
  `g_channel_fail`; política de reseed.
- `shared/` (novo `channel_quality.h` ou extensão de `hopping.h`): ocupação,
  histerese e fusão como lógica pura + testes `native_test`.
- `shared/control_policy.h`: gatilho de reseed.
- `tools/validation/`: script de calibração.
