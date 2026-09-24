## Context

O pareamento atual (`adaptive-rendezvous`) é uma troca de 4 mensagens, todas
enviadas com `radio_send()` (NO_ACK — "enviado" nunca significa
"recebido"):

```
mestre (acampa num candidato)          escravo (varre 8 candidatos, 400 ms cada)
  MSG_LINK_PROBE (1 B)  ───────────▶   responde e já se considera pareado
                        ◀───────────   MSG_LINK_ACK (1 B)
  SeedPacket            ───────────▶   adota seed, notify_ready(true)
                        ◀───────────   SeedAckPacket
  (só agora o mestre começa a saltar)
```

Constantes atuais (`shared/protocol.h`): `LINK_RETRY_MS=300`,
`LINK_ACK_WAIT_MS=120`, `PAIR_CAMP_MS=2000`, `PAIR_DWELL_MS=400`,
`SEED_ACK_WAIT_MS=500`, `SEED_WAIT_TIMEOUT_MS=3000`, `LOSS_HOPS=25`.
Nenhum pacote de pareamento carrega identificação de sessão; nenhum dos
dois lados faz `flush_rx()` ao trocar de canal de pareamento, e
`RF24::startListening()` (RF24 1.6.2, `.pio/libdeps/*/RF24/RF24.cpp`) só
limpa os bits de IRQ, não a FIFO de recepção.

## Evidências (logs de 2026-09-23/24)

Todas extraídas de `logs/session-*.jsonl` (tempos relativos ao início da
sessão).

**E1 — handshake meio-aberto** (`session-20260924-124606`, 6,9 s):
`pair_ok` dos dois lados em ch 76, escravo recebe seed 42 e envia o
`SeedAck`; o mestre registra "Timeout esperando confirmação de seed" 500 ms
depois. O escravo, já pareado, descarta as sondas seguintes do mestre como
"Payload inesperado recebido (1 bytes)" (7,84 s, 8,03 s) e só sai por
`loss_of_sync` em 13,3 s. Uma única perda de pacote custou 6,4 s e deixou
os dois lados em estados incompatíveis.

**E2 — re-pareamento que nunca acontece, com encontro confirmado**
(mesma sessão): depois de 13,3 s, **zero** `pair_ok` em 2.841 s de sessão
(~1.300 acampamentos do mestre, ~6.900 janelas do escravo). Nos primeiros
segundos houve sobreposição real no mesmo canal: mestre acampado em ch 90
de 10,8 s a 16,9 s com o escravo escutando ch 90 em 13,3–13,7 s e
16,5–16,9 s; mestre em ch 96 de 17,0 s em diante com o escravo em ch 96 em
20,1–20,5 s. `LINK_RETRY_MS` (300) < `PAIR_DWELL_MS` (400), então cada
sobreposição tinha pelo menos uma sonda. O escravo não registrou **nenhum**
pacote recebido (nem "payload inesperado") depois de 13 s. No boot, a
mesma configuração pareou em 0,3 s.

**E3 — ack aceito sem escravo presente** (`session-20260924-113006`,
3.058–3.061 s): o mestre registra `pair_ok` em ch 76 **quatro vezes** (cada
uma seguida de timeout de seed) enquanto o escravo, pelos próprios
`pair_scan`, estava em ch 114/119/124/90/96 — e o escravo nunca registrou
ter enviado `LINK_ACK`. O mestre está aceitando um pacote que não pertence
à tentativa corrente (FIFO antiga atravessando troca de canal é o candidato
mais provável — ver H1).

**E4 — padrão entre sessões** (8 sessões): pareamento de boot ~100% em
~7 s após o boot. Re-pareamento após perda: rápido em 2 casos
(`20260923-012211`: 1 s; `20260924-112527`: 4 s), nunca em 3 casos
(`20260924-111258`, `-113006`, `-124606`).

