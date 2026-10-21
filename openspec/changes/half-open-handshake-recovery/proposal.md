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

- **Confirmar o lado surdo antes da correção definitiva**: reproduzir o
  estado preso e, nele, (a) usar o analisador (mesma placa do mestre) para
  ver se há energia do ack do escravo no canal de acampamento a cada
  ~100 ms; (b) reinicializar só o mestre vs. só o escravo e registrar qual
  resolve.
- **Telemetria de saúde do receptor no pareamento**: o mestre passa a medir
  portadora (`testCarrier()`/RPD) dentro da janela de espera do ack e
  publica em `pair_stats` (`rx_rpd=` — janelas com portadora detectada).
  Distingue "ninguém transmitindo" de "alguém transmitindo e nada
  decodificado".
- **Detecção e recuperação de handshake meio-aberto, nos dois lados,
  independente da causa exata**:
  - mestre: `HALF_OPEN_SILENT_CAMPS` acampamentos seguidos sem nenhum pacote
    recebido → `radio_reinit()` + dump `e=regs;why=half_open`, com backoff
    para não reinicializar sem parar quando o escravo está de fato
    desligado;
  - escravo: `HALF_OPEN_SESSIONS` sessões de pareamento seguidas (nonces
    distintos) respondidas sem nenhum `FragmentPacket` → evento
    `half_open`, `radio_reinit()` e volta a varrer.
- **Reinicialização verificada**: depois de `radio_reinit()`, ler de volta
  os registradores críticos (`CONFIG`, `EN_RXADDR`, `RX_ADDR_P1`, `RF_CH`,
  `FEATURE`, `DYNPD`, `RF_SETUP`) e comparar com o esperado; divergência →
  reinicialização completa (`begin()` + configuração de boot). Conferir que
  a FIFO de RX é esvaziada na reinicialização do mestre (requisito existente
  "Estado de rádio conhecido ao entrar em pareamento").

## Capabilities

### New Capabilities
(nenhuma)

### Modified Capabilities
- `clean-channel-rendezvous`: requisitos novos de detecção/recuperação de
  handshake meio-aberto e de observabilidade da saúde do receptor durante o
  pareamento.

## Impact

- `mestre/src/node.h`: `try_establish_link()` (RPD na janela de ack),
  acampamento (contador de acampamentos silenciosos), `radio_reinit()`
  (readback + fallback).
- `escravo/src/node.h`: caminho `ESCRAVO_PROVISIONAL` (contador de sessões
  sem dado), `radio_reinit()` (readback).
- `shared/pairing.h`: decisão pura "meio-aberto?" a partir dos contadores,
  testada em `env:native_test`.
- `shared/protocol.h`: constantes `HALF_OPEN_SILENT_CAMPS`,
  `HALF_OPEN_SESSIONS`, backoff.
- `tools/logger/main.py`: guardar `half_open`/`rx_rpd` em `state.json`.
- Destrava: qualquer bateria A/B/C confiável (o cenário C de 2026-10-21 foi
  anulado por este bug).
