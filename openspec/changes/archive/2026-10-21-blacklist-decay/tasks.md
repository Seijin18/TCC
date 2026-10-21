## 1. Lógica pura em shared/hopping.h

- [x] 1.1 Adicionar `update_channel_mask(mask, fail_count[], blacklisted_since[], hop_counter, fail_threshold, recovery_cooldown_hops)` em `shared/hopping.h` — sem estado próprio, sem dependência de Arduino, operando sobre os arrays passados pelo chamador (ver design.md, seção "Extrair a decisão...").
- [x] 1.2 Constante `RECOVERY_COOLDOWN_HOPS` em `shared/protocol.h`, junto de `CHANNEL_FAIL_THRESHOLD` (valor inicial 200, a calibrar na seção 4).
- [x] 1.3 Testes em `test/test_hopping/test_hopping.cpp`:
      - exclusão básica (canal habilitado cruza o limiar → sai da máscara, `blacklisted_since` gravado com o `hop_counter` da exclusão).
      - reinclusão após cooldown (canal excluído há >= `RECOVERY_COOLDOWN_HOPS` → volta à máscara, contador de falha zerado).
      - não-reinclusão antes do cooldown (canal excluído há < `RECOVERY_COOLDOWN_HOPS` → permanece fora).
      - flapping (exclusão → cooldown cumprido → reinclusão → nova sequência de falhas → exclusão de novo), confirmando que o ciclo completo funciona sem estado inconsistente.
      - guarda de máscara nunca vazia continua respeitada quando a função exclui o último canal habilitado.
      - transição habilitado→excluído só grava `blacklisted_since` uma vez (chamadas repetidas com o canal já excluído não "empurram" o timestamp pra frente).
- [x] 1.4 `pio test -e native_test` passando com os novos casos. **Confirmado 2026-09-24**: 16/16 (10 pré-existentes + 6 novos).

## 2. Integração em mestre/src/node.h

- [x] 2.1 Adicionar `g_channel_blacklisted_since[TABLE_SIZE]` ao lado de `g_channel_fail[TABLE_SIZE]`.
- [x] 2.2 Reescrever `compute_candidate_mask()` como wrapper de `update_channel_mask()`, passando `g_channel_mask`, `g_channel_fail`, `g_channel_blacklisted_since`, `g_hop_counter`, `CHANNEL_FAIL_THRESHOLD`, `RECOVERY_COOLDOWN_HOPS`.
- [x] 2.3 Remover a guarda `if (g_alert_active)` do ramo de blacklist em `maybe_enqueue_control()` — passa a chamar `compute_candidate_mask()` e comparar com `g_channel_mask` incondicionalmente (o ramo de reseed via `g_reseed_needed` continua intocado, ainda exclusivamente disparado pelo alerta).
- [x] 2.4 `reset_to_pairing()` zera `g_channel_blacklisted_since[]` junto com `g_channel_fail`/`g_channel_mask` (ver design.md, risco de underflow após reset).

## 3. Telemetria (opcional, não bloqueia 1-2)

- [x] 3.1 **Decidido: sim.** O evento `blacklist` ganhou `prev=0x%04X` (máscara anterior) em `mestre/src/node.h` `apply_scheduled_changes()`. `tools/logger` já parseia campos genericamente (`k=v` -> dict), não precisou de mudança. `tools/validation/plot_scenario.py` atualizado para colorir exclusão (magenta) e reinclusão (verde) distintamente no painel 3, com fallback (mostra só magenta) em sessões antigas sem `prev`.

## 4. Validação manual em bancada

- [x] 4.1 Rodar `tools/validation/run_scenario.py` com jamming direcionado a um subconjunto conhecido dos 16 canais, tempo suficiente pra ultrapassar `CHANNEL_FAIL_THRESHOLD` nesses canais — confirmar evento `blacklist` excluindo exatamente os canais visados.
      **Validado 2026-09-25 (`logs/working/session-20260925-004847`, 10 min)** com interferência real no lugar do jammer: as exclusões caíram só nos canais com perda alta — ch 34 (59% de miss) 33×, ch 27 (34%) 4×, ch 46 (23%) 2× — e nenhum canal limpo (3-8% de miss) foi excluído. Com o jammer `targeted` (`session-20260925-010250`) a máscara chegou a excluir até 6 canais (`0x3F8D`).