**E5 — ambiente e jammer descartados**: o jammer só emite portadora
contínua (`startConstCarrier`) e está atrás do portão
`g_jammer_ready_source()` — com o escravo sem enlace, não emitiu nada
(nenhum `adaptive_lock` na sessão). O CSV do analisador mostra ~2% de
atividade média na banda, sem canal cronicamente ocupado. O canal de
acampamento do mestre mostra ~2× a atividade dos demais candidatos
(0,22 vs 0,11) — consistente com o mestre de fato transmitindo sondas.

**E6 — temporização não garante encontro**: acampamento do mestre
(`PAIR_CAMP_MS=2000`) é **menor** que o ciclo completo de varredura do
escravo (8 × 400 = 3.200 ms). Um acampamento pode terminar sem o escravo
ter passado pelo canal — e a blacklist de candidatos (adaptive-rendezvous
§7) conta isso como falha do canal. Com o escravo surdo (E2), isso vira o
ciclo `pair_mask 0xE1→0xE2→0xE4→0xE8→0xF0` observado: o mecanismo funciona
como especificado, mas está culpando canais pela ausência do escravo.

## Hipóteses de causa e como discriminar

| # | Hipótese | Explica | Como confirmar (etapa 1) |
|---|---|---|---|
| H1 | FIFO de RX antiga atravessa troca de canal/estado (nenhum `flush_rx()` no pareamento) | E3; parte de E2 | `pair_stats` do mestre com ack recebido sem sonda correspondente do escravo; `regs` mostrando `FIFO_STATUS` não-vazio na entrada do pareamento |
| H2 | Sem identificação de sessão, nenhum lado distingue pacote novo de antigo/duplicado | E1, E3 | Direto do protocolo — não precisa de medição |
| H3 | Mensagens finais NO_ACK (`LINK_ACK`, `SeedAck`) sem retransmissão ⇒ estados meio-abertos | E1 | Direto do protocolo |
| H4 | Rádio do escravo preso em estado que não recebe após a fase de salto | E2 | `regs` no `loss_of_sync` vs no boot (`CONFIG.PRIM_RX`, `EN_RXADDR`, `RX_ADDR_P1`, `RF_CH`, `FIFO_STATUS.RX_FULL`, `FEATURE`, `DYNPD`); `pair_stats` do escravo com `rx_total=0` durante sobreposição com `probes_ok>0` do mestre |
| H5 | Acampamento < ciclo de varredura | E6 | Aritmética das constantes |

H2, H3 e H5 são defeitos de projeto comprovados sem medição adicional. H1
e H4 são as explicações candidatas para E2/E3; a etapa 1 existe para
confirmar qual delas (ou ambas) antes de declarar o problema resolvido — a
etapa 2 as neutraliza de qualquer forma, mas sem a medição não saberíamos
se a correção funcionou pelo motivo certo.

## Goals / Non-Goals

**Goals:**
- Re-pareamento após perda de enlace confiável e rápido na bancada sem
  jammer: ≥ 19/20 tentativas forçadas em ≤ 10 s.
- Nenhum estado meio-aberto: uma perda isolada de pacote no handshake custa
  no máximo um intervalo de retransmissão, não um timeout de segundos.
- Nenhum pacote de pareamento aceito fora da tentativa corrente.
- Toda falha de pareamento diagnosticável pelo log (qual lado, qual etapa,
  estado do rádio).

**Non-Goals:**
- Autenticação/criptografia do handshake (o nonce é identificação de
  sessão, não segurança contra um adversário ativo que forje pacotes —
  fora do escopo do TCC, registrar como trabalho futuro).
- Mudar o protocolo de salto/dados, `ControlPacket` ou a mitigação.
- Usar ack de hardware (ESB auto-ack + retransmissão) no fluxo principal —
  fica como experimento opcional (tarefa 4.6), só se a etapa 3 não atingir
  a meta; o projeto evitou recursos de ack do NRF24 por dois bugs reais
  anteriores (`enableAckPayload()` e `RF24::write()`).

## Decisions

### D1 — Instrumentar antes de corrigir

