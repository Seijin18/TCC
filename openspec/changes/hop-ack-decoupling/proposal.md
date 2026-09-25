## Why

O enlace FHSS dessincroniza sob jamming porque **o salto está acoplado ao
ACK**. Hoje o mestre reenvia o MESMO fragmento no MESMO canal, repetidamente,
dentro de uma janela de `HOP_TIMEOUT_MS`=220 ms (tentativas de ~25 ms), e só
então avança o `hop_counter`. Quando um ACK se perde — comum sob jamming — o
mestre fica preso reenviando num canal antigo, enquanto o escravo (que avançou
ao receber o fragmento) já foi para o próximo canal. Os dois deslizam ~1 hop,
não conseguem se reencontrar (estão em canais diferentes), e o escravo
estaciona.

Isto é a causa raiz do teto de ~25% da mitigação (investigação em
`jamming-mitigation-hardening/design.md`, "Investigação de time-sync"): sob
`hold ch10`, 12 `resync_park`/75 s, cadência de hop do mestre bimodal (mediana
26 ms mas cauda até 12,5 s), ch10 só banido em +36 s. O `park` estrangula os
`FragmentAck` e corrompe a atribuição de canal dos dois lados — então
persistência, detecção-pelo-escravo e attribution gate são todos derrotados
pelo mesmo dessincronismo.

**Referências** (sistemas que saltam por evento sem dessincronizar): o
Bluetooth avança o salto a cada slot/evento **independentemente do ACK**
(lockstep) e trata confiabilidade num layer separado com **ARQN (ack) + SEQN
(sequência)** — o SEQN filtra retransmissões, então um ACK perdido nunca causa
reprocessamento nem dessincronismo (Bluetooth Baseband spec; ACL ARQN/SEQN).
Patentes de FH-ARQ usam número de sequência para deduplicar retransmissões
(US4558453A; método de recuperação de FH). A lição: **desacoplar o salto do
ACK** mantém o hopping por-evento (sem relógio compartilhado) e remove o
dessincronismo do ACK perdido.

## What Changes

- **Separar dois contadores hoje fundidos**:
  - `hop_counter` avança a **cada tentativa de transmissão** (ack ou não) —
    seleciona o canal. É o "salta a cada evento" do Bluetooth.
  - `fragment_index` avança **só com ACK confirmado** — progresso de dados
    (ARQ).
- **Retransmitir o fragmento não-ackado no PRÓXIMO canal**, em vez de reenviar
  no mesmo. O mestre manda o fragmento N em `channel(hop_counter)`, espera uma
  janela CURTA; sem ack, incrementa `hop_counter` e reenvia N em
  `channel(hop_counter+1)` — onde o escravo já está. O mestre nunca fica preso
  num canal morto.
- **Deduplicar no escravo** por `(msg_id, fragment_index)` — já existe
  (`received_bitmap`): uma retransmissão é re-ackada mas não remontada duas
  vezes. É o SEQN.
- **Janela curta por tentativa nos dois lados** (~30 ms), para os dois
  avançarem em lockstep sem relógio compartilhado. O `hop_counter` carregado em
  cada pacote continua sendo a correção fina de sincronismo.
- **Re-derivar as constantes baseadas em CONTAGEM de hops** para a nova
  cadência (~30 ms/hop em vez de até 220 ms): `LOSS_HOPS`, `RESYNC_MISS_HOPS`,
  `CONTROL_RETRY_HOPS`/`CONTROL_APPLY_MARGIN`, `RECOVERY_COOLDOWN_HOPS`,
  `THROUGHPUT_WINDOW_HOPS`, `RESYNC_QUALITY_DECAY_HOPS`, `CHANNEL_FAIL_THRESHOLD`
  — todos preservando os mesmos ORÇAMENTOS DE TEMPO de hoje.
- **`park` vira último recurso**: com lockstep, o dessincronismo de rotina
  desaparece; o estacionamento (hop-resync) só age em desalinhamento genuíno e
  raro.

## Capabilities

### New Capabilities
- `hop-ack-decoupling`: o salto de frequência avança por tentativa de
  transmissão (desacoplado do ACK), com retransmissão do dado não-ackado no
  próximo canal e deduplicação por sequência — mantendo o hopping por-evento e
  eliminando o dessincronismo do ACK perdido.

### Modified Capabilities
- `hop-resync` (interação): o estacionamento deixa de ser o caminho comum sob
  perda e passa a último recurso; os orçamentos em hops são re-derivados para a
  nova cadência. (Coordenar com a spec de `hop-resync`.)

## Impact

- `mestre/src/node.h`: reestrutura o laço de envio de fragmento (uma tentativa
  por canal; avança `hop_counter` sempre, `fragment_index` só no ack).
- `escravo/src/node.h`: janela de escuta curta por hop; `park` como último
  recurso; dedup já existe.
- `shared/protocol.h`: re-derivação das constantes de contagem de hop; sem novo
  campo de pacote (`msg_id`/`fragment_index`/`hop_counter` já viajam).
- **Destrava o teto de `jamming-mitigation-hardening`**: sem o dessincronismo,
  a detecção do escravo chega rápido e a blacklist converge — C deve subir dos
  25% em direção ao teto de ~94%.
- **Risco**: mexe no núcleo do transporte (envio/recepção/hopping), estável há
  várias sessões. Soak sem jammer (0 `loss_of_sync`) obrigatório a cada passo,
  com rollback.
