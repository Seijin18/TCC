## 1. Lógica pura

- [ ] 1.1 `shared/time_sync.h`: estado do servo + `phase_servo_update()`
      (filtro EMA, passo de 1 ms, limite de taxa, guardas D5).
- [ ] 1.2 `test/test_time_sync/`: convergência sob ±50 ppm por 30 min
      simulados, passo/taxa limitados, ruído ±2 ms sem correção espúria,
      amostra ≥ meio slot ignorada, desligamento por divergência.

## 2. Protocolo

- [ ] 2.1 `FragmentPacket.slot_phase_ms` + `static_assert` de tamanho ≤ 32.
- [ ] 2.2 Mestre: preencher `slot_phase_ms` imediatamente antes de cada
      `radio_send()` do fragmento.

## 3. Escravo

- [ ] 3.1 A cada `FragmentPacket` válido: calcular `e`, alimentar o servo,
      aplicar o ajuste em `hop_start_at`.
- [ ] 3.2 `e_alvo` a partir das primeiras amostras depois de `time_sync`;
      zerar o estado do servo em `reset_to_scanning()`.
- [ ] 3.3 Telemetria `e=phase;err=..;adj=..;n=..` a cada ~5 s;
      `state=disabled` se a guarda disparar.
- [ ] 3.4 Logger: último `phase` em `state.json`.

## 4. Validação em bancada

- [ ] 4.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [ ] 4.2 Soak de 15 min sem jammer, sem re-pareamento: throughput do mestre
      ≥ 90% do valor inicial durante todo o soak, 0 `loss_of_sync`,
      `|err|` filtrado ≤ 3 ms, nenhum `reanchor`.
- [ ] 4.3 Soak de 15 min com jammer `hold` ch10 `PA_MAX` em hopping: mesmos
      critérios fora do canal 10.
- [ ] 4.4 Repetir a comparação dos 4 modos de jammer **sem** re-parear entre
      eles: os baselines dos 4 devem ficar iguais (prova que o confound de
      uptime sumiu).
- [ ] 4.5 Se qualificar, copiar sessões para `logs/working/`.

## 5. Fechamento

- [ ] 5.1 Arquivar `time-slotted-hopping` antes (ou junto).
- [ ] 5.2 `openspec validate slot-phase-servo --strict` e arquivar.