`e=regs` (registradores relevantes em hex) e `e=pair_stats` (contadores
por janela de acampamento no mestre / por ciclo de varredura no escravo),
mais falhas de `radio_send()` visíveis. Motivo: E2 tem duas explicações
possíveis (H1, H4) e o registro de 2026-09-24 já errou uma vez atribuindo o
problema ao ambiente. `tools/validation/repair_trials.py` estabelece o
número-base (tempo até re-pareamento em N perdas forçadas) antes e depois
de cada etapa.

### D2 — Estado de rádio conhecido na entrada do pareamento

`radio_reinit()` em cada nó: repete a sequência de configuração do
`setup()` (power-cycle lógico `powerDown()/powerUp()`, PA, data rate, CRC,
dynamic payloads/dynamic ack, pipes, `flush_rx()`/`flush_tx()`, limpa
`STATUS`) e é chamada em toda transição para o estado de pareamento.
Custo ~5 ms, só em transições raras. Alternativa considerada: corrigir
cirurgicamente a causa exata de H4 — rejeitada como caminho único porque
depende de diagnóstico que pode não ser conclusivo, e um reset para estado
conhecido é barato e defensivo. `flush_rx()` também a cada troca de canal
durante o pareamento (mestre: após medir e ao começar a acampar; escravo:
a cada troca de janela).

### D3 — Handshake v2: 2 mensagens, nonce de sessão, re-ack idempotente

```
LinkProbeV2 { type=MSG_LINK_PROBE; ver; nonce(u16); seed(u16) }   6 B
LinkAckV2   { type=MSG_LINK_ACK;   ver; nonce(u16); seed(u16) }   6 B
```

- O mestre gera um `nonce` novo (`esp_random()`) a cada tentativa de
  pareamento (novo acampamento). Envia a sonda a cada `LINK_RETRY_MS`; só
  aceita `LinkAckV2` com `ver`, `nonce` e `seed` iguais aos da tentativa
  corrente — qualquer outro ack é descartado e contado em `pair_stats`
  (`stale_ack`).
- Ao aceitar o ack, o mestre considera o enlace pareado e agenda o início do
  salto (`HOP_START_DELAY_MS`). A seed vai na própria sonda, então
  `SeedPacket`/`SeedAckPacket` saem do fluxo de pareamento (o comando
  runtime `seed <n>` continua valendo: define a seed da próxima tentativa).
- O escravo, ao receber uma sonda válida, adota `{nonce, seed}`, responde
  `LinkAckV2`, entra em **pareado-provisório** e continua no canal de
  pareamento até `hop_start_at`. Enquanto provisório:
  - sonda com o **mesmo nonce** ⇒ re-envia o ack e **re-arma**
    `hop_start_at` (o último ack entregue define o alinhamento — ver
    Riscos);
  - sonda com **nonce diferente** ⇒ o mestre recomeçou; adota a nova sessão;
  - o primeiro `FragmentPacket` válido **confirma** o enlace (só então
    `notify_ready(true)` libera o jammer combinado);
  - sem fragmento válido em `SEED_WAIT_TIMEOUT_MS` ⇒ volta a varrer (mesmo
    caminho de hoje, agora com `radio_reinit()`).
- A decisão "aceitar / re-ackar / adotar nova sessão / ignorar" é uma
  função pura em `shared/pairing.h`, testada em `env:native_test`.
- `ver` evita que um nó gravado com o protocolo antigo seja confundido com
  o novo: versão diferente ⇒ ignora e loga `pair_ver_mismatch`.

Alternativa considerada: manter as 4 mensagens e só adicionar retries —
rejeitada, pois cada mensagem extra é mais um ponto de perda e o
`SeedAck` nunca teria confirmação de entrega de qualquer forma (o problema
dos dois generais só se resolve tornando a última mensagem idempotente e
confirmando pelo primeiro dado útil, que é o que D3 faz).

