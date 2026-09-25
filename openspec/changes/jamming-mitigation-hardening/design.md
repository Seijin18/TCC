# Design — endurecer a mitigação contra jamming de canal fixo

## Evidência de bancada (2026-09-25, `jammer-emission-hardware`)

Com o jammer confiável (`mode hold ch10`, PA_MAX) e o enlace em hopping:
- Mitigação OFF: ~0,1/s (1% do baseline) — enlace praticamente negado.
- Mitigação ON: ~1,65/s (~17%) — recupera parcialmente, não total.
- Traço de 120 s: 82 `rekey_tx`, 61 `blacklist`, 20 `rekey_failed`, 57
  `resync_park`, 56 `resync_ok`. Máscara oscila
  `0xFFFF→0xFEFF→0x7AFD→0x3AD0→0x2AC0→0xAAFF→…`, nunca fixa em `0xFFFE`
  (só ch10 banido). Ferramentas: `scratchpad/mitigation2.py`, `mitdiag.py`.

## As três causas (no código atual)

1. **Blacklist apagada no re-pareamento** — `reset_to_pairing()`
   (`mestre/src/node.h:~969-980`): `memset(g_channel_fail,0)`,
   `g_channel_mask=0xFFFF`, `memset(g_channel_blacklisted_since,0)`. Formalizado
   em `blacklist-decay` ("estado de recuperação não sobrevive ao pareamento").
2. **1 canal jammed → dessincronismo → LOSS_HOPS → reset** — um miss no ch10
   dispara a cadeia de resync; se os lados divergem de canal, vêm misses
   consecutivos em canais limpos até `g_hops_since_ack >= LOSS_HOPS`
   (`mestre/src/node.h:1291`), que chama `reset_to_pairing()`.
3. **Atribuição de falha contaminada** — no fragmento, o miss incrementa
   `g_channel_fail[find_channel_index(channel)]` (`node.h:1281`). Durante o
   dessincronismo o "channel" do mestre não é o do escravo, então canais
   inocentes acumulam falha e entram na máscara.

## Direções de correção (a detalhar na implementação)

### A. Persistir a blacklist através do re-pareamento
Não zerar `g_channel_fail`/`g_channel_mask`/`g_channel_blacklisted_since` num
`reset_to_pairing()` **causado por jamming**. Distinção mínima: um reset que
ocorre com pelo menos um canal já perto do limiar de ban preserva esse estado;
um reset "limpo" (boot, comando) zera. Alternativa mais simples e robusta:
sempre preservar a blacklist no reset e deixar o decaimento (`blacklist-decay`)
reincluir canais quando o cooldown vencer — o cooldown já garante que um ban
não é permanente. Isto **revisa** a requirement de `blacklist-decay`.

### B. Ban rápido de canal dominante (sem esperar alerta/LOSS_HOPS)
Se um canal atinge K falhas (ou K falhas consecutivas naquele canal) e é
claramente o pior, blacklist imediato — antes do enlace colapsar. Hoje o ban
depende de `CHANNEL_FAIL_THRESHOLD` avaliado em `compute_candidate_mask()` +
alerta; sob colapso rápido isso chega tarde. O ban rápido precisa propagar
(control packet + ack) num hop de canal limpo.

### C. Distinguir jamming de perda de sincronismo
- Um miss num canal **já banido / perto do ban** não conta para
  `g_hops_since_ack`/`LOSS_HOPS` (não escala para re-pareamento) — o enlace
  simplesmente pula aquele canal.
- Manter `hop_counter` avançando no cronograma quando um único hop falha, para
  um canal jammed não desalinhar os dois lados (coordenar com `hop-resync`:
  hoje o escravo pode estacionar; estacionar no canal jammed é
  contraproducente).
- Só contar falha para um canal quando há evidência de que os dois lados
  estavam nele (ex.: o pacote de controle/fragmento carrega `hop_counter`; se o
  escravo confirma o mesmo hop, a atribuição é confiável).

### D. Escalonar para reseed quando a blacklist satura
Se a fração de canais banidos ultrapassa um limiar (ex.: > 1/4) e o PLR
continua alto, disparar reseed (troca de seed reembaralha a permutação de
salto) em vez de continuar banindo até quase esvaziar. Útil contra follower
(`adaptive`) e contra jammer cobrindo vários canais. O reseed já existe
(`rekey`, gatilho por alerta); aqui vira também gatilho por saturação de ban.

### E. Não contaminar / convergir
Com A-C, a máscara deve convergir para `0xFFFE` (só ch10) sob `hold ch10` e
ficar estável, em vez de oscilar. É o teste de convergência.

## Critério de sucesso (validação com atacante confiável)

Re-rodar A/B/C (scripts de `jammer-emission-hardware`, `mode hold` e
`adaptive`):
- **hold canal fixo**: C (mitigação ON) recupera para **≥ 80% do baseline**
  (hoje ~17%), com a máscara convergindo para o canal atacado e **sem**
  ciclos de `reset_to_pairing` em regime (ou muito raros).
- **adaptive follower**: documentar o teto de recuperação; se o reseed (D)
  melhora vs só blacklist, registrar o ganho.
- Sem regressão: soak sem jammer continua 0 `loss_of_sync` (não introduzir
  bans espúrios em operação normal).

## O que NÃO fazer

- Não remover o decaimento de blacklist (`blacklist-decay`) — ele evita ban
  permanente; a persistência (A) apoia-se nele para reincluir canais quando o
  jammer sai.
- Não transformar tudo em reseed (D é escalonamento, não substituto do ban —
  reseed é mais disruptivo).
- Não depender do alerta do analisador como único gatilho (sob colapso o
  alerta nem sempe dispara — visto no traço: `alert_on=0`).
