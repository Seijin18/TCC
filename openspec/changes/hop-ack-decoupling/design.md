# Design — desacoplar o salto de frequência do ACK (hopping por-evento em lockstep)

## Princípio (referências)

Bluetooth salta a cada slot/evento **independente do ACK** (lockstep) e resolve
confiabilidade num layer separado: **ARQN** (1 bit: recebi/não) + **SEQN** (1
bit: sequência, filtra retransmissão). Um ACK perdido nunca dessincroniza — o
SEQN deduplica. Patentes de FH-ARQ usam número de sequência para o mesmo fim.
Aplicado aqui: o **canal** é `f(seed, hop_counter)` e o `hop_counter` avança por
TENTATIVA; a **entrega** é rastreada por `fragment_index` (avança só no ack); a
dedup usa `received_bitmap[fragment_index]` (já existe).

## Estado atual (o acoplamento a remover)

`mestre/src/node.h` (laço de fragmento, ~1249-1360):
```
fill_fragment(pkt, g_current_fragment);
while (millis() + 25 < hop_window_end /* = start + HOP_TIMEOUT_MS(220) */) {
    radio_send(pkt em channel(g_hop_counter));   // MESMO canal
    if (sent) { ack_ok = wait_fragment_ack(g_current_fragment, .., 25ms);
                if (ack_ok) break; }
}
// ... atribuição de falha ...
g_hop_counter++;               // avança 1x por JANELA de 220ms
if (ack_ok) g_current_fragment++;
```
Ou seja: reenvia o mesmo fragmento no mesmo canal por até 220 ms, depois salta
1. Sob ACK perdido, o escravo (que já avançou ao receber) fica num canal à
frente; o mestre segue reenviando no canal velho → deslize → park.

`escravo/src/node.h`: escuta `current_channel` por `HOP_TIMEOUT_MS`(220 ms);
`escravo_advance_hop()` no RX válido (adota `hop_counter` do pacote) ou no
timeout. `park` após `RESYNC_MISS_HOPS`(6) timeouts consecutivos.

## Mudança concreta

### 1. Mestre — uma tentativa por canal, salto por tentativa (o núcleo)
Reescrever o laço para NÃO ter janela de 220 ms de reenvio no mesmo canal:
```
// por hop:
uint8_t ch = channel(g_hop_counter);
send(pkt do fragmento g_current_fragment em ch);   // UMA vez
ack_ok = wait_fragment_ack(g_current_fragment, completed, ATTEMPT_TIMEOUT_MS);
// telemetria/latência/atribuição como hoje
if (ack_ok) g_current_fragment++;   // progresso de DADOS só no ack
else        /* fragmento fica pendente: será reenviado no PRÓXIMO canal */;
g_hop_counter++;                     // CANAL avança SEMPRE (a cada tentativa)
apply_scheduled_changes();
if (g_hops_since_ack >= LOSS_HOPS) reset_to_pairing();
```
- O fragmento não-ackado permanece em `g_current_fragment` e é reenviado
  automaticamente no próximo hop (próximo canal), onde o escravo já está.
- `g_hops_since_ack`/`g_channel_fail` seguem a mesma semântica (miss = sem ack),
  mas agora um miss custa ~30 ms, não 220 ms.
- Mantém o attribution gate de `jamming-mitigation-hardening` (só atribui falha
  isolada) e o `suspect_idx` do escravo.

### 2. Escravo — janela curta, lockstep, dedup
- Reduzir a escuta por hop de `HOP_TIMEOUT_MS` para `ATTEMPT_TIMEOUT_MS`
  (~30 ms) — o escravo avança na mesma cadência do mestre. No RX válido, adota
  `hop_counter` do pacote (correção fina) e re-acka; `received_bitmap` já evita
  remontar duplicata.
- `park` só como último recurso (ver item 4).

### 3. Constantes de contagem de hop — re-derivar para ~30 ms/hop
A cadência de um MISS cai de 220 ms para ~30 ms (~7×). Todas as constantes em
CONTAGEM de hops precisam subir para preservar o mesmo ORÇAMENTO DE TEMPO:

| Constante | Hoje (@ até 220 ms/hop) | Tempo alvo | Novo (@ ~30 ms/hop) |
|---|---|---|---|
| `LOSS_HOPS` | 25 (~5,5 s de miss) | ~5 s | ~150-180 |
| `RESYNC_MISS_HOPS` | 6 | manter margem | re-derivar c/ o assert |
| `RESYNC_PARK_MAX_MS` | (TABLE+2)·HOP_TIMEOUT | 1 ciclo | usar `ATTEMPT_TIMEOUT_MS` |
| `CONTROL_RETRY_HOPS` | 6 | ~tempo p/ ackar controle | escalar |
| `CONTROL_APPLY_MARGIN` | 2 | folga | escalar |
| `RECOVERY_COOLDOWN_HOPS` | 200 (~44 s @220ms; ~27 s medido) | manter em TEMPO | ~1500 |
| `THROUGHPUT_WINDOW_HOPS` | (ver mestre) | janela ~ const | escalar |
| `RESYNC_QUALITY_DECAY_HOPS` | 64 | janela ~ const | escalar |
| `CHANNEL_FAIL_THRESHOLD` | 3 | ~N visitas ao canal ruim | reavaliar |

> O `static_assert` do orçamento de resync (`RESYNC_MISS_HOPS·HOP_TIMEOUT +
> RESYNC_PARK_MAX_MS < LOSS_HOPS·HOP_TIMEOUT`) precisa ser reexpresso em termos
> de `ATTEMPT_TIMEOUT_MS`. Manter a garantia em COMPILAÇÃO.

### 4. Park como último recurso
Com lockstep, um miss isolado não desalinha (os dois avançam ~30 ms juntos, e o
`hop_counter` no pacote corrige quando algo passa). O `park` só deve disparar em
desalinhamento genuíno e sustentado (ex.: rádio surdo). Elevar
`RESYNC_MISS_HOPS` na nova escala e/ou condicionar o park a "N hops sem NENHUM
RX em NENHUM canal" (não a misses num canal jammed específico).

## Sequência de implementação (cada passo testável isolado, com soak)

1. **Introduzir `ATTEMPT_TIMEOUT_MS`** e re-derivar as constantes de contagem
   (item 3), SEM mudar a lógica de envio ainda — só compilar e soak sem jammer
   (0 loss) para garantir que a re-derivação não regrediu nada.
2. **Mestre: uma-tentativa-por-canal + salto por tentativa** (item 1). Soak sem
   jammer (0 loss, throughput baseline mantido). Depois medir sob `hold ch10`.
3. **Escravo: janela curta + park como último recurso** (itens 2, 4). Soak sem
   jammer. Medir sob `hold` e `adaptive`.
4. **Re-rodar a bateria A/B/C** (`run_battery.py --jammer-mode hold`) e comparar
   C vs os 25% atuais; alvo: subir claramente em direção a ~94%.

## Critério de sucesso

- Sem jammer: 0 `loss_of_sync` em soak de ≥ 10 min; throughput baseline ≥ o de
  hoje (a cadência de sucesso já é ~20-25 ms, então não deve cair).
- Sob `hold ch10` (Cenário C): **queda drástica de `resync_park`** e de stalls
  >1 s; ch10 banido em poucos segundos (não 36 s); retenção C bem acima de 25%.
- `pio test -e native_test` verde (ajustar/!adicionar testes de
  `get_next_active_channel`/orçamento com as novas constantes).

## Riscos e mitigação

- **Núcleo do transporte** (envio/recepção/hopping/pareamento) é a parte mais
  delicada do projeto (estável há várias sessões). Cada passo tem soak sem
  jammer + rollback se regredir a baseline.
- **Re-derivação de constantes** é a maior fonte de erro sutil — fazer o passo 1
  isolado e validar antes de mudar lógica.
- **Teto ~94%** (não 100%): um jammer de 1 canal sempre custa o slot dele até
  ser banido; isto remove o COLAPSO, não a perda de 1/16.

## O que NÃO fazer

- Não introduzir relógio compartilhado / time-slotting (o objetivo é manter
  event-driven; o lockstep vem da cadência por-tentativa, não de um clock).
- Não remover a dedup por `received_bitmap` (é o SEQN).
- Não mexer na permutação de salto (`hop-sequence`) nem no pareamento.
