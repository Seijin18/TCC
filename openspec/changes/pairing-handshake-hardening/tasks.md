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
- [x] 1.8 `pio run` (todos os envs) + gravar A e B + `repair_trials.py -n 20`
      com jammer off ⇒ **número-base**. Diagnóstico registrado aqui: qual
      de H1/H4 (design.md) os dados sustentam.
      **Resultado (2026-09-24, 14:44-14:53)**: **0/6** re-pareamentos
      (interrompido após 6 timeouts de 60 s — mais tentativas não mudariam
      o número). Em todas: mestre `probes_ok=7` por acampamento, escravo
      `rx_total=0` em todo ciclo. **H1 refutada** (`rx_other=0`,
      `fifo=11`/vazia em todos os dumps). **H4 confirmada numa forma mais
      forte que a prevista**: os registradores do escravo pós-perda são
      *idênticos* aos do boot (`config=7F en_rxaddr=02 feature=05 dynpd=3F
      rx_p1=SLV01`, canal de pareamento correto, conferido também com
      `regs` sob comando no meio da falha) e mesmo assim ele não recebe
      nada — estado interno do rádio (clone NRF24) invisível nos
      registradores. Bisseção: reiniciar **só a placa B** (pulso de RTS,
      mestre intacto) ⇒ pareou em ~7 s; logo a falha é do lado do escravo.
      **Achado novo (H6)**: `send_seed_to_slave()` faz `guarded_printf()`
      *entre* `radio_send()` e `startListening()`; na placa A essa linha
      disputa o mutex da UART com o CSV de ~400 B do analisador (dezenas
      de ms a 115200), então o `SeedAck` do escravo chega com o mestre
      ainda em TX — `seed_timeout` em praticamente todo pareamento
      observado (é a causa do E1). Regra que vale pro protocolo inteiro:
      nenhum print/telemetria entre transmitir e voltar a escutar quando
      se espera resposta.

## 2. Higiene do rádio (design.md D2)

- [x] 2.1 `radio_reinit()` no mestre e no escravo (mesma sequência do
      `setup()`, incluindo `powerDown()/powerUp()`, flushes e limpeza de
      `STATUS`), extraída do `setup()` para os dois usarem o mesmo código.
- [x] 2.2 Chamar `radio_reinit()` em `reset_to_pairing()` (mestre),
      `reset_to_scanning()` e timeout de seed (escravo).
- [x] 2.3 `flush_rx()` a cada troca de canal no pareamento (mestre: início
      de acampamento; escravo: cada janela).
- [x] 2.4 Confirmar na placa B que reinicializar o rádio do escravo não
      afeta o jammer (HSPI) — `jam_state` contínuo durante um
      `reset_to_scanning`.
      **Resultado (15:32-15:34)**: jammer em `jam_state_on` desde 15:32:47
      atravessou vários `radio_reinit()` do escravo (`loss_of_sync`
      15:32:53, `pair_timeout` 15:33:33, ...) sem `jam_state_off` nem
      reboot, heartbeat contínuo.
- [x] 2.5 `repair_trials.py -n 20` ⇒ comparar com 1.8 e registrar.
      **Resultado (14:57)**: com `radio_reinit()` o escravo **volta a ouvir
      em ~1 s** após `loss_of_sync` (antes: nunca) — `powerDown()/powerUp()`
      limpa o estado preso do H4. Mas o enlace não fecha: todo re-pareamento
      cai no H6 (`seed_timeout` no mestre) → escravo meio-aberto → perda →
      reinit → re-pareia → H6 de novo, em ciclo de ~7 s. Rodar os 20 trials
      nesse estado só mediria o H6; o critério de sucesso do script (dado
      fluindo) fica para depois da etapa 3, que elimina o `SeedAck`.
- [x] 2.6 *(achado em bancada)* Comandos `reset`/`regs`/`mode` da placa B
      chegam pela task do jammer (core 0) via `jammer_set_unknown_cmd_sink`
      e mexiam no rádio/estado do escravo em paralelo com `escravo_loop()`
      (core 1) — corrida de SPI. Corrigido: `handle_escravo_command()` só
      marca pedido pendente, consumido no próprio `escravo_loop()`. Placa
      A não tem o problema (serial lida pela task do mestre).

