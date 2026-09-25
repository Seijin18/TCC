## 1. Modo de jammer em canal fixo (firmware)

- [ ] 1.1 Em `jammer/src/node.h`: novo `JAM_MODE_HOLD` no enum de modos e um
      `loopHold()` que mantém a portadora constante parada no canal escolhido
      (sem chamar `hopCarrier`), preservando o heartbeat `.`.
- [ ] 1.2 Canal do hold: variável `g_hold_channel` (default 125, um canal alto
      da tabela). Ao entrar em `hold`, iniciar a portadora nesse canal
      (`startConstCarrier`/`startCarrierOn`).
- [ ] 1.3 Parser serial: aceitar `mode hold` (canal default) e `mode hold <ch>`
      no `handleCommand` do jammer, junto de `sweep|targeted|adaptive`. Emitir
      telemetria `jam_mode_hold` (espelhando os outros `jam_mode_*`).
- [ ] 1.4 Não quebrar a interface existente (`jam on|off`, `power`,
      `jam_state_on/off`) nem `run_scenario.py`/`run_battery.py`.
- [ ] 1.5 `.venv/bin/pio run -e jammer -e placa_b` limpo (parar o logger antes
      de qualquer upload — ele é dono das seriais).

## 2. Suporte ao hold no runner de emissão

- [ ] 2.1 Em `tools/validation/emission_check.py`: opção `--mode {targeted,hold}`
      (default `hold` para este reteste) e `--ch <n>` (default 125) na fase ON,
      trocando o comando `mode targeted` por `mode hold <ch>`.
- [ ] 2.2 Opção para pôr o enlace em `mode fixed` no **mesmo** canal do hold
      (teste mais duro de dessensibilização do co-locado) — ou manter em ch 10
      (teste de banda limpa). Documentar as duas variantes no `--help`.

## 3. Reteste de emissão com portadora parada (bancada)

- [ ] 3.1 Parar o logger, flashear placa_b com o firmware do modo hold, subir o
      logger. Confirmar re-pareamento.
- [ ] 3.2 Rodar `emission_check.py run --mode hold --ch 125 --phase-s 120`:
      banda limpa (link em ch 10) — o analisador vê o canal 125 subir com o
      jammer ON?
- [ ] 3.3 Rodar a variante de canal coincidente (link `mode fixed` em 125,
      jammer hold em 125): o escravo co-locado cai (`loss_of_sync`)?
- [ ] 3.4 Registrar a tabela por-canal + veredito. Aplicar o ramo de decisão
      do `design.md`.

## 4. Diagnóstico de hardware (manual — CONDICIONAL a 3.x = não irradia)

> Só se 3.2 E 3.3 continuarem "não irradia" mesmo com a portadora parada.

- [ ] 4.1 Inspecionar antena/conexão do módulo nRF24 do jammer (solda, cabo,
      conector u.FL/antena PCB), comparar visualmente com os módulos de
      mestre/escravo que funcionam.
- [ ] 4.2 Medir 2525 MHz (= 2400 + ch 125) com SDR/analisador de espectro
      externo, com o jammer em `hold`+`power max` — confirma emissão real ou
      ausência de forma independente do nRF24 do analisador.
- [ ] 4.3 Testar outro canal (ex. hold em ch 2) e comparar; conferir PA real
      vs `RF_SETUP` reportado.
- [ ] 4.4 Se confirmado morto: trocar o módulo nRF24 do jammer por um bom e
      repetir a Tarefa 3.

## 5. Fechamento

- [ ] 5.1 Registrar o resultado final no `openspec/ROADMAP.md` (resolver a
      pendência #1 de vez) e atualizar a memória `project-rf-status`.
- [ ] 5.2 Se o jammer passa a emitir (via hold e/ou troca de rádio): o modo
      hold vira o atacante do Cenário A — atualizar `scenario-automation`
      4.2/4.3 e retomar a fila de validação sob ataque. Se qualificar, copiar
      a sessão para `logs/working/` conforme `CLAUDE.md`.
- [ ] 5.3 `openspec validate jammer-emission-hardware --strict` e arquivar a
      change.

---

### Resultado (preencher ao rodar)

- Reteste hold @125 (banda limpa), OFF vs ON:
- Reteste hold @125 (link fixo no mesmo canal), loss_of_sync:
- Veredito:
- Diagnóstico de hardware (se rodou):
- Rádio trocado? (s/n):
