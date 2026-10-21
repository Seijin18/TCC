# Design — recuperação de handshake meio-aberto

## Evidência (bateria 2026-10-21)

| Momento | O que aconteceu | Recuperou? |
|---|---|---|
| A, queda em +37 s → reinit do mestre | re-pareou em 8,6 s | sim |
| A, queda em +77 s → reinit do mestre | mestre em `pair_scan`, escravo `pair_ok` → `pair_timeout` a cada ~4 s; escravo reinicializou 6x | **não** (31,5 s) |
| `reset` dos dois (A→B) | B rodou limpo | sim |
| `reset` dos dois (início do C) | escravo re-ack a cada 100 ms, mestre `rx_total=0` | **não** (120 s) |
| `reset` manual dos dois | `confirmed:true`, PLR 0,3% | sim |

Em todo estado preso: o mestre transmite (o escravo ouve cada sonda nova
em ~0,1 s) e não recebe nada (`rx_total=0`, `rx_other=0`, `flushed=0`).

## Decisões

### D1 — Confirmar antes de corrigir a causa
A hipótese "receptor do mestre surdo após reinit" é forte mas não provada.
A tarefa 1 reproduz o estado e faz duas medidas independentes:
1. **Analisador como observador independente** (está na placa A, ao lado do
   mestre): no estado preso, o `spectrum.csv` do canal de acampamento deve
   mostrar a energia do ack do escravo (~1 pacote a cada 100 ms). Se houver
   energia → o escravo transmite e o mestre não decodifica (mestre surdo).
   Se não houver → o lado com defeito é o TX do escravo.
2. **Isolamento por reinit**: no estado preso, `placa_a reset` (reinicializa
   só o mestre) vs. `placa_b reset` (só o escravo). O que destravar aponta
   o lado.

### D2 — Recuperação independente da causa
Mesmo sem saber o mecanismo interno do clone, reinicializar o rádio do lado
surdo resolve na prática (evidência acima). Os dois lados ganham um
watchdog, porque o defeito pode estar em qualquer um:

- **Mestre**: `HALF_OPEN_SILENT_CAMPS = 2` acampamentos seguidos (~9 s) com
  `rx_total == 0` → `radio_reinit()` + `e=regs;why=half_open`. Backoff
  exponencial (9 s → 18 s → … até 60 s) enquanto nenhum pacote chegar, para
  não reinicializar a cada 9 s quando o escravo está de fato desligado.
  Primeiro pacote recebido zera o backoff.
- **Escravo**: `HALF_OPEN_SESSIONS = 2` sessões seguidas (nonces distintos,
  ~9 s) respondidas sem nenhum `FragmentPacket` → `e=half_open`,
  `radio_reinit()`, volta a `ESCRAVO_SCANNING`. Hoje, enquanto sondas
  continuam chegando, o escravo re-arma `hop_start_at` a cada sonda e nunca
  atinge `SEED_WAIT_TIMEOUT_MS` — fica preso indefinidamente (cenário C).

Alvo: enlace de volta em ≤ 15 s depois de um meio-aberto, sem operador.

### D3 — Indicador de portadora no pareamento
`testCarrier()` (RPD > −64 dBm) amostrado durante `LINK_ACK_WAIT_MS` depois
de cada sonda. `pair_stats` ganha `rx_rpd` = número de janelas com portadora.
`rx_rpd > 0 && rx_total == 0` é a assinatura de "transmitindo e não
decodificado". RPD também acende com Wi-Fi/jammer, por isso é só
diagnóstico e acelerador opcional — o watchdog de silêncio (D2) é a rede de
segurança.

### D4 — Reinicialização verificada
Depois de `radio_reinit()`, readback de `CONFIG`, `EN_RXADDR`,
`RX_ADDR_P1`, `RF_CH`, `FEATURE`, `DYNPD`, `RF_SETUP` contra os valores
esperados. Divergência → reinicialização completa (sequência de boot com
`begin()`). Se o readback for normal e o rádio continuar surdo (hipótese
atual), o watchdog D2 cobre. Conferir também que `radio_reinit()` do mestre
esvazia a FIFO de RX (requisito "Estado de rádio conhecido"; hoje o esvazia
em `pick_new_pair_channel()`, não na própria reinit).

### D5 — Lógica pura testável
A decisão "meio-aberto?" (contadores → ação, com backoff) vai para
`shared/pairing.h` como função pura e é coberta em `env:native_test`, como
`pair_decide_escravo()` já é.

## Riscos

- **Escravo desligado**: o watchdog do mestre reinicializa com backoff até
  60 s — custo ~5 ms por reinit e uma linha de log. Aceitável.
- **Falso positivo do escravo**: um mestre lento para começar o salto poderia
  parecer meio-aberto. `HALF_OPEN_SESSIONS` conta **sessões** (nonces
  distintos), e o mestre só troca de nonce quando desiste do acampamento —
  ou seja, só quando ele próprio não recebeu o ack. Não dispara num
  pareamento normal.
- **Causa não é o receptor do mestre**: D1 decide; D2 cobre os dois lados
  de qualquer forma.
- **Penalização de candidato (D5 de `pairing-handshake-hardening`)**: no
  cenário A o mestre quarentenou os candidatos 76 e 90 (`pair_mask`
  0xFF → 0xFC) por "ack recebido, dado não confirmado", quando a falha era
  do canal de dados jammed / do rádio surdo, não do candidato. Fora do
  escopo desta change; registrar e reavaliar depois do D1.
