## 1. Preparação: nova cadência e re-derivação de constantes (sem mudar lógica)

- [ ] 1.1 Introduzir `ATTEMPT_TIMEOUT_MS` (~30 ms) em `shared/protocol.h` e
      passar a usá-lo como janela de escuta por hop do escravo e de espera de
      ack do mestre (hoje `HOP_TIMEOUT_MS`=220 / `attempt_wait_ms`=25).
- [ ] 1.2 Re-derivar as constantes de CONTAGEM de hops para preservar os mesmos
      orçamentos de TEMPO na nova cadência (~30 ms/hop): `LOSS_HOPS`,
      `RESYNC_MISS_HOPS`, `RESYNC_PARK_MAX_MS`, `CONTROL_RETRY_HOPS`,
      `CONTROL_APPLY_MARGIN`, `RECOVERY_COOLDOWN_HOPS`, `THROUGHPUT_WINDOW_HOPS`,
      `RESYNC_QUALITY_DECAY_HOPS`, `CHANNEL_FAIL_THRESHOLD`. Reexpressar o
      `static_assert` do orçamento de resync em termos de `ATTEMPT_TIMEOUT_MS`.
- [ ] 1.3 `pio run` + `pio test -e native_test` verdes. Soak sem jammer ≥ 10 min:
      0 `loss_of_sync`, throughput baseline mantido. (Ainda sem mudar a lógica
      de envio — só valida a re-derivação.)

## 2. Mestre: uma tentativa por canal, salto por tentativa (o núcleo)

- [ ] 2.1 Reescrever o laço de fragmento em `mestre/src/node.h`: enviar o
      fragmento `g_current_fragment` UMA vez em `channel(g_hop_counter)`, esperar
      `ATTEMPT_TIMEOUT_MS` pelo ack; avançar `g_hop_counter` SEMPRE; avançar
      `g_current_fragment` só no ack (o não-ackado é reenviado no próximo canal).
- [ ] 2.2 Preservar attribution gate (`ATTRIB_MAX_HOPS_SINCE_ACK`), `suspect_idx`,
      throughput/latência e a máquina de controle (blacklist/reseed) — só muda
      QUANDO o hop avança, não a semântica de miss/ack.
- [ ] 2.3 Soak sem jammer (0 loss, baseline mantido). Depois medir sob
      `hold ch10`: cadência de hop deve ficar regular (sem cauda de 220 ms).

## 3. Escravo: janela curta, lockstep, park como último recurso

- [ ] 3.1 Escuta por hop = `ATTEMPT_TIMEOUT_MS` (avança em lockstep com o mestre);
      manter adoção de `hop_counter` no RX e o dedup por `received_bitmap`.
- [ ] 3.2 `park` só em desalinhamento genuíno e sustentado (elevar
      `RESYNC_MISS_HOPS` na nova escala e/ou condicionar a "N hops sem NENHUM RX
      em NENHUM canal", não a um canal jammed específico).
- [ ] 3.3 Soak sem jammer (0 loss). Medir sob `hold` e `adaptive`: `resync_park`
      deve cair drasticamente; ch10 banido em poucos segundos.

## 4. Validação comparativa

- [ ] 4.1 `run_battery.py --jammer-mode hold --jammer-power max` e comparar C vs
      os 25% atuais (mesmos scripts/gráficos). Alvo: subir claramente rumo a
      ~94%; PLR sob jamming cair; A continua colapsando (sanidade).
- [ ] 4.2 Bateria `adaptive` (follower) para o segundo perfil de atacante.
- [ ] 4.3 Se qualificar, copiar a sessão para `logs/working/` (`CLAUDE.md`).

## 5. Fechamento

- [ ] 5.1 Atualizar `jamming-mitigation-hardening` (o teto foi destravado?) e o
      `ROADMAP.md`/memória `project-rf-status` com os novos números.
- [ ] 5.2 `openspec validate hop-ack-decoupling --strict` e arquivar.

---

### Resultado (preencher ao rodar)

- Soak sem jammer pós-re-derivação (loss / throughput):
- Cadência de hop sob jamming (mediana / cauda) antes vs depois:
- `resync_park` sob `hold` antes (12/75s) vs depois:
- Tempo até banir ch10 antes (~36 s) vs depois:
- Retenção C sob `hold` antes (25%) vs depois:
- Bateria adaptive:
