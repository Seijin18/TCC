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

---

## Investigação de time-sync (2026-09-25) — findings + plano, antes de tocar no loop de hopping

Instrumentação read-only sobre a bateria `battery-20260925-165233` (Cenário C,
fase jamming 75 s, `hold ch10` PA_MAX). Dados que reorientam o diagnóstico:

### Achados

1. **Cadência de hop do mestre vira bimodal sob jamming.** Baseline: 32 hops/s,
   inter-hop mediano 20 ms, 5% de hops "lentos" (>150 ms). Jamming: 7,5 hops/s,
   mediano ainda ~26 ms MAS p90=226 ms e **máx=12,5 s**, 38% de hops lentos.
   Ou seja, a maioria dos hops é rápida (canal limpo), mas há uma cauda longa
   de hops que travam a janela inteira (canal jammed) ou re-pareiam (segundos).
2. **ch10 só é banido em +36 s** (metade da fase). Nos primeiros 36 s a máscara
   mantém bit0 setado (0xBFFF→0x3FFF→0x3FF9→…) e **bane canais INOCENTES antes**
   do ch10. Detecção lenta E errada.
3. **A máscara oscila o tempo todo** (24 trocas em 75 s, 4-6 canais banidos), e
   **ch10 é re-incluído periodicamente** (~a cada 27 s = RECOVERY_COOLDOWN_HOPS
   200 / ~7,5 hops/s) e re-banido — disrupção recorrente.
4. **Custo direto de tempo**: 12 `resync_park` (~0,6 s cada), 4 stalls de hop
   >1 s, 1 `loss_of_sync` (re-pareamento) — ~10-15 s dos 75 s perdidos parado.
5. **A deriva em si é pequena** (`resync_ok` hop_after-hop_before mediano 3,
   máx 12): o escravo não se perde longe; o caro é o **custo por park** (0,6 s
   pra reencontrar o mestre) e a frequência deles.

### Cadeia de causa (revisada)

Hop jammed → mestre gasta a janela toda tentando (cauda de 220 ms+) e/ou os dois
lados perdem fase → **escravo estaciona** → e o park:
- (a) **estrangula os FragmentAcks** — o sinal limpo do escravo ("nunca recebo no
  ch10") viaja no FragmentAck, que só sai quando ele recebe; parado, ele quase
  não manda ack, então o mestre demora a saber qual é o canal ruim (explica os
  36 s);
- (b) **corrompe a visão por-canal dos DOIS lados** — durante o park o escravo
  também não recebe nos canais inocentes em que caiu, então eles viram "ok=0"
  (suspeitos) e o mestre os bane;
- (c) custa throughput diretamente.

Ou seja, **persistência + detecção-pelo-escravo + attribution gate são todos
derrotados pelo mesmo dessincronismo/park.** O park é a peça central.

### Plano (cada item testável isolado, com rollback e soak sem-jammer)

1. **Manter o mestre em fase no canal jammed** (maior alavanca): limitar as
   tentativas por hop para o mestre abandonar um hop sem ack ~no mesmo instante
   em que o escravo expira, em vez de queimar a janela inteira — reduz o offset
   que dispara o park. Medir antes o instante exato de avanço dos dois lados.
2. **Detecção de ch10 robusta a park e rápida**: (a) o escravo só conta
   miss/ok de hops REAIS (não dos canais em que caiu durante o park — já é o
   caso do g_resync_channel_*, confirmar), e (b) levar o `suspect_idx` também
   nos ControlAck / num canal de retorno que não dependa de FragmentAck, pra o
   sinal chegar mesmo com o enlace degradado. Meta: banir ch10 em poucos
   segundos, não 36.
3. **Cortar o churn de re-teste do ch10**: subir RECOVERY_COOLDOWN_HOPS (ou
   re-inclusão condicionada a sondagem/energia) pra um canal jammed banido não
   ser re-incluído às cegas a cada ~27 s.
4. **Baratear o park**: orçamento menor / re-lock mais rápido.

Teto teórico (jammer 1-canal, blacklist perfeita): ~94% (perde 1/16 enquanto
banido). Alvo realista com 1-3: bem acima dos 25% atuais. Risco: itens 1 e 4
tocam o núcleo do hopping/resync (estável há várias sessões) — soak sem jammer
(0 loss) obrigatório a cada passo.
