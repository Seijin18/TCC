## 1. `jammer/src/main.cpp` — modo targeted

- [x] 1.1 Incluir `shared/hopping.h` e adicionar enum/flag de modo
      (`JAM_MODE_SWEEP` / `JAM_MODE_TARGETED`), default sweep.
- [x] 1.2 Adicionar parser de serial não bloqueante no `loop()` para o
      comando `mode sweep|targeted`, com eco de confirmação e mensagem de
      erro para comando desconhecido.
- [x] 1.3 Implementar avanço de canal restrito a `HOPPING_CHANNELS` quando
      em modo targeted (round-robin com índice em vez de `ch_fwd`/`ch_rev`
      percorrendo `BT_CH_MIN..BT_CH_MAX`), reaproveitando `hopCarrier`.
- [x] 1.4 Dividir os 16 canais entre `radio`/`radio2` quando `has_radio2`
      for verdadeiro, análogo à divisão atual da faixa BT.

## 2. `validacao/src/main.cpp` — modo targeted no harness

- [x] 2.1 Incluir `shared/hopping.h` e adicionar
      `randomTargetedChannel()` (sorteia dentro de `HOPPING_CHANNELS`,
      análoga a `randomBtChannel()`).
- [x] 2.2 Adicionar comando `mode sweep|targeted` a `handleCommand`,
      guardando o modo em uma flag global.
- [x] 2.3 Fazer `stepJammer`/`jammerStart` escolherem `randomBtChannel()`
      ou `randomTargetedChannel()` conforme o modo ativo.
- [x] 2.4 Incluir o modo ativo na saída do comando `status`.

## 3. Validação manual

- [x] 3.1 Compilar os environments `jammer` e `validacao`
      (`platformio.ini`) e confirmar build limpo com o include de
      `shared/hopping.h`.
- [ ] 3.2 Com o `analisador` (ou `tools/spectrum/main.py`) rodando em
      paralelo, confirmar visualmente no heatmap que o modo `targeted`
      concentra energia só nos 16 canais da tabela, diferente do padrão
      disperso do `sweep`.
      **Tentativa 2026-09-24 16:01 (PA_LOW, inconclusiva)**: 30 s base →
      40 s `targeted` → 40 s `sweep`, comparando `logs/spectrum.csv`:
      nos dois modos a atividade subiu ~2,3× por igual (16 canais da
      tabela 1,7 → ~3,8; demais 2-80 4,5 → ~10), e o pico em todas as
      fases, inclusive a base, é ch 38-45 (Wi-Fi real perto da bancada).
      Em PA_LOW, na posição atual das placas, o analisador não separa o
      padrão do jammer do Wi-Fi. Repetir com `power high` (sintaxe correta
      é `power <min|low|high|max>`, não `jam power ...`).
- [x] 3.3 Rodar mestre+escravo com PLR (`plr-metric`) sob jammer targeted e
      confirmar que o PLR sobe de forma comparável ao sweep — serve de
      baseline para o cenário B da Fase 5. **Confirmado em 2026-09-18**
      via `tools/validation/run_scenario.py --jammer-mode targeted`:
      `plr_window` chegou a 12,4% sob targeted + potência alta (vs.
      3,5–5,8% sob sweep no mesmo teste) — mesma ordem de grandeza,
      targeted inclusive mais efetivo por mirar direto nos 16 canais do
      hopping.

## 4. Modo `adaptive` (adicionado 2026-09-24, ver design.md "Implementado depois")

- [x] 4.1 `JAM_MODE_ADAPTIVE` + comando serial `mode adaptive` em
      `jammer/src/node.h`, reconhecido pelo parser existente.
- [x] 4.2 `senseChannel()`: sai de `startConstCarrier()`, sensoria RPD por
      `ADAPTIVE_SENSE_SAMPLES` amostras, volta a emitir no mesmo canal.
- [x] 4.3 `loopAdaptive()`: escaneia os 16 `HOPPING_CHANNELS` procurando
      atividade; trava no primeiro canal ocupado e revalida a cada
      `ADAPTIVE_RECHECK_MS`, soltando e voltando a escanear se ficar
      quieto. Eventos de telemetria `adaptive_lock`/`adaptive_release`.
- [x] 4.4 `pio run -e jammer -e placa_b` limpo.
- [ ] 4.5 Validação em bancada: confirmar `adaptive_lock` disparando no
      canal realmente em uso pelo enlace (não em canais ociosos), e que
      `dynamic-seed-blacklist` ainda consegue escapar dele.
      **Bloqueado em 2026-09-24**: a placa combinada só liga a portadora
      depois que o handshake mestre/escravo confirma pelo menos uma vez
      (`jammer_set_ready_source`) — a sessão encontrou dificuldade
      recorrente de pareamento (ver `adaptive-rendezvous` tasks.md) que
      impediu o modo `adaptive` de chegar a emitir de fato. Confirmado só
      até `mode adaptive` sendo aceito e a telemetria de troca de modo
      disparando; falta o resto assim que o enlace conseguir parear.
