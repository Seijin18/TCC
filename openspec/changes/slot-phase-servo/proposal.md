## Why

Soak de 13 min sem jammer em 2026-10-21 (`logs/session-20261021-054356.jsonl`)
mostrou que o enlace **apodrece com o tempo desde o pareamento**:

- throughput medido pelo mestre caiu de ~105 para ~31 fragmentos/s em
  ~7,5 min (`throughput.window_ms` de 3300 para 10470 ms por 350 tentativas);
- o PLR do escravo ficou em 3-5% e a cadência de slots do escravo ficou
  constante (10500 ms por janela de 350 slots) — o enlace não "perde
  pacotes", perde a **folga dentro de cada slot**: de ~3 fragmentos por slot
  para ~1;
- em ~7,5 min houve `loss_of_sync`, re-pareamento automático, throughput
  cheio de novo e o decaimento recomeçou (reproduzido duas vezes no mesmo
  soak);
- não é artefato do logger (atraso logger-vs-dispositivo estável em ±0,1 s),
  nem hardware (registradores dos 4 rádios normais, 58/58 testes nativos),
  nem back-pressure da serial (linhas de `ack` caem junto com `msg_done`).

Causa: os cristais dos dois ESP32 andam em ritmos levemente diferentes
(dezenas de ppm). O salto é por tempo (`slot = (agora − âncora)/SLOT_MS`),
então a fase de slot do escravo desliza em relação à do mestre — ~2,4 ms
por minuto a 40 ppm, mais de meio slot de 30 ms em ~7 min. As janelas de
transmissão do mestre e de escuta do escravo só se sobrepõem em parte de
cada slot, e essa parte encolhe.

`escravo_reanchor()` só corrige quando o número do slot do pacote difere do
local, e na prática nem isso detecta: um pacote de outro slot está em outro
canal e não é recebido. A fase sub-slot nunca é rastreada.

Consequência para a avaliação: toda medição com mais de ~2 min sem
re-pareamento mistura o efeito testado com esse decaimento — inclusive o
"hold → 51% de perda" de 2026-09-26.

## What Changes

- `FragmentPacket` ganha `uint8_t slot_phase_ms`: a posição do mestre
  dentro do slot corrente no instante do envio (30 → 31 bytes, dentro do
  limite de 32).
- O escravo mede, a cada fragmento válido, o desvio de fase
  `e = fase_escravo − fase_mestre`, onde
  `fase_escravo = agora − (hop_start_at + master_slot·SLOT_MS)`, filtra
  (média móvel exponencial) e corrige `hop_start_at` em passos de 1 ms
  quando o desvio filtrado passa de 1 ms — servo de fase de 1ª ordem, com
  limite de passo.
- A latência fixa (ar + processamento) é absorvida usando como alvo o
  desvio medido logo após `time_sync`.
- Lógica pura em `shared/time_sync.h` (novo), coberta em `env:native_test`.
- Telemetria periódica `@escravo;e=phase;err=..;adj=..;n=..` (~5 s): a
  deriva passa a ser visível nos logs, hoje só dedutível indiretamente.
- `escravo_reanchor()` (slot inteiro) permanece como rede de segurança.

## Capabilities

### New Capabilities
- `slot-phase-tracking`: o escravo mantém a fase do seu relógio de slots
  alinhada à do mestre continuamente, a partir da fase informada pelo
  mestre em cada fragmento.

### Modified Capabilities
(nenhuma — `time-slotted-hopping` ainda não foi arquivado; esta change
depende dele e deve ser arquivada depois dele ou junto)

## Impact

- `shared/protocol.h`: campo novo em `FragmentPacket` + `static_assert` de
  tamanho; constantes do servo.
- `shared/time_sync.h` (novo): atualização pura do servo.
- `mestre/src/node.h`: preencher `slot_phase_ms` no envio.
- `escravo/src/node.h`: medir, filtrar, corrigir `hop_start_at`, telemetria.
- `test/test_time_sync/` (novo).
- Torna confiáveis medições longas (soaks, baterias sem re-pareamento entre
  cenários) e invalida a necessidade de re-parear antes de cada run.
