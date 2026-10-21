## Why

O salto por evento não tem como manter mestre e escravo no MESMO canal ao
MESMO tempo sem um relógio compartilhado: os dois só se realinham quando um
pacote atravessa (o escravo adota o `hop_counter` do mestre no RX). Mas o
jamming é exatamente a condição que interrompe os pacotes — então a máquina de
alinhamento (adoção de counter, park/resync, janela assimétrica) fica mais
fraca justamente quando é mais necessária. `hop-ack-decoupling` provou isto em
bancada (2026-09-26): janelas casadas de 30 ms deram 65% de miss por deriva de
fase SEM jammer; a correção (janela de escuta do escravo maior que a do mestre)
passou o gate sem-jammer mas é um paliativo — faz o escravo ESTACIONAR mais no
canal morto, atrasando-o sob jamming. Sob `hold` PA_MAX a retenção regrediu
(16%→7%): a deriva/estacionamento corrompe a detecção e a blacklist super-bane.

O salto por TEMPO remove a falha na raiz: os dois lados derivam o canal do
TEMPO decorrido (`slot = (agora − T0) / SLOT_MS`, `canal = f(seed, slot)`).
Estão no mesmo canal POR CONSTRUÇÃO, tenha ou não passado pacote. Um slot
jammed é só desperdiçado — no próximo slot os dois já estão num canal novo,
alinhados. Sem park, sem cascata de resync, sem deriva de fase. É o que
Bluetooth e todo FHSS real fazem (o relógio do mestre é a referência; o
escravo sincroniza a ele).

**A infraestrutura já existe ~80%**: os dois lados já fixam uma âncora comum no
pareamento (`hop_start_at = millis() + HOP_START_DELAY_MS`, a ~1-2 ms um do
outro — um trânsito de pacote); o escravo já adota o `hop_counter` do mestre em
todo RX (correção fina de deriva já pronta); seed/máscara/função de canal já são
compartilhados. A virada é: **trocar "avança o counter num evento" por "calcula
o counter a partir do tempo, e reancora pelo pacote".** O split de contadores,
a dedup por sequência, o retransmit-no-próximo-slot e a blacklist continuam.

Deriva a favor: cristais do ESP32 diferem ~40-80 ppm (~80 µs/s no pior caso).
Para escorregar um slot inteiro de 30 ms seriam ~375 s de blackout TOTAL de
pacotes — e pacotes fluem o tempo todo (mesmo sob jammer de 1 canal, 15/16
slots são limpos e reancoram o relógio). O relógio atravessa qualquer jamming
realista sem perder o alinhamento de canal.

## What Changes

- **Salto por TEMPO, não por evento.** `hop_counter` deixa de avançar em
  ack/miss/RX/timeout e passa a ser `slot = (millis() − hop_start_at) / SLOT_MS`
  nos dois lados. `channel = f(seed, hop_counter, mask)` como hoje.
- **Reancoragem do relógio pelo pacote (correção de deriva).** No RX de um
  pacote com `hop_counter = N` do mestre, se o slot local escorregou (|slot −
  N| ≥ 1), o escravo corrige `hop_start_at` para voltar ao slot N. É a mesma
  adoção que já existe, agora aplicada ao relógio em vez do contador direto.
- **Mestre transmite dentro do slot corrente; avança dado só no ack.** Mantém o
  split `hop_counter` (tempo) × `fragment_index` (ack) do hop-ack-decoupling.
  Um fragmento não-ackado é reenviado no próximo slot (onde o escravo já está),
  deduplicado pelo `received_bitmap` (SEQN).
- **Escravo escuta o canal do slot corrente; sem park/resync-por-timeout.** O
  park (`hop-resync`) e a janela assimétrica (`ESCRAVO_LISTEN_MS`) deixam de
  existir — não há mais deriva de fase para recuperar. O escravo só reancora
  pelo pacote; um slot sem RX é só um slot perdido, ele avança no tempo.
- **Guarda de slot.** O settle de rádio (~500 µs, já existe) mais uma pequena
  folga no fim do slot absorvem o skew inicial de ~1-2 ms entre os dois `T0`.
- **Perda de enlace por TEMPO sem RX** (não por contagem de hops sem ack): se o
  escravo passa `LINK_LOSS_MS` sem nenhum RX válido, volta a parear (substitui
  o `LOSS_HOPS`/`RESYNC` da era por-evento).

## Capabilities

### New Capabilities
- `time-slotted-hopping`: o salto de frequência avança por TEMPO decorrido
  desde uma âncora comum estabelecida no pareamento, com o canal derivado do
  número do slot; o escravo reancora o relógio a partir do `hop_counter` que
  vem em cada pacote. Elimina a deriva de fase e o dessincronismo do salto por
  evento sem exigir troca de relógio dedicada (a âncora vem do handshake).

### Modified Capabilities
- `hop-resync`: o estacionamento/ressincronização por timeout deixa de existir
  (não há deriva de fase a recuperar no salto por tempo). A recuperação vira
  reancoragem do relógio pelo pacote + re-pareamento por `LINK_LOSS_MS`.
- `hop-ack-decoupling`: mantém o split de contadores, a dedup e o
  retransmit-no-próximo-slot; troca o gatilho de avanço do `hop_counter` de
  evento para tempo. A janela assimétrica (`ESCRAVO_LISTEN_MS`) é removida.

## Impact

- `shared/protocol.h`: `SLOT_MS`, `LINK_LOSS_MS`, `SLOT_GUARD_MS`; remove/aposenta
  as constantes de contagem de hop da era por-evento (`LOSS_HOPS`,
  `RESYNC_*`, `ESCRAVO_LISTEN_MS`, `ATTEMPT_TIMEOUT_MS`) ou re-expressa em tempo.
- `mestre/src/node.h`: laço de hopping passa a derivar o slot do tempo; envia o
  fragmento no slot corrente; avança dado só no ack; perda por tempo.
- `escravo/src/node.h`: laço passa a derivar o slot do tempo; reancora
  `hop_start_at` pelo `hop_counter` recebido; remove park/resync-por-timeout;
  perda por `LINK_LOSS_MS`.
- **Relação com `hop-ack-decoupling`**: este change SUPERA o mecanismo
  por-evento daquele (que fica arquivado como tentativa explorada + achados de
  fase que JUSTIFICAM esta virada). As partes válidas (split, dedup,
  retransmit-no-próximo) são preservadas.
- **Risco**: mexe no núcleo do transporte. Soak sem jammer (0 `loss_of_sync`)
  obrigatório antes de medir jamming; rollback por passo. Não destrava sozinho
  o teto de desense de banda larga do PA_MAX (hardware) — mas dá à blacklist um
  substrato sem deriva para a detecção funcionar.
