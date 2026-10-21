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
      **Repetido 2026-09-25 00:17 com `power high`**: nenhuma mudança
      entre base, `targeted` e `sweep` (16 canais 3,3/3,3/3,1; demais 2-80
      4,5/4,5/4,9). Comparando com as fases de jamming de 2026-09-23
      (`scenario-20260923-042225`/`-045128`) no mesmo `spectrum.csv`: o
      "rastro" do jammer visto pelo analisador era um bloco fixo de ~6
      canais em ch 36-44 (~70%), não a banda 2-80 nem os 16 canais da
      tabela — e o teste de 16:01 mostrou o mesmo bloco (38-45) tanto em
      `targeted` quanto em `sweep`. **Hipótese em aberto (validade do
      teste)**: `hopCarrier()` só chama `setChannel()` com o modo de
      portadora constante ativo (`CONT_WAVE`+`PLL_LOCK`, CE alto); nestes
      clones isso pode não re-sintonizar, e o jammer ficaria numa
      frequência fixa independentemente do modo (`senseChannel()` do modo
      `adaptive` é o único caminho que faz stop → setChannel → start).
      Teste direto proposto: pôr o analisador (placa A) colado no jammer e
      comparar `sweep` contra uma versão de `hopCarrier()` que faz
      `stopConstCarrier()`/`startConstCarrier(power, ch)` a cada troca.
      **Hipótese refutada pelo lado do enlace (2026-09-25 01:08-01:20)**:
      o perfil de miss por canal do mestre é bem diferente entre modos —
      `targeted` derruba 70-100% nos canais baixos da tabela (15-82) e
      10-46% nos altos; `sweep` 44-100% em 10-82 e ainda 29-63% em 94-125
      (fora da faixa varrida: dessensibilização de campo próximo, jammer a
      centímetros do escravo na mesma placa). Logo o jammer re-sintoniza;
      o "bloco fixo" visto pelo analisador era distância/Wi-Fi. Continua
      em aberto só a verificação visual pedida aqui (energia concentrada
      nos 16 canais): exige o analisador perto do jammer — **precisa mover
      placas (intervenção manual)**.
      **Retune determinístico (CE baixo → RF_CH → CE alto) em
      hopCarrier() após resultados não reprodutíveis do targeted em
      2026-09-25; revalidar reprodutibilidade com duas baterias
      seguidas.** Resultado (`logs/battery-20260925-045843`/`-050631`):
      B e C reprodutíveis entre as duas rodadas (7,2/6,8% e 5,6/5,2%), mas
      com perfil por canal quase igual ao sem jammer — o `targeted` em
      PA_LOW, na posição atual, quase não afeta o enlace em salto; o
      cenário A (canal fixo 10) continua não reprodutível (27,7% com 1
      queda vs 0,0%). Sem enxergar a emissão real do jammer não dá para
      separar "jammer fraco" de "jammer errático" — **precisa do analisador
      ao lado do jammer (mover placas)**.
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
- [x] 4.5 Validação em bancada: confirmar `adaptive_lock` disparando no
      canal realmente em uso pelo enlace (não em canais ociosos), e que
      `dynamic-seed-blacklist` ainda consegue escapar dele.
      **Resultado (2026-09-25 01:14-01:17, com `hop-resync`)**: o
      `adaptive_lock` dispara só em canais da tabela (todos usados pelo
      enlace), mas trava principalmente em **ch 27 (93 de ~400 travas)** —
      o canal colado no Wi-Fi real da bancada: o sensoriamento por energia
      é enganado pelo ruído ambiente e não distingue o enlace do Wi-Fi.
      Cada trava dura ~150-200 ms (o enlace já saltou), então na prática o
      modo age como um seguidor reativo. Efeito no enlace: miss 20,8%
      (vs 9,5% sem jammer), 3 quedas em 3 min, 26/30 ressincronizações —
      o enlace escapa sozinho na maior parte das vezes (salto +
      `hop-resync`); a blacklist aplicou 19 máscaras, sem reseed (o
      analisador não alertou — ver `spectrum-attack-classification`).
      Critério atendido com a ressalva do Wi-Fi.
      **Bloqueado em 2026-09-24**: a placa combinada só liga a portadora
      depois que o handshake mestre/escravo confirma pelo menos uma vez
      (`jammer_set_ready_source`) — a sessão encontrou dificuldade
      recorrente de pareamento (ver `adaptive-rendezvous` tasks.md) que
      impediu o modo `adaptive` de chegar a emitir de fato. Confirmado só
      até `mode adaptive` sendo aceito e a telemetria de troca de modo
      disparando; falta o resto assim que o enlace conseguir parear.
