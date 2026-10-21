# Design — salto de frequência por tempo (time-slotted), com âncora do pareamento

## Princípio

`slot = (millis() − hop_start_at) / SLOT_MS` nos DOIS lados; `channel =
get_next_active_channel(seed, slot, mask)` (mesma função de hoje, `hop_counter`
:= `slot`). Como o canal é função só do tempo e da seed/máscara (compartilhados),
os dois estão no mesmo canal por construção. O relógio de referência é o do
mestre; o escravo mede um offset no pareamento e o corrige pelos pacotes.

## O que JÁ existe e é reaproveitado

- **Âncora comum**: no pareamento os dois fazem `hop_start_at = millis() +
  HOP_START_DELAY_MS` (mestre em `mestre/src/node.h:1178`, escravo em
  `escravo/src/node.h:825/912`), a ~1-2 ms um do outro (um trânsito). É o `T0`.
- **`hop_counter` no pacote**: `FragmentPacket.hop_counter` e os
  `Control*Packet.hop_counter` já carregam o slot do mestre; o escravo já os lê.
- **Função de canal / seed / máscara**: `get_next_active_channel()` em
  `shared/hopping.h`, `g_seed`, `g_channel_mask` — sem mudança.
- **Dedup (SEQN)**: `received_bitmap` no escravo — sem mudança.
- **Split de contadores** (hop-ack-decoupling): `fragment_index` avança só no
  ack — sem mudança.

## Mudança concreta

### 1. Mestre — laço por tempo (`mestre/src/node.h`, ~1190-1372)
```
if (millis() < g_hop_start_at) return;            // ainda no delay pós-pareamento
uint32_t slot = (millis() - g_hop_start_at) / SLOT_MS;
if (slot != g_hop_counter) {                       // fronteira de slot: hop
    g_hop_counter = slot;
    maybe_enqueue_control();                        // igual hoje
    channel = get_next_active_channel(g_seed, slot, g_channel_mask);
    setChannel(channel); delayMicroseconds(500); flush_rx;
    // atribuição/telemetria de miss do slot ANTERIOR, se não teve ack
}
// dentro do slot: envia controle pendente OU o fragmento corrente uma vez,
// espera ack curto; no ack -> fragment_index++ (e próximo fragmento pode ir
// no mesmo slot, pipeline); guarda no fim do slot (não enviar nos últimos
// SLOT_GUARD_MS pra não cruzar a fronteira).
```
- `g_hop_counter` NÃO avança em ack/miss — só quando o tempo cruza a fronteira.
- Miss = slot sem ack; a atribuição de canal (attribution gate, suspect_idx,
  blacklist) segue igual, avaliada na fronteira do slot.
- Perda: `g_slots_since_ack` (contado em slots) ou tempo sem ack ≥ `LINK_LOSS_MS`
  → `reset_to_pairing()`.

### 2. Escravo — laço por tempo + reancoragem (`escravo/src/node.h`, ~966-1347)
```
if (millis() < hop_start_at) { ... provisório ... }
uint32_t slot = (millis() - hop_start_at) / SLOT_MS;
if (slot != local_slot) {                           // fronteira: hop
    local_slot = slot; hop_counter = slot;
    channel = get_next_active_channel(g_seed, slot, g_channel_mask);
    setChannel(channel); delayMicroseconds(500); flush_rx;
}
// escuta o canal do slot; no RX válido:
//   - reancora: se |slot − pkt.hop_counter| >= 1, hop_start_at corrige pra o
//     slot do mestre (drift slip) -> hop_start_at = t_rx - pkt.hop_counter*SLOT_MS
//   - dedup received_bitmap, acka (com suspect_idx), atualiza qualidade/PLR
// perda: tempo sem RX válido >= LINK_LOSS_MS -> reset_to_scanning()
```
- **Remove** `g_resync_parked`, `resync_should_park`, `RESYNC_MISS_HOPS`,
  `RESYNC_PARK_MAX_MS`, o ramo ESTACIONADO e a janela `ESCRAVO_LISTEN_MS`. Não há
  deriva de fase a recuperar; o slot vem do tempo.
- A reancoragem só dispara em SLIP (deriva escorregou ≥1 slot), raro (µs/s).

