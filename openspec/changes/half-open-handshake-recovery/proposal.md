## Why

Na bateria de 2026-10-21 (`logs/battery-20261021-091229.json`, jammer `hold`
no ch10 a `PA_MAX`, sessão `logs/session-20261021-054356.jsonl`) o enlace
ficou preso no pareamento duas vezes, sem se recuperar sozinho:

- **Cenário A**: depois da 2ª queda, 31,5 s sem nenhum tráfego, ciclando
  `pair_ok` → `pair_timeout` → `loss_of_sync`, até o `reset` do cenário
  seguinte (inclusive depois de `jam off`).
- **Cenário C**: nunca pareou (120 s). Como o jammer só arma depois do
  primeiro handshake, o cenário C rodou **sem enlace e sem ataque** — a
  bateria registrou "dados" de um cenário que não aconteceu.

Nos dois casos o handshake ficou **meio-aberto**: o escravo recebia cada
sonda do mestre (nonce novo a cada acampamento de 4,5 s, `pair_reack` a cada
100 ms = cadência das sondas), e o mestre não recebia nada (`pair_stats
rx_total=0`, nem pacote espúrio). Mestre e escravo estavam no mesmo canal
(no cenário A o `rf_ch` dos dumps do escravo bate com o rótulo do
`pair_ok`: 76 → `0x4C`, 90 → `0x5A`).

O que já se sabe sobre a causa:

- No cenário A o escravo reinicializou o próprio rádio **6 vezes** (a cada
  `pair_timeout`) e o estado persistiu. Todas as recuperações coincidiram
  com uma reinicialização nova do rádio do **mestre** (`reset` A→B
  recuperou; `reset` no início do C não recuperou; `reset` manual depois
  recuperou). Hipótese principal: o receptor do mestre fica surdo depois de
  `radio_reinit()`, de forma intermitente, com os registradores lendo
  normal — mesma classe de "rádio surdo" dos clones SI24R1 já vista neste
  projeto (ver comentário de `enableAckPayload()` em `radio_reinit()`).
- `radio_send()` usa `writeFast(..., multicast=true)` (sem ack de hardware):
  `probes_ok`/`sent=true` só significam que a FIFO de TX esvaziou, não que
  o outro lado recebeu. Por isso a telemetria atual não distingue "escravo
  ausente" de "escravo transmitindo e não decodificado".

Uma explicação anterior ("conjuntos de candidatos disjuntos") foi
descartada: o escravo recebia as sondas do mestre em ~0,1 s depois de cada
novo acampamento, logo estavam no mesmo canal.

## Verificação 2026-10-21 (CORRIGE a causa; a correção continua válida)

Bateria diagnóstica (`diag_battery.py t1-run`, 20 ciclos: 10 re-pareamentos
simples + 10 re-pareamentos após perda forçada com **mitigação desligada**):
**0 ocorrências do estado preso** — todo re-pareamento subiu em 3,3–3,7 s.
Ou seja, o bug **não** é do re-pareamento comum nem da perda sob jamming com
mitigação desligada.

A ocorrência real (pré-existente na bancada, presa ~2,8 h) foi capturada com
a gênese (`session-20261021-062819`): **mitigação LIGADA** → o mestre agenda
um rekey de blacklist (`mask=0xFFDB`) → **`rekey_failed` 3 vezes seguidas**
(cada uma reagenda `apply`) → `loss_of_sync` nos dois lados → aí instala o
pareamento meio-aberto.

Isolamento (o ponto que **refuta** minha hipótese anterior de "receptor do
mestre surdo"): **resetar SÓ o escravo (`placa_b reset`) recuperou o enlace
em 3,2 s** (passo c); não precisou resetar o mestre. Logo o lado preso é o
**escravo** — um estado de máquina ruim (fica em `pair_ok`/re-ack sem os
dados confirmarem), não o receptor do mestre. Correções à proposta:

1. **Gatilho** = caminho de controle (rekey/blacklist) falhando sob
   mitigação, provavelmente porque o pacote de controle (enviado no lugar
   do fragmento, precisa de ack dentro do slot) falha quando a
   sobreposição de slot já está encolhida pela deriva (#2). Então **#2 é
   provável contribuinte de #1** — corrigir a deriva deve reduzir a
   frequência deste bug.
2. **Lado que trava** = escravo. O watchdog do escravo (abaixo) é a correção
   principal e está **validado** pelo resultado do reset isolado; o watchdog
   do mestre vira rede de segurança secundária.
3. A tarefa 1 (confirmar o lado surdo) está **resolvida** pelo isolamento;
   substituída por "reproduzir com mitigação LIGADA + falha de rekey e
   medir o tempo de auto-recuperação do watchdog do escravo".

## What Changes

**Escopo enxuto (decisão do autor, 2026-10-21):** como o #2 eliminou o
gatilho e o travamento não reproduz mais, só a rede de segurança é
implementada; o diagnóstico de lado surdo, a instrumentação de RPD e o
watchdog do mestre são descartados.

- **Watchdog de auto-recuperação do escravo**: o escravo conta sessões de
  pareamento provisório seguidas (nonces distintos) que nunca confirmam um
  `FragmentPacket`. Ao atingir `HALF_OPEN_SESSIONS`, emite `e=half_open`,
  faz `radio_reinit()` completo e volta a varrer **a partir de um índice de
  candidato / fase de dwell aleatorizados** — imitando o `reset` manual do
  escravo (único que recuperava o estado preso real) e quebrando qualquer
  ressonância de fase com a rotação de acampamento do mestre. O contador
  zera ao confirmar o enlace (CONFIRMED).
- A decisão "deve recuperar?" vira função pura em `shared/pairing.h`, testada
  em `env:native_test`.

## Capabilities

### New Capabilities
(nenhuma)

### Modified Capabilities
- `clean-channel-rendezvous`: requisitos novos de detecção/recuperação de
  handshake meio-aberto e de observabilidade da saúde do receptor durante o
  pareamento.

## Impact

- `escravo/src/node.h`: contador de sessões provisórias sem confirmação;
  gatilho de recuperação forçada (reinit + rescan aleatorizado) ao atingir o
  limiar; zera em CONFIRMED e no `reset` por comando.
- `shared/pairing.h`: decisão pura `half_open_should_recover(...)`, testada
  em `env:native_test`.
- `shared/protocol.h`: constante `HALF_OPEN_SESSIONS`.
- `tools/logger/main.py`: guardar `half_open` em `state.json`.
- Rede de segurança: o cenário C que havia sido anulado por este bug em
  2026-10-21 já volta a funcionar com o #2; este watchdog garante a
  auto-recuperação caso o estado meio-aberto ocorra por outro caminho.
