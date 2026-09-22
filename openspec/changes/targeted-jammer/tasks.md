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
- [x] 3.3 Rodar mestre+escravo com PLR (`plr-metric`) sob jammer targeted e
      confirmar que o PLR sobe de forma comparável ao sweep — serve de
      baseline para o cenário B da Fase 5. **Confirmado em 2026-09-18**
      via `tools/validation/run_scenario.py --jammer-mode targeted`:
      `plr_window` chegou a 12,4% sob targeted + potência alta (vs.
      3,5–5,8% sob sweep no mesmo teste) — mesma ordem de grandeza,
      targeted inclusive mais efetivo por mirar direto nos 16 canais do
      hopping.