## 3. Handshake v2 (design.md D3)

- [x] 3.1 `shared/protocol.h`: `PAIRING_PROTO_VER`, `LinkProbeV2`,
      `LinkAckV2` (+ `static_assert` de tamanho); `SeedPacket`/
      `SeedAckPacket` saem do fluxo de pareamento.
- [x] 3.2 `shared/pairing.h` (novo, puro): `pair_decide_escravo(state,
      cur_nonce, pkt) -> {ADOPT_NEW, REACK, IGNORE}` e
      `pair_accept_ack_mestre(cur_nonce, seed, ack) -> bool`.
- [x] 3.3 Testes em `test/` (native): sessão nova, re-ack com mesmo nonce,
      nonce diferente ⇒ nova sessão, ack antigo/versão errada rejeitados,
      ack com seed divergente rejeitado.
- [x] 3.4 Mestre: nonce por acampamento (`esp_random()`), sonda v2, aceita
      só ack válido; remove `send_seed_to_slave()` do caminho de pareamento;
      conta `stale_ack` em `pair_stats`; `seed <n>` vale para a próxima
      tentativa.
- [x] 3.5 Escravo: estado pareado-provisório (permanece no canal de
      pareamento até `hop_start_at`, re-ack idempotente com re-arme de
      `hop_start_at`, adota nova sessão em nonce diferente); confirma o
      enlace e chama `notify_ready(true)` só no primeiro `FragmentPacket`
      válido; timeout ⇒ volta a varrer.
- [x] 3.6 Telemetria: `pair_ok` ganha `nonce=`; novos `pair_reack`,
      `pair_confirm`, `pair_ver_mismatch`; logger atualizado.
- [x] 3.7 Comando de teste `drop_next_ack` no escravo (descarta o próximo
      `LinkAckV2` em vez de enviar) para validar 5.2 de forma determinística.

## 4. Temporização e blacklist de candidatos (design.md D4, D5)

- [x] 4.1 Constantes: `LINK_ACK_WAIT_MS=30`, `LINK_RETRY_MS=100`,
      `PAIR_DWELL_MS=250`, `PAIR_CAMP_MS=4500` (ajustáveis pelos dados de
      1.8, mantendo as invariantes).
- [x] 4.2 `static_assert`s das duas invariantes em `shared/protocol.h`.
- [x] 4.3 `note_candidate_result(false)` só quando houve ack válido naquele
      candidato e o enlace não confirmou dados em `LOSS_HOPS` hops;
      acampamento sem nenhuma resposta não penaliza.
- [x] 4.4 Atualizar os testes de `test_hopping` que dependam da semântica
      antiga (se houver) e `pio test -e native_test` verde.
- [x] 4.5 `pio run` (10 envs) limpo.
- [ ] 4.6 *(opcional, só se 5.1 não atingir a meta)* experimento com ack de
      hardware (ESB) apenas nos pacotes de pareamento:
      `writeFast(..., false)` + `txStandBy(timeout)` + `setRetries()`.

## 5. Validação em bancada

