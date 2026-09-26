## 1. Constantes e utilitário de slot (sem mudar o laço)

- [ ] 1.1 `SLOT_MS`=30, `SLOT_GUARD_MS`=4, `LINK_LOSS_MS`=5000 em
      `shared/protocol.h`. Helper `current_slot(anchor_ms)` =
      `(millis() - anchor) / SLOT_MS` (guardado p/ anchor no futuro).
- [ ] 1.2 Aposentar/reexpressar as constantes da era por-evento (`LOSS_HOPS`,
      `RESYNC_MISS_HOPS`, `RESYNC_PARK_MAX_MS`, `ESCRAVO_LISTEN_MS`,
      `ATTEMPT_TIMEOUT_MS`) — manter só o que o laço novo usa.
- [ ] 1.3 `pio run` + `pio test -e native_test` verdes (ajustar/remover testes
      de park/resync que não fazem mais sentido; adicionar teste de
      `current_slot` e da reancoragem por slip).

## 2. Escravo por tempo + reancoragem (remove park/resync)

- [ ] 2.1 Laço do escravo deriva `slot` do tempo; troca de canal na fronteira.
      No RX válido: dedup + ack (com `suspect_idx`) + reancora `hop_start_at` se
      |slot − pkt.hop_counter| ≥ 1. Telemetria `reanchor` (slip corrigido).
- [ ] 2.2 Remover `g_resync_parked`/`resync_should_park`/ramo ESTACIONADO/
      `ESCRAVO_LISTEN_MS`. Perda por `LINK_LOSS_MS` sem RX → `reset_to_scanning`.
- [ ] 2.3 Soak sem jammer: 0 `loss_of_sync`, throughput ≥ baseline, poucos/nenhum
      `reanchor` (só deriva). Medir o skew inicial de `T0`.

## 3. Mestre por tempo (dado só no ack; perda por tempo)

- [ ] 3.1 Laço do mestre deriva `slot` do tempo; `g_hop_counter := slot`; envia
      controle/fragmento no slot corrente respeitando `SLOT_GUARD_MS`; dado
      avança só no ack; attribution gate/suspect_idx/blacklist na fronteira.
- [ ] 3.2 Perda por tempo sem ack (`LINK_LOSS_MS`) → `reset_to_pairing`
      (preserva blacklist no reset por jamming, igual hoje).
- [ ] 3.3 Soak sem jammer (0 loss, baseline). Sob `hold ch10`: cadência de slot
      regular, escravo nunca estaciona, sem cauda de tempo.

## 4. Validação comparativa

- [ ] 4.1 `run_battery.py --jammer-mode hold --jammer-power max`: C vs 7%
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

### Resultado (preencher ao rodar)

- Skew inicial de T0 medido (mestre vs escravo):
- Soak sem jammer (loss / throughput / nº de reanchor):
- Cadência de slot sob jamming (regular?):
- Retenção C sob hold: pré-change 16% / por-evento v3 7% / por-tempo:
- Bateria PA menor:
- Bateria adaptive:
