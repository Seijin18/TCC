## Why

Com o pareamento consertado (`pairing-handshake-hardening`, arquivado), a
bancada expôs o próximo defeito: **com o jammer desligado, o enlace cai a
cada 11-30 s** (sessão `logs/session-20260924-153644.jsonl`, 8 quedas em
~3 min), sempre se recuperando por re-pareamento — o que mascarava o
problema. Nas sessões de 2026-09-18/23 o mesmo enlace ficou 725 s e
1.262 s sem cair; as sessões da manhã de 2026-09-24 (antes de qualquer
mudança de pareamento) já mostravam enlaces de ~10 s, junto com Wi-Fi real
mais ativo perto da bancada (~24% de ocupação em ch 28-38).

Toda queda observada tem a mesma assinatura: uma cadeia de acks bem
sucedidos, uma falha no canal ruidoso (ch 34, colado no Wi-Fi), e a partir
dali **todo hop falha, em todos os canais — inclusive os limpos 94-125**
(`34✗ 46✗ 58✗ 70✗ … 125✗`), até `LOSS_HOPS` derrubar o enlace. Uma falha
isolada em 34 normalmente se recupera (`34✗ 46✓`); a que não se recupera
é a em que o escravo **recebeu** o fragmento mas o **ack se perdeu**: o
escravo avança na hora (evento de sucesso), o mestre continua
retransmitindo em 34 até o fim da janela de 220 ms, e o escravo fica ~uma
janela adiantado — sai do próximo canal justo quando o mestre chega. A
partir daí os dois ficam defasados de um canal para sempre: nada no
protocolo reaproxima os dois lados se nenhum pacote é recebido (o
`hop_counter` no `FragmentPacket` só corrige quando algo chega).

Isso bloqueia toda a validação sob ataque que falta: qualquer perda de ack
num canal atacado vira queda do enlace, e os números de PLR/throughput dos
cenários A/B/C mediriam re-pareamentos em vez de mitigação.

## What Changes

- **Escravo: ressincronização por estacionamento.** Após
  `RESYNC_MISS_HOPS` (2) timeouts de hop consecutivos sem nada válido
  recebido, o escravo para de avançar por conta própria e **estaciona** no
  último canal em que recebeu um fragmento válido, escutando
  continuamente. Como a sequência de canais é um round-robin sobre a
  máscara ativa (`get_next_active_channel`), o mestre passa por esse canal
  em no máximo `active_count` hops. O primeiro `FragmentPacket` válido
  traz o `hop_counter` do mestre → o escravo adota, acka e volta a saltar
  normalmente. Se o estacionamento durar mais que um ciclo completo do
  mestre (+ folga) sem nada, segue o caminho de perda de sempre.
- Orçamento de tempo com invariante verificada em compilação:
  `RESYNC_MISS_HOPS × HOP_TIMEOUT_MS + TABLE_SIZE × HOP_TIMEOUT_MS + folga
  < LOSS_HOPS × HOP_TIMEOUT_MS` — a ressincronização tem que caber antes
  do mestre desistir.
- Telemetria: `@escravo;e=resync_park;ch=..;hop=..` ao estacionar e
  `@escravo;e=resync_ok;ch=..;hop_before=..;hop_after=..;ms=..` ao
  recuperar (a diferença de hop mostra se o escravo estava adiantado ou
  atrasado — confirma o mecanismo em bancada).
- Mestre: nenhuma mudança de protocolo (continua retransmitindo a cada
  25 ms na janela do hop).

## Capabilities

### New Capabilities
- `hop-resync`: recuperação de sincronismo de salto sem re-pareamento.

## Impact

- `escravo/src/node.h`: estado de estacionamento no laço de salto;
  contagem de perda (`g_hops_since_rx`) passa a considerar o tempo
  estacionado.
- `shared/protocol.h`: `RESYNC_MISS_HOPS`, limite do estacionamento,
  `static_assert` do orçamento.
- `tools/logger/main.py`: `resync_park`/`resync_ok` no `events.log`.
- Validação: soak com jammer desligado (meta: nenhuma `loss_of_sync` em
  10 min com o Wi-Fi ambiente atual) e com jammer `adaptive`/`targeted`.
- Depende de `dynamic-seed-blacklist` (máscara) e do handshake de
  `pairing-handshake-hardening` (arquivado).
