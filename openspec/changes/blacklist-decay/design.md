## Context

`compute_candidate_mask()` (`mestre/src/node.h:417-428`) hoje só sabe
excluir: percorre os 16 canais, e qualquer um cujo `g_channel_fail[i]`
(contador de falhas de ACK, incrementado a cada miss e decrementado a
cada ack — `mestre/src/node.h:180`) atinja `CHANNEL_FAIL_THRESHOLD` sai da
máscara. Essa função só é chamada de dentro de `maybe_enqueue_control()`
(`mestre/src/node.h:446`), e só quando `g_alert_active` está ligado — ou
seja, a máscara para de ser reavaliada no instante em que o analisador
sinaliza fim de ataque, ficando congelada dali em diante até o próximo
alerta.

Um problema estrutural adicional: `g_channel_fail[i]` só muda quando o
mestre efetivamente tenta um hop naquele canal (incrementa em falha,
decrementa em sucesso — `mestre/src/node.h:783-806`). Uma vez que um canal
é excluído da máscara, `get_next_active_channel()` nunca mais o escolhe —
então `g_channel_fail[i]` fica congelado no valor que causou a exclusão,
para sempre. Não existe hoje nenhum sinal de "esse canal pode ter
melhorado" — nem tempo decorrido, nem nova tentativa, nada.

## Goals / Non-Goals

**Goals:**
- Canais blacklisted voltam a ficar disponíveis depois de um período de
  quarentena, sem exigir um novo ciclo de alerta do analisador.
- A decisão de reincluir um canal é local ao mestre (mesma filosofia já
  declarada em `dynamic-seed-blacklist/design.md`: evidência própria, não
  o alerta binário) — aqui a "evidência" é ausência de atividade
  suficiente por tempo suficiente, não confirmação ativa de que o canal
  está limpo (ver Trade-offs).
- Reavaliação de blacklist (nos dois sentidos — excluir e reincluir) para
  de depender de `g_alert_active`. O alerta continua sendo o único
  gatilho de `MUDAR_SEED` (`g_reseed_needed`), que é uma ação mais
  disruptiva e não faz sentido disparar por flutuação comum de PLR.
- A lógica de decisão (quando excluir, quando reincluir) fica pura e
  testável em `env:native_test`, seguindo a convenção já estabelecida
  para `shared/hopping.h` (`get_next_channel`/`get_next_active_channel`
  já são testados lá sem hardware).

**Non-Goals:**
- Não implementa nenhuma forma de "sondar" ativamente um canal blacklisted
  (enviar um fragmento de teste nele antes de confirmar recuperação) — a
  reinclusão é cega, baseada só em tempo decorrido. Ver Trade-offs.
- Não muda o formato dos pacotes de controle (`ControlBlacklistPacket`
  continua carregando só a máscara final) nem adiciona lógica no
  escravo — ele já aplica qualquer máscara recebida de forma agnóstica.
- Não cruza informação por canal do analisador (`channelData`/
  `channelHeat`) para decidir recuperação — permanece só o boolean de
  alerta existente, e mesmo esse não é mais usado pelo caminho de
  blacklist (só pelo de reseed). Cruzar informação por canal seria uma
  extensão bem maior (mudaria o formato de sinalização entre analisador e
  mestre) e fica fora de escopo aqui.

## Decisions

### Cooldown por hop_counter, não por millis()
Alternativa considerada: cooldown em tempo real (`millis()`). Rejeitada
pelo mesmo motivo que a reescrita do hopping da sessão anterior preferiu
contar em `hop_counter` em vez de relógio: o projeto já tem um padrão
estabelecido de usar o contador de hop (um inteiro que os dois lados
mantêm sincronizado) como "relógio lógico" em vez de `millis()`
independente por nó. Um bônus natural: sob condições ruins (canal ainda
comprometido, mais timeouts), cada hop demora mais perto de
`HOP_TIMEOUT_MS`, então o mesmo cooldown-em-hops automaticamente
corresponde a mais tempo real decorrido quando as condições estão piores
— sem precisar calcular isso explicitamente.

### Extrair a decisão para `shared/hopping.h` como função pura
Alternativa considerada: manter toda a lógica dentro de
`compute_candidate_mask()` em `mestre/src/node.h`, testada só
manualmente em bancada (como está hoje). Rejeitada: essa função já tem
uma sutileza real de timing (o timestamp de exclusão só pode ser gravado
na transição habilitado→excluído, nunca recomputado enquanto já excluído,
senão o cooldown nunca vence) e um caso de borda de underflow (ver
Riscos) — exatamente o tipo de lógica que se beneficia de teste
automatizado antes de ir pra bancada. Proposta: nova função
`update_channel_mask(mask, fail_count[], blacklisted_since[],
hop_counter, fail_threshold, recovery_cooldown_hops)` em
`shared/hopping.h`, sem estado próprio (opera sobre os arrays que o
chamador mantém) e sem nenhuma dependência de Arduino — roda em
`env:native_test` igual `get_next_active_channel`.
`compute_candidate_mask()` em `mestre/src/node.h` vira um wrapper fino
que chama essa função passando `g_channel_fail` e o novo
`g_channel_blacklisted_since`.