### 3. Constantes (`shared/protocol.h`)
| Nova | Valor inicial | Racional |
|---|---|---|
| `SLOT_MS` | 30 | par com a cadência de sucesso atual (~14-25 ms) + guarda |
| `SLOT_GUARD_MS` | 4 | mestre não transmite nos últimos 4 ms do slot (settle + skew) |
| `LINK_LOSS_MS` | ~5000 | tempo sem RX válido até re-parear (era `LOSS_HOPS` em tempo) |
- Aposentar (remover ou reexpressar): `LOSS_HOPS`, `RESYNC_MISS_HOPS`,
  `RESYNC_PARK_MAX_MS`, `RESYNC_QUALITY_DECAY_HOPS` (vira contagem por slot ou
  por tempo), `ESCRAVO_LISTEN_MS`, `ATTEMPT_TIMEOUT_MS` (o ack-wait vira ≤
  `SLOT_MS − SLOT_GUARD_MS`), `CONTROL_*_HOPS`/`RECOVERY_COOLDOWN_HOPS`/
  `THROUGHPUT_WINDOW_HOPS`/`MASK_HYSTERESIS_HOPS` seguem em CONTAGEM DE SLOTS
  (semântica igual; um slot agora é tempo fixo `SLOT_MS`, então já são "por
  tempo" automaticamente — reavaliar valores).

### 4. Deriva e guarda (por que funciona)
- Skew inicial de `T0`: os dois `hop_start_at` vêm do mesmo handshake, ≤~2 ms.
  Com `SLOT_MS`=30 e o mestre transmitindo do início do slot até `SLOT_MS −
  SLOT_GUARD_MS`, a sobreposição dos slots dos dois lados é ~26 ms — folga.
- Deriva de cristal ~80 µs/s: para escorregar 30 ms seriam ~375 s de blackout
  total. Reancoragem por pacote zera a deriva a cada RX (constante sob operação
  normal e sob jammer de 1 canal, onde 15/16 slots passam).

## Sequência de implementação (cada passo testável, com soak)
1. **Constantes + slot util**: `SLOT_MS`/`SLOT_GUARD_MS`/`LINK_LOSS_MS` e um
   helper `current_slot(anchor)`. Sem mudar laço ainda. Build/native verdes.
2. **Escravo por tempo + reancoragem** (remove park/resync). Soak sem jammer:
   0 `loss_of_sync`, 0 slip anômalo (telemetria de reancoragem), throughput ≥
   baseline.
3. **Mestre por tempo** (slot do tempo; dado só no ack; perda por tempo). Soak
   sem jammer. Depois medir sob `hold ch10`: cadência de slot deve ser
   perfeitamente regular (sem cauda), escravo nunca "estaciona".
4. **Bateria A/B/C** (`run_battery.py --jammer-mode hold`) comparar C vs 7%
   (v3 por-evento) e 16% (pré-change). Depois `adaptive`.
5. **Re-tuning da blacklist** (separado): sob substrato sem deriva, ajustar
   limiares p/ não super-banir sob desense (é o teto real do PA_MAX).

## Critério de sucesso
- Sem jammer: 0 `loss_of_sync` em ≥ 10 min; miss estável e baixo (a cadência de
  sucesso não muda); 0 slips não explicados por deriva.
- Sob `hold ch10`: escravo nunca estaciona; slot regular; máscara converge sem o
  churn de park; retenção C ≥ a da v3 por-evento (7%) e idealmente ≥ 16%, com o
  teto restante sendo desense de banda larga (hardware), não protocolo.
- `pio test -e native_test` verde (adaptar testes de resync/park removidos;
  adicionar teste de `current_slot`/reancoragem).

## Riscos
- **Núcleo do transporte** — soak sem jammer + rollback por passo.
- **Skew de `T0` maior que o previsto** (se o handshake tiver jitter): mitigado
  pela guarda e pela reancoragem nos primeiros pacotes; medir o skew real.
- **Perda de reatividade**: o mestre salta no relógio mesmo sem dado; sob enlace
  ocioso, manda keepalive/fragmento corrente pra o escravo reancorar.

## O que NÃO fazer
- Não remover a dedup (`received_bitmap`) nem o split de contadores.
- Não mexer na permutação de salto nem no handshake de pareamento (só ADICIONAR
  a leitura do `T0`, que já está lá via `hop_start_at`).
- Não tentar bater a desense de banda larga do PA_MAX no protocolo (é hardware).
