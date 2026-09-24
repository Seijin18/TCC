## 0. Procedimento de bancada

- [x] 0.1 Desarmar o jammer deixado em `jam on` + `mode adaptive` por um
      teste anterior (`placa_b jam off`, 2026-09-24).
- [x] 0.2 `tools/validation/run_scenario.py` e `run_battery.py`: enviar
      `jam off` como primeiro comando (antes do `reset`), para nenhuma
      sessão herdar estado de jammer de uma sessão anterior.

## 1. Instrumentação (sem mudança de comportamento — ver design.md D1)

- [x] 1.1 `shared/radio_diag.h` (novo): `tlm_radio_regs(src, radio, reason)`
      emite `@<src>;e=regs;why=<boot|loss|pair_timeout|seed_timeout|cmd>;
      config=..;en_aa=..;en_rxaddr=..;rf_ch=..;rf_setup=..;status=..;
      fifo=..;feature=..;dynpd=..;rx_p1=..` (hex), lendo registradores via
      API pública do RF24 (`read_register` é protegido — usar
      `printPrettyDetails`/`sprintfPrettyDetails` ou subclasse fina).
- [x] 1.2 Mestre e escravo: `regs` no boot, em toda entrada no estado de
      pareamento, e sob comando `regs`.
- [x] 1.3 Mestre: `@mestre;e=pair_stats` ao fim de cada acampamento —
      `ch`, `probes_ok`, `probes_fail`, `rx_total`, `rx_link_ack`,
      `rx_other` (tamanho/tipo inesperado), `ms`.
- [x] 1.4 Escravo: `@escravo;e=pair_stats` a cada ciclo completo de
      varredura — `rx_total`, `rx_probe`, `rx_other`, `acks_ok`,
      `acks_fail`.
- [x] 1.5 Falhas de `radio_send()` no pareamento deixam de ser silenciosas
      (contadas em `pair_stats`; linha humana no primeiro caso de cada
      janela).
- [x] 1.6 `tools/logger/main.py`: últimos `regs`/`pair_stats` por nó no
      `state.json`; `pair_stats` fora de `ROUTINE_EVENTS` (vai pro
      `events.log`).
- [x] 1.7 `tools/validation/repair_trials.py` (novo): N ciclos de "enlace
      estável ≥ 5 s → força perda (`placa_b reset` / `placa_a reset`,
      alternando) → mede tempo até `pair_ok` + primeiro `hop` com ack dos
      dois lados", timeout por tentativa 60 s; grava
      `logs/repair-<ts>.json` com tempos, sucessos e os `regs`/`pair_stats`
      de cada tentativa.
- [ ] 1.8 `pio run` (todos os envs) + gravar A e B + `repair_trials.py -n 20`
      com jammer off ⇒ **número-base**. Diagnóstico registrado aqui: qual
      de H1/H4 (design.md) os dados sustentam.

## 2. Higiene do rádio (design.md D2)

- [ ] 2.1 `radio_reinit()` no mestre e no escravo (mesma sequência do
      `setup()`, incluindo `powerDown()/powerUp()`, flushes e limpeza de
      `STATUS`), extraída do `setup()` para os dois usarem o mesmo código.
- [ ] 2.2 Chamar `radio_reinit()` em `reset_to_pairing()` (mestre),
      `reset_to_scanning()` e timeout de seed (escravo).
- [ ] 2.3 `flush_rx()` a cada troca de canal no pareamento (mestre: início
      de acampamento; escravo: cada janela).
- [ ] 2.4 Confirmar na placa B que reinicializar o rádio do escravo não
      afeta o jammer (HSPI) — `jam_state` contínuo durante um
      `reset_to_scanning`.
- [ ] 2.5 `repair_trials.py -n 20` ⇒ comparar com 1.8 e registrar.

## 3. Handshake v2 (design.md D3)

- [ ] 3.1 `shared/protocol.h`: `PAIRING_PROTO_VER`, `LinkProbeV2`,
      `LinkAckV2` (+ `static_assert` de tamanho); `SeedPacket`/
      `SeedAckPacket` saem do fluxo de pareamento.
- [ ] 3.2 `shared/pairing.h` (novo, puro): `pair_decide_escravo(state,
      cur_nonce, pkt) -> {ADOPT_NEW, REACK, IGNORE}` e
      `pair_accept_ack_mestre(cur_nonce, seed, ack) -> bool`.
- [ ] 3.3 Testes em `test/` (native): sessão nova, re-ack com mesmo nonce,
      nonce diferente ⇒ nova sessão, ack antigo/versão errada rejeitados,
      ack com seed divergente rejeitado.
- [ ] 3.4 Mestre: nonce por acampamento (`esp_random()`), sonda v2, aceita
      só ack válido; remove `send_seed_to_slave()` do caminho de pareamento;
      conta `stale_ack` em `pair_stats`; `seed <n>` vale para a próxima
      tentativa.
- [ ] 3.5 Escravo: estado pareado-provisório (permanece no canal de
      pareamento até `hop_start_at`, re-ack idempotente com re-arme de
      `hop_start_at`, adota nova sessão em nonce diferente); confirma o
      enlace e chama `notify_ready(true)` só no primeiro `FragmentPacket`
      válido; timeout ⇒ volta a varrer.
- [ ] 3.6 Telemetria: `pair_ok` ganha `nonce=`; novos `pair_reack`,
      `pair_confirm`, `pair_ver_mismatch`; logger atualizado.
- [ ] 3.7 Comando de teste `drop_next_ack` no escravo (descarta o próximo
      `LinkAckV2` em vez de enviar) para validar 5.2 de forma determinística.

## 4. Temporização e blacklist de candidatos (design.md D4, D5)

- [ ] 4.1 Constantes: `LINK_ACK_WAIT_MS=30`, `LINK_RETRY_MS=100`,
      `PAIR_DWELL_MS=250`, `PAIR_CAMP_MS=4500` (ajustáveis pelos dados de
      1.8, mantendo as invariantes).
- [ ] 4.2 `static_assert`s das duas invariantes em `shared/protocol.h`.
- [ ] 4.3 `note_candidate_result(false)` só quando houve ack válido naquele
      candidato e o enlace não confirmou dados em `LOSS_HOPS` hops;
      acampamento sem nenhuma resposta não penaliza.
- [ ] 4.4 Atualizar os testes de `test_hopping` que dependam da semântica
      antiga (se houver) e `pio test -e native_test` verde.
- [ ] 4.5 `pio run` (10 envs) limpo.
- [ ] 4.6 *(opcional, só se 5.1 não atingir a meta)* experimento com ack de
      hardware (ESB) apenas nos pacotes de pareamento:
      `writeFast(..., false)` + `txStandBy(timeout)` + `setRetries()`.

## 5. Validação em bancada

- [ ] 5.1 `repair_trials.py -n 20`, jammer off: **≥ 19/20 re-pareamentos
      em ≤ 10 s**; registrar mediana e p95 contra 1.8 e 2.5.
- [ ] 5.2 Perda injetada (`drop_next_ack`): pareamento conclui pelo re-ack
      em ≤ 2 × `LINK_RETRY_MS` a mais que o normal; primeiro fragmento com
      ack sem `loss_of_sync`.
- [ ] 5.3 Reboot assimétrico (desligar uma placa ~5 s) ×5: recupera sem
      intervenção (fecha `adaptive-rendezvous` 6.3/6.5).
- [ ] 5.4 Jammer sweep/targeted ligado **antes** do boot: pareamento
      converge num candidato fora da faixa atacada (fecha
      `adaptive-rendezvous` 6.2).
- [ ] 5.5 Retomar a fila bloqueada, nesta ordem: jammer `adaptive` ao vivo
      (`targeted-jammer` 4.5) → `run_scenario.py --jammer-mode targeted`
      (`dynamic-seed-blacklist` 4.4/4.5) → `blacklist-decay` §4 →
      `run_battery.py` A/B/C (`scenario-automation` 4.2/4.3) →
      `spectrum-attack-classification` 3.1/3.3 → `two-board-topology`
      6.2-6.5.

## 6. Documentação

- [ ] 6.1 `adaptive-rendezvous/design.md` e `tasks.md` (§6, §7.5):
      substituir o diagnóstico "interferência de RF ambiente" pelas
      evidências deste change; apontar para cá.
- [ ] 6.2 `openspec/ROADMAP.md` e `openspec/project.md` (ordem de
      dependência): incluir este change como pré-requisito da validação de
      bancada.
- [ ] 6.3 Sincronizar a spec delta com o que foi de fato implementado;
      `openspec validate --changes --strict`; arquivar.