**Implementado (tarefa 3.5):** o escravo usa uma máquina de estados
explícita (`EscravoLinkState`: `SCANNING` / `PROVISIONAL` / `CONFIRMED` —
`escravo/src/node.h`) em vez de bolar mais um bool sobre
`link_established`/`g_seed_initialized` do adaptive-rendezvous (ambos
removidos). Detalhe que o texto acima não deixava explícito: a checagem de
re-sonda (mesmo nonce ⇒ re-ack; nonce diferente ⇒ adota nova sessão) só
roda **antes** de `hop_start_at` — o escravo ainda está fisicamente no
canal de pareamento nessa janela. Depois de `hop_start_at`, o escravo já
entrou no loop de salto normal (mesmo raciocínio de hoje: o mestre só
agenda `hop_start_at` depois de ele mesmo aceitar o ack, então não sobra
sonda pra reagir depois disso) e uma `LinkProbeV2` que chegasse ali cairia
no branch de "payload inesperado", sem efeito — não observado em bancada,
mas é o comportamento caso aconteça. Enquanto `PROVISIONAL` e depois de
`hop_start_at`, o timeout de confirmação (`SEED_WAIT_TIMEOUT_MS`) é
verificado a cada iteração do loop comparando `millis() - hop_start_at`, e
só passa a valer LOSS_HOPS/`reset_to_scanning()` de novo depois que o
estado vira `CONFIRMED`. O comando de teste `drop_next_ack` (tarefa 3.7)
descarta o envio mas mantém toda a adoção de sessão (nonce/seed, agenda de
`hop_start_at`) como se o ack tivesse saído — é exatamente o cenário que
5.2 precisa (mestre não recebe o ack, reenvia a sonda, escravo re-ackar na
segunda tentativa).

**H6 (achado de bancada 1.8) — auditoria completa:** a instância principal
(`send_seed_to_slave()` no mestre, print entre `radio_send()` e
`startListening()`) desaparece por remoção da função inteira, não por
reordenar. Auditados todos os pontos de TX→aguarda-resposta em
`mestre/src/node.h` e `escravo/src/node.h` (sondas/acks de pareamento,
envio/espera de `FragmentPacket`/`FragmentAck`, `ControlSeedPacket`/
`ControlBlacklistPacket`/`ControlAck`): nenhuma outra instância do padrão
encontrada — todos já seguiam (ou passaram a seguir, no handshake novo)
`stopListening()`→`radio_send()`→`startListening()`→só então
`guarded_printf`/`tlm_printf`. Um caso listado mas não corrigido por não
ser uma instância real: em `try_establish_link()`, o `guarded_printf` de
falha de `radio_send()` da sonda roda antes do `startListening()` do ramo
de falha — mas como o `radio_send()` já retornou falso (a sonda nunca
saiu), não há resposta em trânsito pra perder ali; esse código é anterior
a este change e foi deixado como estava.

### D4 — Temporização com invariantes verificadas em compilação

- `LINK_ACK_WAIT_MS` 120 → **30** (o ack sai em poucos ms; 120 ms só
  atrasa a próxima sonda).
- `LINK_RETRY_MS` 300 → **100** ⇒ ≥ 2 sondas por janela do escravo.
- `PAIR_DWELL_MS` 400 → **250** ⇒ ciclo de varredura 8 × 250 = 2.000 ms.
- `PAIR_CAMP_MS` 2000 → **4500** ⇒ ≥ 2 ciclos completos de varredura por
  acampamento.
- `static_assert(PAIR_DWELL_MS >= 2 * LINK_RETRY_MS)` e
  `static_assert(PAIR_CAMP_MS >= 2 * PAIRING_CANDIDATE_COUNT * PAIR_DWELL_MS)`
  em `shared/protocol.h`, para uma mudança futura de constante não quebrar
  o encontro silenciosamente (foi exatamente o que aconteceu quando o dwell
  subiu de 250 para 400 ms).

Os valores finais podem ser ajustados pela etapa 1 (tempo real de
resposta do escravo na placa combinada), mantendo as invariantes.

**Implementado (tarefas 4.1/4.2):** valores finais usados exatamente como
acima (30/100/250/4500 ms), sem ajuste adicional — os dados de bancada de
1.8/2.5 não deram motivo pra desviar. Os dois `static_assert`s ficam logo
depois de `PAIR_DWELL_MS` em `shared/protocol.h` (antes de
`PAIR_MEASURE_SAMPLES`), já que os dois precisam de `PAIRING_CANDIDATE_COUNT`
(definida mais acima) e das quatro constantes de timing.

