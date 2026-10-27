## 1. Constantes e utilitário de slot (sem mudar o laço)

- [x] 1.1 `SLOT_MS`=30, `SLOT_GUARD_MS`=4, `LINK_LOSS_MS`=5000 em
      `shared/protocol.h`. Helper `current_slot(anchor_ms)` =
      `(millis() - anchor) / SLOT_MS` (guardado p/ anchor no futuro).
- [x] 1.2 Aposentar/reexpressar as constantes da era por-evento (`LOSS_HOPS`,
      `RESYNC_MISS_HOPS`, `RESYNC_PARK_MAX_MS`, `ESCRAVO_LISTEN_MS`,
      `ATTEMPT_TIMEOUT_MS`) — manter só o que o laço novo usa.
- [x] 1.3 `pio run` + `pio test -e native_test` verdes (ajustar/remover testes
      de park/resync que não fazem mais sentido; adicionar teste de
      `current_slot` e da reancoragem por slip).

## 2. Escravo por tempo + reancoragem (remove park/resync)

- [x] 2.1 Laço do escravo deriva `slot` do tempo; troca de canal na fronteira.
      No RX válido: dedup + ack (com `suspect_idx`) + reancora `hop_start_at` se
      |slot − pkt.hop_counter| ≥ 1. Telemetria `reanchor` (slip corrigido).
- [x] 2.2 Remover `g_resync_parked`/`resync_should_park`/ramo ESTACIONADO/
      `ESCRAVO_LISTEN_MS`. Perda por `LINK_LOSS_MS` sem RX → `reset_to_scanning`.
- [x] 2.3 Soak sem jammer: 0 `loss_of_sync`, throughput ≥ baseline, poucos/nenhum
      `reanchor` (só deriva). Medir o skew inicial de `T0`.

## 3. Mestre por tempo (dado só no ack; perda por tempo)

- [x] 3.1 Laço do mestre deriva `slot` do tempo; `g_hop_counter := slot`; envia
      controle/fragmento no slot corrente respeitando `SLOT_GUARD_MS`; dado
      avança só no ack; attribution gate/suspect_idx/blacklist na fronteira.
- [x] 3.2 Perda por tempo sem ack (`LINK_LOSS_MS`) → `reset_to_pairing`
      (preserva blacklist no reset por jamming, igual hoje).
- [x] 3.3 Soak sem jammer (0 loss, baseline). Sob `hold ch10`: cadência de slot
      regular, escravo nunca estaciona, sem cauda de tempo.

## 4. Validação comparativa

- [x] 4.1 `run_battery.py --jammer-mode hold --jammer-power max`: C vs 7%
      (por-evento v3) e 16% (pré-change). Alvo: ≥ v3, idealmente ≥ 16%.
- [ ] 4.2 Bateria em PA menor (`high`/`low`): onde a desense é localizada, a
      blacklist deve mostrar ganho real — isola protocolo de hardware.
- [ ] 4.3 Bateria `adaptive` (follower).
- [ ] 4.4 Se qualificar, copiar sessão p/ `logs/working/` (`CLAUDE.md`).

## 5. Fechamento

- [ ] 5.1 Arquivar `hop-ack-decoupling` (superado pelo mecanismo por-tempo;
      achados de fase preservados como justificativa). Atualizar
      `jamming-mitigation-hardening`/`ROADMAP.md`/memória `project-rf-status`.
- [ ] 5.2 `openspec validate time-slotted-hopping --strict` e arquivar.

---

### Resultado de bancada (2026-09-26)

**Implementação:** exigiu um PREÂMBULO DE SINCRONISMO (não estava no plano
original): sem relógio compartilhado, o skew de fase do handshake deixava os
dois em slots diferentes permanentemente (100% miss, 0 RX). Os dois seguram o
canal do slot 0 até o primeiro ack e ancoram o relógio nesse instante. Dois
bugs de bancada no bootstrap do escravo: (1) `current_channel` stale após
rescan → escutava no canal errado (corrigido: invalida em `reset_to_scanning`);
(2) check de perda por tempo disparava na 1ª iteração (timer=0) → re-escaneava
antes de receber (corrigido: gated em `g_time_synced`).

**Soak sem jammer** (`session-20260926-001430`, ~145 s):
- **0 `loss_of_sync`, 0 reanchor (slips)** — o relógio NUNCA derivou um slot
  inteiro em 2,4 min (confirma a matemática de deriva: ~µs/s).
- Miss estável em cerca de 1% (mestre 1,0%; PLR do escravo 0,3-1,7% por janela) — vs 4-49% do por-evento. (Corrigido em 2026-10-27: o "1-6% (mediana 2-3%)" original não reproduz ao recontar o jsonl.)
- Cadência de slot PERFEITA: 333 slots/10 s = 30 ms/slot exato.
- ack latência p50=3 ms, p90=19 ms, max=20 ms.
- ~19 msg/s (pipeline de fragmentos por slot limpo) = ~4× o baseline antigo.

**Bateria hold PA_MAX** (`battery-20260926-031714`), retenção `msg_done/s`
jamming/baseline:

| Cenário | pré-change | por-evento v3 | **por-tempo** |
|---|---|---|---|
| A (sem hopping) | 0% | 0% | 0% |
| B (FHSS, sem mitigação) | 8% | 4% | **49%** |
| C (mitigação completa) | 16% | 7% | **83%** |

- Fase C jamming (60 s): **0 `loss_of_sync`, 0 slips** — o enlace NUNCA
  dessincronizou sob jamming máximo. Canais limpos passam ~100% (ch125 312/1,
  ch82 308/2); a máscara converge (~0x4031: canal atacado + poucos vizinhos
  dessensibilizados), sem o super-ban do por-evento (2-4 canais).
- O ~17% restante é o canal jammed + desense de banda larga dos vizinhos
  (limite de HARDWARE do PA_MAX), não falha de protocolo.
- Baselines ~4× maiores (15-19/s vs 4-6/s) pelo pipeline.

**Pendente:** bateria em PA menor e `adaptive`; arquivar `hop-ack-decoupling`.
