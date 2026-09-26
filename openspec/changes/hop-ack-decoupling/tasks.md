## 1. Preparação: nova cadência e re-derivação de constantes (sem mudar lógica)

- [x] 1.1 Introduzir `ATTEMPT_TIMEOUT_MS` (~30 ms) em `shared/protocol.h` e
      passar a usá-lo como janela de escuta por hop do escravo e de espera de
      ack do mestre (hoje `HOP_TIMEOUT_MS`=220 / `attempt_wait_ms`=25).
      `HOP_TIMEOUT_MS` removido (só sobravam referências em comentário, todas
      atualizadas).
- [x] 1.2 Re-derivar as constantes de CONTAGEM de hops para preservar os mesmos
      orçamentos de TEMPO na nova cadência (~30 ms/hop): `LOSS_HOPS` 25→180,
      `RESYNC_MISS_HOPS` 6→45, `RESYNC_PARK_MAX_MS` agora
      `(TABLE_SIZE+2)*ATTEMPT_TIMEOUT_MS`=540, `CONTROL_RETRY_HOPS` 6→45,
      `CONTROL_APPLY_MARGIN` 2→15, `RECOVERY_COOLDOWN_HOPS` 200→1500,
      `THROUGHPUT_WINDOW_HOPS` 50→350, `g_window_size` 50→350,
      `RESYNC_QUALITY_DECAY_HOPS` 64→450 (contador `g_resync_quality_hops`
      alargado p/ uint16_t), `MASK_HYSTERESIS_HOPS` 16→112. `CHANNEL_FAIL_THRESHOLD`
      MANTIDO em 3 (conta VISITAS, não tempo). `static_assert` reexpresso em
      `ATTEMPT_TIMEOUT_MS`.
- [~] 1.3 `pio run` (7 envs) + `pio test -e native_test` (58 casos) VERDES,
      incluindo o `static_assert` do orçamento. **Soak sem jammer ≥ 10 min
      pendente (hardware — precisa das duas placas).**

## 2. Mestre: uma tentativa por canal, salto por tentativa (o núcleo)

- [x] 2.1 Reescrever o laço de fragmento em `mestre/src/node.h`: enviar o
      fragmento `g_current_fragment` UMA vez em `channel(g_hop_counter)`, esperar
      `ATTEMPT_TIMEOUT_MS` pelo ack; avançar `g_hop_counter` SEMPRE; avançar
      `g_current_fragment` só no ack (o não-ackado é reenviado no próximo canal).
      O `while` de reenvio no mesmo canal (janela de 220 ms) foi removido.
- [x] 2.2 Preservado: attribution gate (`ATTRIB_MAX_HOPS_SINCE_ACK`=1),
      `suspect_idx`, throughput/latência e a máquina de controle
      (blacklist/reseed) intactos — só muda QUANDO o hop avança, não a semântica
      de miss/ack. `g_current_fragment++` só no ack / `g_hop_counter++` sempre já
      eram assim; o que mudou foi tirar a janela de reenvio.
- [~] 2.3 Build verde. **Soak sem jammer + medição sob `hold ch10` pendentes
      (hardware).**

## 3. Escravo: janela curta, lockstep, park como último recurso

- [x] 3.1 Escuta por hop = `ATTEMPT_TIMEOUT_MS` (avança em lockstep com o mestre);
      adoção de `hop_counter` no RX e dedup por `received_bitmap` mantidos.
- [x] 3.2 `park` como último recurso: `RESYNC_MISS_HOPS` elevado 6→45 (na nova
      escala). O `g_resync_miss_count` já zera em QUALQUER RX válido (frag ou
      controle), então 45 timeouts CONSECUTIVOS sem nada = desalinhamento
      genuíno e sustentado, não um canal jammed pontual.
- [~] 3.3 Build verde. **Soak sem jammer + medição sob `hold`/`adaptive`
      pendentes (hardware).**

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
