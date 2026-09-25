## Context

Hopping por evento (`adaptive-rendezvous` "Correção 2"): o mestre fica no
canal do hop reenviando o fragmento a cada 25 ms até receber o ack ou a
janela `HOP_TIMEOUT_MS` (220 ms) acabar; o escravo escuta o canal corrente
até receber um fragmento válido (acka e avança na hora) ou a mesma janela
de 220 ms acabar (avança mesmo assim). O canal do hop `n` é
`get_next_active_channel(seed, n, mask)` = round-robin sobre os canais
ativos da máscara.

## Evidência (`logs/session-20260924-153644.jsonl`, jammer desligado)

8 enlaces em ~3 min, vida de 11-30 s cada. Sequência típica do mestre
antes da queda (✓ ack, ✗ sem ack): `… 10✓ 22✓ 34✗ 46✓ 58✓ … 22✓ 34✗ 46✗
58✗ 70✗ 82✗ 94✗ 100✗ … 125✗` → escravo `loss_of_sync` → mestre
`loss_of_sync`. Ch 34 falha em quase todo ciclo (Wi-Fi em 28-38); a falha
só vira apagão em alguns ciclos, e o apagão cobre canais limpos — não é
RF, é defasagem.

## Mecanismo

| Caso em 34 | Escravo | Mestre | Resultado |
|---|---|---|---|
| Fragmento perdido | espera 220 ms, avança | espera 220 ms, avança | chegam em 46 quase juntos; mestre reenvia a cada 25 ms → escravo pega → **recupera** |
| Ack perdido | recebeu → avança **na hora** (t+ε) | não viu ack → fica até t+220 | escravo sai de 46 em t+ε+220, mestre chega em t+220 → corrida de ~ε ms; perdida a corrida, **defasagem permanente de 1 canal** |

Sem nenhum pacote recebido, nada reaproxima os dois lados.

## Decisões

### D1 — Estacionar no último canal bom

Após `RESYNC_MISS_HOPS` timeouts consecutivos sem recepção, o escravo
estaciona em `g_last_good_channel` (canal do último `FragmentPacket`
válido — sabidamente utilizável e dentro da máscara) e escuta sem avançar.
O mestre, avançando por timeout a cada 220 ms, passa por ele em ≤
`active_count` hops. Ao receber: adota `pkt.hop_counter`, acka, avança
normalmente (mesmo caminho de hoje), emite `resync_ok`.

Alternativas descartadas:
- *Janela do escravo maior que a do mestre*: cobre o caso "ack perdido",
  mas cria o simétrico — num canal realmente jamado os dois dão timeout, e
  o escravo com janela maior fica atrasado um canal para sempre.
- *Escravo segurar o avanço após sucesso até o fim da janela do mestre*:
  resolve a corrida mas derruba o throughput (o hopping por evento avança
  na hora justamente para isso).
- *Mestre avisar "recebi seu ack" (3-way)*: mais um pacote por hop, mais
  um ponto de perda.

Estacionar é a técnica padrão de FHSS com contador no pacote: resolve
defasagem nos dois sentidos com uma única regra.

### D2 — Orçamento e limite

Estacionamento dura no máximo `RESYNC_PARK_MAX_MS = (TABLE_SIZE + 2) ×
HOP_TIMEOUT_MS` (≈ 3,96 s). `static_assert(RESYNC_MISS_HOPS ×
HOP_TIMEOUT_MS + RESYNC_PARK_MAX_MS < LOSS_HOPS × HOP_TIMEOUT_MS)` (0,44 +
3,96 = 4,4 s < 5,5 s): a ressincronização termina antes do mestre desistir
por `LOSS_HOPS`. Estacionamento esgotado ⇒ `reset_to_scanning()` (caminho
de perda atual). Enquanto estacionado, `g_hops_since_rx` continua contando
hops "virtuais" por tempo (1 a cada `HOP_TIMEOUT_MS`), para a estatística
de PLR continuar honesta.

### D3a — Pacotes de controle também carregam `hop_counter`

Enquanto há controle pendente (blacklist/seed), o mestre envia o pacote de
controle **no lugar** do fragmento em todo hop até ser ackado — um escravo
estacionado nunca veria um `FragmentPacket` e não teria de onde tirar o
contador. `ControlSeedPacket`/`ControlBlacklistPacket` ganham `uint32_t
hop_counter` (7 → 11 bytes, continuam do mesmo tamanho entre si; `type`
desambigua), preenchido com o hop corrente do mestre, e o escravo adota
esse contador ao receber qualquer um dos dois — estacionado ou não (mesma
correção de deriva que o fragmento já faz). Mestre e escravo precisam ser
gravados juntos.

### D3 — Mudanças agendadas durante o estacionamento

Seed/máscara com `apply_at_hop` agendado continuam valendo: ao adotar o
`hop_counter` do mestre, o escravo aplica o que estiver agendado para
hops ≤ o novo contador antes de calcular o próximo canal (mesmo
`apply_scheduled_changes()` de hoje). Se a mudança nunca chegou ao escravo
(controle perdido), a defasagem de máscara persiste — fora do escopo
(já coberto por `LOSS_HOPS`/re-pareamento).

## Implementado

- D2 (`RESYNC_PARK_MAX_MS`/`static_assert`): `shared/protocol.h` não incluía
  `shared/hopping.h` (só os consumidores o faziam, em ordens variadas --
  `shared/pairing.h`, por exemplo, inclui só `protocol.h`), então `TABLE_SIZE`
  não estava visível no ponto onde o `static_assert` precisa dele. Em vez de
  depender da ordem de include de cada consumidor, `protocol.h` passou a
  incluir `hopping.h` diretamente (sem risco de ciclo -- `hopping.h` não
  depende de nada de `protocol.h`), tornando-o autossuficiente.
- Tarefa 1.4: as duas decisões de estacionamento ("estacionar agora?" e
  "orçamento esgotou?") saíram como funções puras em `shared/resync.h`
  (`resync_should_park()`/`resync_park_expired()`), testadas em
  `test/test_resync/`. A contabilidade de hops "virtuais" durante o
  estacionamento (D2) ficou inline em `escravo/src/node.h` -- mexe em vários
  contadores globais de PLR e `millis()`, não ganha nada em virar função pura
  (só trocaria efeito colateral direto por uma lista grande de parâmetros por
  referência).
- Tarefa 1.5 (achado de bancada, soak de 2.1): a 1ª versão (D1) estacionava
  sempre em `g_last_good_channel` -- mas o soak com jammer desligado deu 11
  `resync_park`, **0 `resync_ok`**, 9 de 11 em ch 34 (colado no Wi-Fi
  28-38), todos expirando. Causa: no caso "ack perdido" (ver "Mecanismo"
  acima), a ÚLTIMA recepção válida acontece EXATAMENTE no canal ruim cujo
  ack se perdeu -- é a causa da própria defasagem -- então
  `g_last_good_channel` aponta pro pior canal disponível, e a única passada
  do mestre por ele (~220 ms a cada ~16 hops) falha de novo. Correção: o
  escravo agora mantém `g_resync_channel_ok[]`/`g_resync_channel_miss[]`
  (por posição em `HOPPING_CHANNELS`, saturando em 0xFF, decaindo por 2 a
  cada `RESYNC_QUALITY_DECAY_HOPS` (64) hops REAIS -- nunca durante hops
  "virtuais" do estacionamento) e escolhe o canal de estacionamento com
  `resync_pick_park_channel()` (`shared/resync.h`, pura, testada em
  `test/test_resync/`): score `ok - 2×miss` entre os canais habilitados com
  `ok > 0`, empate por menor `miss` e depois por menor índice (de graça, pela
  ordem de busca crescente), sem candidato ⇒ cai de volta em
  `g_last_good_channel`. `resync_park` ganhou o campo `score=` do canal
  escolhido.
- Tarefa 1.6 (experimento): mesmo estacionando no canal de melhor qualidade
  (1.5), o log do mestre mostra TX confirmado no canal estacionado (82/94/125,
  limpos) durante toda a janela de park sem nenhum `resync_ok` do lado do
  escravo -- consistente com a "surdez" pós-hop já vista em
  pairing-handshake-hardening (registradores idênticos ao boot, só limpos por
  `radio_reinit()`/reboot). A/B na entrada do park: contador de entradas
  alterna entre variante "listen" (`startListening()` simples, reafirma CE
  alto/PRIM_RX) e "reinit" (`radio_reinit()` completo, reprograma o canal
  para o candidato de pareamento e depois reaplica o canal de park); dump de
  registradores via `tlm_radio_regs(..., "park_pre")` antes de aplicar a
  variante, e `fix=listen|reinit` adicionado a `resync_park` e `resync_ok`
  (guardado em estático pra `resync_ok` reportar a variante do park corrente).
  **Removido após 1.8**: diagnóstico errado (ver 1.8 abaixo) -- soak de 2.1
  com o bug já corrigido deu 49/49 `resync_ok` com as duas variantes
  igualmente, então nenhuma delas fazia diferença de verdade.
- Tarefa 1.7 (experimento): mesmo com o escravo estacionado e reiniciado
  (1.6), o log do mestre confirma TX no canal de park sem nenhuma recepção
  do lado do escravo -- indício de que o rádio do MESTRE, não o do escravo,
  é quem trava (TX NO_ACK só confirma FIFO esvaziada, não irradiação real);
  após `loss_of_sync` os dois lados fazem `radio_reinit()` e o
  reparelhamento funciona em ~2s. `mestre/src/node.h` ganhou
  `maybe_reinit_radio_after_misses()`, chamada nos três pontos de
  `mestre_loop()` onde `g_hops_since_ack` é atualizado (fragmento com envio
  falho, fragmento com/sem ack, e controle via `send_pending_control_if_any`):
  a cada 3 misses consecutivos (3, 9, 15, 21 -- sempre antes de
  `LOSS_HOPS`=25) chama `radio_reinit()` sem tocar em `g_hop_counter`/
  seed/máscara/fragmento corrente, com `tlm_radio_regs(..., "miss_pre")`
  antes e telemetria `mestre_reinit` (`miss=`, `hop=`) depois -- o hop
  seguinte já chama `setChannel()` por conta própria, então o canal de
  pareamento em que `radio_reinit()` deixa o rádio é sobrescrito antes de
  qualquer TX/RX.
  **Removido após 1.8**: mesmo diagnóstico errado do 1.6 -- soak de 2.1 com
  o bug já corrigido deu 49/49 `resync_ok` sem nenhum reinit do mestre por
  misses, então a função nunca chegou a fazer diferença de verdade.
- Tarefa 1.8 (bug de bancada): `escravo_loop()` tinha
  `if (g_resync_parked) { if (timed_out) {...} } else if (timed_out) {...}
  else {...}` aninhado -- com `g_resync_parked==true` e `timed_out==false`
  (o caso comum, toda iteração até o timeout estourar), nenhum dos dois
  `if` internos disparava, e o `else` de recepção (`available()`/`read`)
  nunca rodava enquanto estacionado. Corrigido calculando `timed_out` uma
  única vez e testando `timed_out && g_resync_parked` / `timed_out` / `else`
  no mesmo nível. Isso invalida as conclusões dos experimentos 1.6 e 1.7:
  a "surdez" de rádio que motivou as variantes "listen"/"reinit" (1.6) e o
  `radio_reinit()` do mestre por misses (1.7) não existia -- o escravo
  estacionado simplesmente nunca chamava `available()`. Soak de 10 min com
  o bug corrigido (2.1) confirmou: 0 `loss_of_sync`, 49/49 `resync_ok`
  (mediana 2,2 s) -- os experimentos 1.6/1.7 foram então removidos (ver
  notas neles acima). Entrada no estacionamento, na versão final, é só
  `setChannel(current_channel); delayMicroseconds(500); flush_rx();` --
  igual ao settle/flush de `escravo_advance_hop()`, sem `startListening()`
  extra (o rádio já está escutando).

## Riscos

- Canal "último bom" ter virado ruim (jammer mudou de alvo): o mestre
  passa por ele e o fragmento se perde → estacionamento esgota → perda →
  re-pareamento (comportamento de hoje, sem regressão).
- Jammer `adaptive` seguindo o enlace pode travar exatamente no canal
  estacionado (há tráfego do mestre ali a cada ciclo) — medir em bancada.
