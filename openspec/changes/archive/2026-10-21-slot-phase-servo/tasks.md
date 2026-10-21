## 1. Lógica pura

- [x] 1.1 `shared/time_sync.h`: estado do servo + `phase_servo_update()`
      (filtro EMA, passo de 1 ms, limite de taxa, guardas D5).
- [x] 1.2 `test/test_time_sync/`: convergência sob ±50 ppm por 30 min
      simulados, passo/taxa limitados, ruído ±2 ms sem correção espúria,
      amostra ≥ meio slot ignorada, desligamento por divergência.

## 2. Protocolo

- [x] 2.1 `FragmentPacket.slot_phase_ms` + `static_assert` de tamanho ≤ 32.
- [x] 2.2 Mestre: preencher `slot_phase_ms` imediatamente antes de cada
      `radio_send()` do fragmento.

## 3. Escravo

- [x] 3.1 A cada `FragmentPacket` válido: calcular `e`, alimentar o servo,
      aplicar o ajuste em `hop_start_at`.
- [x] 3.2 `e_alvo` a partir das primeiras amostras depois de `time_sync`;
      zerar o estado do servo em `reset_to_scanning()`.
- [x] 3.3 Telemetria `e=phase;err=..;adj=..;n=..` a cada ~5 s;
      `state=disabled` se a guarda disparar.
- [x] 3.4 Logger: último `phase` em `state.json`.

## 4. Validação em bancada

- [x] 4.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [x] 4.2 Soak de 15 min sem jammer, sem re-pareamento: throughput do mestre
      ≥ 90% do valor inicial durante todo o soak, 0 `loss_of_sync`,
      `|err|` filtrado ≤ 3 ms, nenhum `reanchor`.
      **PASS 2026-10-21** (`session-20261021-101328`, 15 min): fps 98,6-102,1
      (vs 105→40 no firmware antigo), 0 `loss_of_sync`, `err`=0 em todas as
      186 linhas `phase`, 0 `reanchor`, sem reset. adj acumulado +53 ms ≈
      57 ppm (compensa a deriva medida).
- [ ] 4.3 Soak de 15 min com jammer `hold` ch10 `PA_MAX` em hopping: mesmos
      critérios fora do canal 10.
- [ ] 4.4 Repetir a comparação dos 4 modos de jammer **sem** re-parear entre
      eles: os baselines dos 4 devem ficar iguais (prova que o confound de
      uptime sumiu).
- [ ] 4.5 Se qualificar, copiar sessões para `logs/working/`.

## 5. Fechamento

- [ ] 5.1 Arquivar `time-slotted-hopping` antes (ou junto).
- [ ] 5.2 `openspec validate slot-phase-servo --strict` e arquivar.
