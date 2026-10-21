## Why

Com o jammer agora emitindo de forma confiável (`jammer-emission-hardware`:
reinit da portadora + `mode hold`), a mitigação foi testada contra um atacante
FORTE de verdade pela primeira vez — e **recupera muito pouco**:

| Cenário (enlace em hopping, jammer `hold` no ch10, PA_MAX) | throughput |
|---|---|
| A — sem jammer (baseline) | ~9,7/s |
| B — jammer ON, mitigação OFF | ~0,1/s (**1%**) |
| C — jammer ON, mitigação ON | ~1,65/s (**~17%**) |

A blacklist ajuda ~14× (17% vs 1%), mas **não restaura o enlace**. O
diagnóstico (`e=blacklist`/`e=rekey_tx` ao longo de 120 s) mostra a máscara
**oscilando** sem convergir: `0xFFFF → 0xFEFF → 0x7AFD → 0x3AD0 → 0x2AC0 →
0xAAFF → …`, banindo até 10 canais e re-incluindo, nunca isolando de forma
limpa só o ch10 (bit 0).

Três causas se somam (todas confirmadas no código atual):

1. **A blacklist é apagada a cada re-pareamento.** `reset_to_pairing()`
   (`mestre/src/node.h`) faz `memset(g_channel_fail, 0)` e volta
   `g_channel_mask = 0xFFFF`. A requirement de `blacklist-decay` até formaliza
   isso ("estado de recuperação não sobrevive a um retorno ao pareamento").
   Sob jamming forte o enlace re-pareia toda hora, então nunca acumula um ban
   estável.
2. **Um único canal jammed dessincroniza o enlace.** Um hop perdido no ch10
   leva mestre/escravo a divergir de canal nos hops seguintes → misses
   consecutivos em canais limpos → `g_hops_since_ack >= LOSS_HOPS` →
   `reset_to_pairing()`. Ou seja, 1 canal morto derruba o enlace inteiro (é o
   que explica os ~95-100% de negação medidos para N=1..16 em
   `jammer-emission-hardware`).
3. **Atribuição de falha corrompida.** Durante o dessincronismo os dois lados
   estão em canais diferentes, então os misses incrementam `g_channel_fail`
   de canais **inocentes**, não do ch10 — a máscara culpa os errados.

Resultado prático: o resultado "self-healing validado" (PLR 22-32% → 0,4%,
`dynamic-seed-blacklist` 4.4) foi obtido com o jammer fraco/intermitente de
antes; contra o atacante confiável ele não se sustenta. Sem endurecer a
mitigação, a defesa do TCC não resiste ao caso de ataque mais básico (um
jammer parado num canal do conjunto de hopping).

## What Changes

- **Persistir a blacklist através do re-pareamento**: canais com falha
  dominante/recente continuam banidos depois de um `reset_to_pairing()`, em
  vez de zerar. Revisa a decisão de `blacklist-decay` ("estado não sobrevive
  ao pareamento") para o caso de perda **causada por jamming** (distinta de
  uma perda transitória qualquer).
- **Banir rápido um canal persistentemente ruim antes do colapso**: um canal
  com K falhas dominantes é blacklisted imediatamente (sem esperar o alerta do
  analisador nem acumular `LOSS_HOPS`), para que o enlace o descarte antes de
  dessincronizar.
- **Distinguir "canal jammed" de "perda de sincronismo"**: um miss num canal
  já sinalizado como ruim não conta para `LOSS_HOPS` (não escala para
  re-pareamento), e o `hop_counter` continua avançando no cronograma para um
  único hop ruim não desalinhar os dois lados. Protege a atribuição de falha
  de contaminar canais inocentes.
- **Escalonar para reseed quando a blacklist não basta** (jammer follower/
  adaptive ou cobrindo vários canais): se a fração de canais banidos passar de
  um limiar e o PLR continuar alto, trocar a seed (rekey) para reembaralhar a
  sequência, em vez de continuar banindo até esvaziar.
- **Validação com o atacante confiável**: re-rodar A/B/C com `mode hold`
  (canal fixo) e com `adaptive` (follower); critério de sucesso: C recupera
  para perto do baseline (não ~17%), sem oscilação de máscara, com o ban
  convergindo para o(s) canal(is) realmente atacado(s).

## Capabilities

### Modified Capabilities
- `adaptive-mitigation`: acrescenta persistência de blacklist através do
  re-pareamento, ban rápido de canal dominante, distinção jamming-vs-
  dessincronismo (para não escalar a re-pareamento nem contaminar canais
  inocentes) e escalonamento para reseed — de forma que a mitigação recupere o
  enlace sob um jammer forte de canal fixo, não só sob interferência fraca.

## Impact

- `mestre/src/node.h`: atribuição de falha e gatilho de `reset_to_pairing`
  (não zerar blacklist por jamming; não escalar por canal já banido);
  ban rápido; escalonamento para reseed.
- `escravo/src/node.h` / `shared/resync.h`: manter alinhamento de `hop_counter`
  sob um hop jammed sem re-parear; preservar a blacklist ao voltar a escanear.
- `shared/hopping.h` (`update_channel_mask`) e `shared/control_policy.h`:
  possivelmente ajustar limiares/decisão para o ban rápido e o escalonamento.
- **Dependência de ordem**: modifica a capability `adaptive-mitigation`, hoje
  definida pelos changes ativos `dynamic-seed-blacklist` e `blacklist-decay`.
  Idealmente arquivar esses dois antes (ou coordenar os deltas). Interage
  também com `hop-resync` (alinhamento sob perda) e `clean-channel-rendezvous`
  (re-pareamento).
- Depende do jammer confiável de `jammer-emission-hardware` (o `mode hold` é o
  atacante de teste). Ao concluir, destrava a re-execução de toda a bateria
  sob ataque com números confiáveis.