### Reinclusão é cega (baseada em tempo), não verificada
Alternativa considerada: sondagem ativa — gastar um hop ocasional
enviando no canal blacklisted antes de confirmar recuperação, só
reincluindo de fato após K sondagens bem-sucedidas (simétrico ao
`CHANNEL_FAIL_THRESHOLD` de exclusão). Rejeitada por complexidade: o
escravo deriva o canal de escuta puramente de `(seed, hop_counter,
mask)` — uma sondagem exigiria ele saber, com antecedência, que UM hop
específico vai usar um canal fora da máscara atual, o que significa mais
um tipo de pacote de controle e mais um estado a sincronizar, só para
esta função. Fica registrado como extensão futura possível se a
reinclusão cega se mostrar problemática em bancada (ver Riscos).

Alternativa considerada e também rejeitada: usar `channelData`/
`channelHeat` do analisador (que já mede continuamente todos os 126
canais, inclusive os blacklisted) como evidência de recuperação. Mais
preciso que reinclusão cega, mas exige cruzar informação por canal entre
analisador e mestre — hoje só um boolean cruza (ver conversa anterior
sobre o assunto) — e o analisador ainda tem calibração pendente
(`spectrum-attack-classification` tasks 3.1-3.3). Manter a reinclusão
independente do analisador é o que permite validar este change sem
esperar aquela calibração.

### Guarda de "nunca esvaziar todos os canais" reaproveitada sem mudança
`update_channel_mask()` preserva o guard existente (`candidate == 0` ->
retorna a máscara anterior) — como reinclusão só ADICIONA bits, ela nunca
pode ser a causa de uma máscara vazia; o guard continua relevante só para
o lado de exclusão.

## Risks / Trade-offs

- **Reinclusão sem verificação pode trazer de volta um canal ainda
  ruim** → mitigado pelo mesmo mecanismo que já existe para excluir: se o
  canal voltar a falhar, `CHANNEL_FAIL_THRESHOLD` falhas frescas (o
  contador é zerado na reinclusão) e ele é excluído de novo. Custo de um
  palpite errado é limitado a `CHANNEL_FAIL_THRESHOLD` hops de perda
  extra, não permanente.
- **Flapping** (canal saindo e voltando repetidamente perto do limiar) →
  mitigado por `RECOVERY_COOLDOWN_HOPS` ser bem maior que
  `CHANNEL_FAIL_THRESHOLD` (ordem de grandeza a calibrar em bancada, ver
  tasks.md) — o canal precisa ficar "quieto" (fora de uso) por bastante
  tempo antes de voltar a ser tentado.
- **Underflow de `hop_counter - blacklisted_since[i]`**: ambos são
  `uint32_t`; se `blacklisted_since[i]` nunca foi setado (0, valor
  inicial) e `hop_counter` já avançou, a subtração é válida (positiva) e
  provavelmente já excede o cooldown — o que faria um canal nunca-antes-
  blacklisted "elegível para reinclusão" default, mas isso é inofensivo
  porque só importa para bits que estão em 0 na máscara, e um canal nunca
  blacklisted sempre está em 1. O caso real de risco é após
  `reset_to_pairing()`: `g_hop_counter` volta a 0 mas
  `g_channel_blacklisted_since[]` manteria valores antigos (grandes) de
  uma sessão anterior — a subtração ficaria negativa e, por wraparound de
  unsigned, viraria um número enorme, tornando TODOS os canais
  "elegíveis" instantaneamente. Mitigação: `reset_to_pairing()` passa a
  zerar `g_channel_blacklisted_since[]` junto com `g_channel_fail`
  (mesmo padrão já usado ali).

## Migration Plan

1. `shared/hopping.h`: adicionar `update_channel_mask()` + testes em
   `test/test_hopping/test_hopping.cpp` cobrindo: exclusão básica,
   reinclusão após cooldown, não-reinclusão antes do cooldown, flapping
   (exclusão → reinclusão → exclusão de novo), guarda de máscara nunca
   vazia, e o caso de reset (contador voltando a um valor menor que
   `blacklisted_since`). Isso valida a lógica sem hardware antes de tocar
   `mestre/src/node.h`.
2. `mestre/src/node.h`: adicionar `g_channel_blacklisted_since[TABLE_SIZE]`,
   reescrever `compute_candidate_mask()` como wrapper de
   `update_channel_mask()`, remover a guarda `g_alert_active` do ramo de
   blacklist em `maybe_enqueue_control()`, resetar o novo array em
   `reset_to_pairing()`.
3. Validação em bancada com `tools/validation/run_scenario.py`: forçar
   perda real de fragmentos num canal específico (jammer targeted num
   subconjunto), confirmar blacklist via evento `blacklist` na
   telemetria, desligar o jammer, esperar `RECOVERY_COOLDOWN_HOPS` e
   confirmar uma segunda linha `blacklist` reincluindo o canal.

## Open Questions

- Valor inicial de `RECOVERY_COOLDOWN_HOPS`: nenhuma calibração em
  bancada ainda — proposto como tarefa de validação (tasks.md), não
  decidido aqui.
- Vale a pena o evento de telemetria `blacklist` carregar a máscara
  anterior além da nova (para `tools/logger`/`plot_scenario.py`
  distinguirem visualmente adição de remoção)? Proposto como tarefa
  opcional — não bloqueia a funcionalidade principal.
