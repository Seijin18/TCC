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

- [ ] 4.1 Rodar `tools/validation/run_scenario.py` com jamming direcionado a um subconjunto conhecido dos 16 canais, tempo suficiente pra ultrapassar `CHANNEL_FAIL_THRESHOLD` nesses canais — confirmar evento `blacklist` excluindo exatamente os canais visados.
- [ ] 4.2 Desligar o jammer e aguardar `RECOVERY_COOLDOWN_HOPS` (calibrar o valor inicial de `RECOVERY_COOLDOWN_HOPS` observando quantos hops/segundos isso representa nas condições reais de bancada) — confirmar um segundo evento `blacklist` reincluindo os mesmos canais, sem exigir nenhum novo alerta do analisador.
- [ ] 4.3 Repetir 4.1-4.2 com o alerta do analisador ainda ativo durante a janela de recuperação (ou seja, sem esperar `alert_off`) — confirmar que a reinclusão acontece de qualquer forma, já que deixou de depender de `g_alert_active`.
- [ ] 4.4 Forçar um cenário de flapping (jamming curto o bastante pra excluir um canal, cooldown cumprido, jamming no mesmo canal de novo logo em seguida) e confirmar que o canal é excluído novamente sem nenhum comportamento anômalo (oscilação rápida, mask travando, etc.).
- [ ] 4.5 Confirmar em `logs/events.log`/`tools/validation` que uma perda de sincronismo (`loss_of_sync`) no meio de um cooldown de recuperação não deixa `g_channel_blacklisted_since` com valores da sessão anterior (ver tarefa 2.4).