- [x] 5.1 `repair_trials.py -n 20`, jammer off: **≥ 19/20 re-pareamentos
      em ≤ 10 s**; registrar mediana e p95 contra 1.8 e 2.5.
      **Resultado (2026-09-24 15:25-15:31, `logs/repair-20260924-182542.json`)**:
      **20/20 recuperados**, 19/20 em ≤ 10 s — mediana 7,88 s, p95 9,03 s
      (número-base 1.8: 0/6). Decomposição: ~5,3-5,5 s é a *detecção* da
      perda pelo lado que não foi resetado (`LOSS_HOPS` × timeout de hop),
      0,3-1,8 s é o handshake v2 em si, ~1,1 s até `pair_confirm`. O único
      trial acima de 10 s (#3, 13,0 s): `pair_ok` dos dois lados mas o
      escravo não recebeu o primeiro fragmento → `pair_timeout` → novo
      pareamento automático, confirmado em seguida. Se for preciso baixar
      o tempo total, o próximo alvo é a detecção (`LOSS_HOPS`), não o
      handshake.
- [x] 5.2 Perda injetada (`drop_next_ack`): pareamento conclui pelo re-ack
      em ≤ 2 × `LINK_RETRY_MS` a mais que o normal; primeiro fragmento com
      ack sem `loss_of_sync`.
      **Resultado (15:30)**: ack descartado de propósito (nonce 36522) →
      `pair_reack` **91 ms** depois (≈ 1 × `LINK_RETRY_MS`) → `pair_ok` do
      mestre 50 ms depois → `pair_confirm`, sem `loss_of_sync`.
- [x] 5.3 Reboot assimétrico (desligar uma placa ~5 s) ×5: recupera sem
      intervenção (fecha `adaptive-rendezvous` 6.3/6.5).
      **Resultado (15:31)**: 5/5 recuperados, placa segurada em reset por
      5 s via RTS (não dá pra cortar a alimentação remotamente — o rádio
      continua alimentado, o ESP32 para). Placa B: `pair_confirm` 4,4-4,5 s
      após soltar; placa A: 7,5 s — ambos dominados pelo próprio boot
      (atrasos de `setup()` e janela de seed de 3 s do mestre).
- [x] 5.4 Jammer sweep/targeted ligado **antes** do boot: pareamento
      converge num candidato fora da faixa atacada (fecha
      `adaptive-rendezvous` 6.2).
      **Resultado (15:32-15:34), critério revisado**: na placa B combinada
      o jammer volta desligado a cada boot e só arma depois do primeiro
      handshake, então "antes do boot" não é possível por construção —
      testado o equivalente: sweep 2-80 em PA_LOW ativo, forçando
      re-pareamentos. **O pareamento convergiu em todas as 7 tentativas**
      (re-acks absorvendo acks perdidos), mas quase sempre em ch 76,
      *dentro* da faixa varrida — e a regra D5 corretamente **não** o
      excluiu, porque houve dado de verdade ali (acks/`msg_done` em 5 das
      6 janelas). A premissa do critério original ("candidato dentro da
      faixa varrida é inutilizável") não vale para uma varredura em PA_LOW:
      cada canal só é atingido por instantes a cada ciclo. O enlace então
      durou ~10-20 s antes de cair por perda de hops (`LOSS_HOPS`) — isso é
      desempenho da mitigação sob ataque, validado na fila de 5.5, não do
      pareamento.
      **Achado de telemetria**: o heartbeat `'.'` do jammer (sem newline)
      grudava no início da próxima linha `@escravo;...`, e o logger (regex
      ancorada em `^@`) perdia o evento — 31 linhas perdidas nesta sessão,
      incluindo 10 `pair_confirm` e 8 `loss_of_sync`. Toda medição anterior
      com jammer ligado na placa B pode ter perdido eventos do escravo.
      Corrigido no logger (5.6).
- [x] 5.6 Logger tolerante a `'.'` antes de `@` (heartbeat do jammer).
- [ ] 5.5 Retomar a fila bloqueada, nesta ordem: jammer `adaptive` ao vivo
      (`targeted-jammer` 4.5) → `run_scenario.py --jammer-mode targeted`
      (`dynamic-seed-blacklist` 4.4/4.5) → `blacklist-decay` §4 →
      `run_battery.py` A/B/C (`scenario-automation` 4.2/4.3) →
      `spectrum-attack-classification` 3.1/3.3 → `two-board-topology`
      6.2-6.5.

## 6. Documentação

- [x] 6.1 `adaptive-rendezvous/design.md` e `tasks.md` (§6, §7.5):
      substituir o diagnóstico "interferência de RF ambiente" pelas
      evidências deste change; apontar para cá.
- [ ] 6.2 `openspec/ROADMAP.md` e `openspec/project.md` (ordem de
      dependência): incluir este change como pré-requisito da validação de
      bancada.
- [ ] 6.3 Sincronizar a spec delta com o que foi de fato implementado;
      `openspec validate --changes --strict`; arquivar.
