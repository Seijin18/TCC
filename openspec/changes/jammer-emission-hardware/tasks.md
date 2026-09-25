## 1. Baseline de emissão pelo caminho confiável (co-locado, sem Placa A)

- [ ] 1.1 Parar o logger; reflashar uma placa com `env:validacao`
      (`.venv/bin/pio run -e validacao -t upload`), que roda jammer VSPI +
      analisador HSPI no mesmo ESP32 (`validacao/src/main.cpp`). Subir o logger.
- [ ] 1.2 Rodar o harness: `jammer off` (baseline de ruído) por ~30 s, depois
      `jammer on` (CW PA_MAX) por ~60 s, comparando o salto de atividade no CSV.
      Registrar a magnitude do salto ATUAL (antes de qualquer fix) — este é o
      número de referência de emissão.
- [ ] 1.3 Registrar também, via o firmware de produção (placa_b): telemetria
      `RF_SETUP` **durante** `jam on` a `power max` (não só o boot) — confirmar
      RF_PWR/bit extra de potência. Se não houver evento, adicionar um
      `tlm_radio_regs("jammer", ..., "jam_on")` logo após `startCarrier()`.

## 2. Modo de canal fixo (`hold`) + robustez de emissão (firmware)

- [ ] 2.1 `JAM_MODE_HOLD` + `loopHold()` em `jammer/src/node.h`: portadora
      parada no canal `g_hold_channel` (default 125), sem `hopCarrier`.
- [ ] 2.2 Comando serial `mode hold` e `mode hold <ch>` no parser do jammer;
      telemetria `jam_mode_hold`. Não quebrar `jam on|off`/`power` nem
      `run_scenario.py`/`run_battery.py`.
- [ ] 2.3 Re-arme anti drop-out do clone: dentro do loop de emissão, reescrever
      periodicamente `RF_SETUP`+CONT_WAVE+PLL_LOCK (técnica "overdrive" do
      Jammox) para clones Si24R1 que caem fora da portadora constante.
- [ ] 2.4 Garantir potência máxima real: confirmar (via 1.3) que `RF_SETUP` sob
      `jam on` está no nível máximo com o bit extra do Si24R1; ajustar a
      configuração se a lib não o estiver setando.
- [ ] 2.5 `.venv/bin/pio run` limpo (todos os envs). Parar o logger antes de
      qualquer upload.

## 3. Fixes de hardware (manual — em ordem de custo/probabilidade)

- [ ] 3.1 **Alimentação** (primeira hipótese): dar ao módulo PA+LNA do jammer
      uma fonte 3V3 que sustente ~115 mA de pico — capacitor de bulk
      (100-470 µF) junto ao módulo e/ou regulador dedicado. Consertar a
      alimentação compartilhada da Placa A (dois rádios no mesmo 3V3, aviso em
      `docs/pinout.md`). Registrar no `docs/pinout.md` a solução aplicada.
- [ ] 3.2 Re-rodar a Tarefa 1 após 3.1: a magnitude do salto de emissão
      aumentou? O analisador da Placa A voltou a ler algo?
- [ ] 3.3 Se ainda fraco: medir com SDR/analisador de espectro externo a
      frequência do canal em `hold` (ex. ch 125 = 2525 MHz) com `power max` —
      confirma emissão real e sua potência de forma independente do nRF24.
- [ ] 3.4 Se confirmado defeito: trocar o módulo nRF24 do jammer por um bom e
      repetir a Tarefa 1.

## 4. Validação de EFICÁCIA de jamming (negação de comunicação — o objetivo)

> Esta é a validação central. Presença de RF (Tarefas 1-3) não basta.

- [ ] 4.1 Em `tools/validation/emission_check.py`: opção `--mode hold --ch <n>`
      na fase ON, e uma variante que **prende o enlace no mesmo canal do
      hold** (`placa_a mode fixed`/`placa_b mode fixed` no canal atacado) para
      medir negação, além da variante de banda limpa (link em ch 10).
- [ ] 4.2 Rodar o ataque de canal coincidente: enlace preso no canal X, jammer
      em `hold` no canal X a `power max`. **Critério de sucesso**: escravo
      co-locado registra `loss_of_sync` e/ou o PLR/`miss` sobe claramente vs a
      fase OFF — o ataque NEGA comunicação. Registrar os números.
- [ ] 4.3 Se 4.2 não negar mesmo com a emissão já fortalecida (Tarefas 2-3),
      trocar a CW estreita por **inundação de payload** no canal alvo
      (payloads fixos contínuos, auto-ack off) e repetir 4.2 — sinal modulado
      real nega recepção melhor que CW estreita.

## 5. Fechar o pipeline de mitigação sob ataque real

- [ ] 5.1 Com o ataque negando de fato (4.2/4.3 OK), rodar
      `run_scenario.py --jammer-mode targeted` (ou `hold`) com mitigação ON:
      confirmar alerta→blacklist/reseed→**recuperação** (enlace volta, PLR cai)
      nos dois lados. Isto encerra a ambiguidade de toda a validação sob ataque.
- [ ] 5.2 Retomar a fila desbloqueada: `targeted-jammer` 3.2,
      `spectrum-attack-classification` 3.1/3.3, `two-board-topology` 6.2,
      `dynamic-seed-blacklist` 4.5, bateria A/B/C (`scenario-automation`
      4.2/4.3 — o modo `hold` é o atacante do Cenário A). Se qualificar, copiar
      a sessão para `logs/working/` conforme `CLAUDE.md`.

## 6. Fechamento

- [ ] 6.1 Registrar resultado final no `openspec/ROADMAP.md` (resolver a
      pendência #1) e atualizar a memória `project-rf-status`.
- [ ] 6.2 `openspec validate jammer-emission-hardware --strict` e arquivar.

---

### Resultado (preencher ao rodar)

- Emissão baseline (validacao/, salto de atividade) antes dos fixes:
- RF_SETUP sob jam on (PA_MAX):
- Fix de alimentação aplicado / efeito:
- Fixes de firmware aplicados (re-arme CONT_WAVE / payload-flood):
- Eficácia de jamming (loss_of_sync / PLR com link no canal atacado):
- Pipeline de mitigação sob ataque real (recuperação confirmada?):
- Módulo trocado? (s/n):