### D5 — Blacklist de candidatos só com evidência de canal ruim

Hoje `note_candidate_result(false)` é chamado em (a) acampamento esgotado
sem nenhum ack e (b) seed não confirmada. Com D3, (b) deixa de existir e
(a) não distingue "canal ruim" de "escravo ausente/reiniciando/em outro
estado". Nova regra: conta falha só quando houve **evidência de presença
do escravo e o handshake não fechou** — ack com nonce correto recebido mas
nenhum `FragmentAck` nos primeiros `LOSS_HOPS` hops depois do pareamento
naquele candidato. Acampamento sem nenhuma resposta não penaliza o canal
(a medição de RPD em `measure_and_pick_candidate()` continua evitando
canais ocupados). Reaproveita `update_channel_mask()`/`g_candidate_*` sem
mudança.

**Implementado (tarefa 4.3):** duas flags por tentativa em
`mestre/src/node.h` — `g_pair_link_ack_received` (vira `true` no exato
ponto em que `try_establish_link()` retorna sucesso, antes mesmo de
agendar `hop_start_at`) e `g_pair_data_confirmed` (vira `true` na primeira
vez que `ack_ok` é verdadeiro no loop de fragmentos, i.e. o primeiro
`FragmentAck` depois do pareamento). As duas são zeradas em
`pick_new_pair_channel()` (nova tentativa = nenhuma evidência ainda) e
avaliadas no TOPO de `reset_to_pairing()`, antes de qualquer outro reset:
`note_candidate_result(false)` só é chamado se `link_ack_received &&
!data_confirmed`. Isso cobre os dois pontos onde `reset_to_pairing()` é
chamado hoje (LOSS_HOPS sem ack, tanto no branch de fragmento quanto no de
controle, e o comando manual `reset`) sem precisar duplicar a lógica em
cada um. O branch de acampamento esgotado (`camp_expired`, dentro de
`if (!g_link_established)`) não chama mais `note_candidate_result()` —
esse caminho nunca chega a setar `g_pair_link_ack_received`, então a
guarda já bastaria, mas a chamada foi removida de lá mesmo assim pra
deixar explícito que aquele branch nunca penaliza, sem depender de ler as
duas flags pra ter certeza. `note_candidate_result(true)` (sucesso) também
mudou de lugar: era chamado na confirmação de seed (removida), agora é
chamado inline no branch `ack_ok` do loop de hop, guardado pela mesma
`g_pair_data_confirmed` pra só contar a primeira vez por tentativa.

## Riscos / Trade-offs

- **Desalinhamento de início de salto no re-ack**: se o 1º ack se perde e o
  2º chega, o mestre começa a saltar ~`LINK_RETRY_MS` depois do que o
  escravo previa — o re-arme de `hop_start_at` a cada re-ack cobre o caso
  comum; o residual (último re-ack perdido, um anterior entregue) fica
  limitado a 100 ms e é absorvido pelo resync de `hop_counter` do
  `FragmentPacket` + hopping por evento. Validar em 5.2.
- **Gravação conjunta obrigatória**: formato de pareamento incompatível com
  o firmware atual; `ver` torna a incompatibilidade visível no log.
- **`radio_reinit()` na placa combinada**: o escravo (VSPI) e o jammer
  (HSPI) estão em barramentos separados desde `two-board-topology` §7, então
  reinicializar o rádio do escravo não toca o do jammer — confirmar em
  bancada que o jammer não reinicia junto.
- **H4 pode não ser confirmada**: se `regs` não mostrar diferença entre boot
  e pós-perda, a causa de E2 fica como "neutralizada por D2" sem
  explicação fina — aceitável para o objetivo, registrar honestamente.
- **Custo de energia/ocupação**: sondas a cada 100 ms dobram a ocupação do
  canal de acampamento durante o pareamento — irrelevante na bancada, e o
  pareamento é transitório.
