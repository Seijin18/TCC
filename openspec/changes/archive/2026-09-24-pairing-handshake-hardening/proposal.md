## Why

A validação de bancada de 2026-09-24 ficou bloqueada porque mestre e
escravo **param de conseguir se parear depois da primeira perda de enlace**.
O diagnóstico registrado naquele dia ("ruído de RF ambiente intermitente
atingindo um handshake frágil") **não se sustenta** diante de uma análise
mais cuidadosa dos logs (`logs/session-20260924-124606.jsonl`,
`logs/session-20260924-113006.jsonl` e mais 6 sessões anteriores — detalhes
em [design.md](design.md) "Evidências"):

- O pareamento **no boot** funciona em praticamente 100% das sessões, em
  ~0,3 s. O re-pareamento depois de um `loss_of_sync` às vezes funciona em
  segundos (2 de 5 casos) e às vezes **nunca mais** (3 de 5 casos — um deles
  por 50 min, outro por 45 min, com ~1.300 acampamentos do mestre e ~6.900
  janelas de varredura do escravo, **zero** `pair_ok`). Ruído ambiente com
  ~2% de ocupação média da banda não produz 0 sucessos em milhares de
  tentativas.
- Nos primeiros 10 s depois da falha, mestre e escravo estiveram **no mesmo
  candidato ao mesmo tempo** pelo menos 3 vezes (ch 90 duas vezes, ch 96 uma
  vez) — com a mesma configuração que pareou em 0,3 s no boot — e o escravo
  não recebeu **nenhum** pacote.
- O mestre chegou a aceitar `MSG_LINK_ACK` em ch 76 **enquanto o escravo
  estava escutando em outro canal** (e sem o escravo registrar ter enviado
  ack nenhum) — ou seja, o mestre aceita pacotes que não pertencem à
  tentativa corrente.
- Um handshake de boot ficou **meio-aberto**: o escravo recebeu a seed e se
  considerou pareado, o `MSG_SEED_ACK` se perdeu, o mestre desistiu, e o
  escravo passou 6,4 s descartando as novas sondas do mestre como "payload
  inesperado" até cair por `loss_of_sync`.

Ou seja: o problema é **do protocolo/estado do rádio**, não do ambiente.
Enquanto isso não for resolvido, nada que dependa de um enlace estável pode
ser validado em bancada: jammer `adaptive` (portão de handshake),
`run_scenario.py`, `run_battery.py` (A/B/C), e os itens pendentes de
`blacklist-decay` §4, `dynamic-seed-blacklist` 4.4/4.5,
`spectrum-attack-classification` 3.1/3.3, `adaptive-rendezvous` 6.2-6.5,
`two-board-topology` 6.2-6.5 e `targeted-jammer` 3.2/4.5.

## What Changes

Em quatro etapas, cada uma com critério de saída mensurável, na ordem:

1. **Instrumentação (sem mudança de comportamento)**: dump dos
   registradores do NRF24 (`e=regs`) no boot, em toda entrada no estado de
   pareamento e sob comando; contadores de pareamento por janela
   (`e=pair_stats`: sondas enviadas/falhas, pacotes recebidos por tipo e
   tamanho, acks enviados); falhas de `radio_send()` deixam de ser
   silenciosas. Um script `tools/validation/repair_trials.py` força N perdas
   de enlace e mede o tempo de re-pareamento — **número-base antes de
   qualquer correção**.
2. **Higiene do rádio**: ao entrar em pareamento (boot, `loss_of_sync`,
   `pair_timeout`, timeout de seed) cada lado reinicializa o rádio para um
   estado conhecido (mesma sequência do `setup()`), e faz `flush_rx()` a
   cada troca de canal de pareamento — `RF24::startListening()` (v1.6.2)
   **não** esvazia a FIFO de recepção, então pacotes antigos atravessam
   trocas de canal e de estado.
3. **Handshake v2 (identificado por sessão e idempotente)**: a sonda passa a
   carregar `{versão, nonce de sessão, seed}` — seed embutida na sonda, o
   handshake cai de 4 mensagens para 2. O escravo responde
   `ACK{nonce, seed}` e **continua respondendo** (idempotente) a sondas com
   o mesmo nonce enquanto o enlace não for confirmado por um fragmento de
   dado real; o mestre só aceita ack com o nonce da tentativa corrente.
   Elimina o estado meio-aberto e os acks "fantasmas"/antigos.
4. **Temporização do encontro e semântica da blacklist de candidatos**:
   garantir (com `static_assert`) que cada acampamento do mestre cubra pelo
   menos 2 ciclos completos de varredura do escravo e que cada janela do
   escravo veja pelo menos 2 sondas; e a blacklist de candidatos passa a
   contar falha só com evidência de canal ruim (handshake iniciado e não
   concluído), não por "ninguém respondeu" — hoje ela culpa os canais pela
   ausência do escravo (ciclo `0xE1→0xE2→0xE4→0xE8→0xF0` observado).

Mais validação em bancada (re-pareamento forçado 20×, handshake com perda
injetada, jammer antes do boot) e, só então, a retomada da fila de
validação bloqueada.

Também corrige o procedimento de bancada: os scripts de validação passam a
enviar `jam off` no início (um `jam on` + `mode adaptive` de um teste
anterior ficou armado por 45 min, contaminando a observação — neste caso
sem efeito real porque o jammer esperava o portão de handshake, mas é uma
armadilha).

## Capabilities

### New Capabilities
(nenhuma)

### Modified Capabilities
- `clean-channel-rendezvous` (de `adaptive-rendezvous`, ainda não
  arquivado): adiciona requisitos de estado de rádio limpo na entrada do
  pareamento, handshake identificado por sessão e idempotente, cobertura
  temporal garantida do encontro, e evidência mínima para excluir um
  candidato. Não remove requisito nenhum.

## Impact

- `shared/protocol.h`: `LinkProbeV2`/`LinkAckV2` (substituem
  `LinkPacket`/`SeedPacket`/`SeedAckPacket` no pareamento), constantes de
  temporização revisadas + `static_assert`s de invariantes.
- `shared/pairing.h` (novo, puro, sem Arduino): decisão "aceitar/ignorar/
  re-ackar" por nonce/estado, testável em `env:native_test`.
- `mestre/src/node.h`, `escravo/src/node.h`: `radio_reinit()`, flush em
  troca de canal, máquina de estados do handshake v2, telemetria nova.
- **Mestre e escravo precisam ser gravados juntos** (formato do pacote de
  pareamento muda; sonda com versão diferente é ignorada e logada).
- `tools/logger/main.py`: campos `regs`/`pair_stats` no `state.json`.
- `tools/validation/repair_trials.py` (novo); `run_scenario.py` /
  `run_battery.py` ganham `jam off` inicial.
- Depende de `adaptive-rendezvous` (capability `clean-channel-rendezvous`)
  e de `telemetry-logging`; não depende do analisador.