- [x] 4.2 Desligar o jammer e aguardar `RECOVERY_COOLDOWN_HOPS` (calibrar o valor inicial de `RECOVERY_COOLDOWN_HOPS` observando quantos hops/segundos isso representa nas condições reais de bancada) — confirmar um segundo evento `blacklist` reincluindo os mesmos canais, sem exigir nenhum novo alerta do analisador.
      **Validado**: toda reinclusão veio exatamente **207 hops** após a exclusão (`RECOVERY_COOLDOWN_HOPS`=200 + margem de aplicação), o que nesta bancada dá 5-10 s, sem nenhum alerta envolvido. Valor mantido.
- [x] 4.3 Repetir 4.1-4.2 com o alerta do analisador ainda ativo durante a janela de recuperação (ou seja, sem esperar `alert_off`) — confirmar que a reinclusão acontece de qualquer forma, já que deixou de depender de `g_alert_active`.
      **Validado**: 37 das 39 reinclusões do soak aconteceram com o alerta do analisador **ativo** (alerta disparado pelo Wi-Fi real).
- [x] 4.4 Forçar um cenário de flapping (jamming curto o bastante pra excluir um canal, cooldown cumprido, jamming no mesmo canal de novo logo em seguida) e confirmar que o canal é excluído novamente sem nenhum comportamento anômalo (oscilação rápida, mask travando, etc.).
      **Validado com flapping real**: ch 34 (colado no Wi-Fi) foi excluído 33× em 10 min, reincluído às cegas após cada cooldown e excluído de novo — sempre respeitando os 207 hops, sem máscara travada nem oscilação mais rápida que o cooldown. Custo observado: a cada reinclusão o canal volta a perder pacotes até ser excluído de novo (~7-9 s de ciclo); um backoff para reincidentes fica como melhoria futura.
- [x] 4.5 Confirmar em `logs/events.log`/`tools/validation` que uma perda de sincronismo (`loss_of_sync`) no meio de um cooldown de recuperação não deixa `g_channel_blacklisted_since` com valores da sessão anterior (ver tarefa 2.4).
      **Validado** (`logs/working/session-20260925-010250`, jammer `adaptive`/`sweep`): em 7 perdas de sincronismo com máscara parcialmente excluída (até `0x3F8D`), a primeira máscara depois do re-pareamento sempre partiu de `prev=0xFFFF` e foi uma exclusão nova — nenhuma reinclusão herdada da sessão anterior.
- [x] 4.6 (achado de bancada) Em bancada (`logs/events.log` ~00:19) o mestre entrou em flood de controle: `rekey_tx kind=blacklist mask=0xFFFB apply=289`, o escravo ackava, e no hop seguinte o mestre reenviava a MESMA `mask` com `apply=290`, depois 291, 292... indefinidamente — todo hop carregava um pacote de controle em vez de dado, e a máscara nunca chegava a ser aplicada. Causa: `maybe_enqueue_control()` (`mestre/src/node.h`) comparava o candidato de `compute_candidate_mask()` só contra `g_channel_mask` (a máscara já *aplicada*); após o ack a troca fica apenas *agendada* (`g_mask_apply_scheduled`/`g_seed_apply_scheduled`, consolidada mais tarde por `apply_scheduled_changes()`), então o candidato recalculado a cada hop continuava diferente do já aplicado, uma nova proposta era enfileirada com `apply_at` mais distante, o escravo reagendava, e o ponto de aplicação fugia pra sempre. Fix: nova função pura `decide_control_proposal()` em `shared/control_policy.h` (com testes nativos em `test/test_control_policy/`) — não propõe nada novo enquanto `g_seed_apply_scheduled` ou `g_mask_apply_scheduled` estiver true (reavalia só depois de `apply_scheduled_changes()` consolidar), um `g_reseed_needed` pendente não se perde (a flag só é limpa ao efetivamente enfileirar a proposta), e a comparação de máscara passou a usar o alvo *efetivo* (`effective_channel_mask()`: valor agendado se houver, senão o aplicado) em vez de só `g_channel_mask`.
