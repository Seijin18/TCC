## 1. Extrair cada nó para `node.h` (sem mudar comportamento)

- [x] 1.1 `mestre/src/main.cpp` → mover corpo de `setup()`/`loop()` para
      `mestre/src/node.h` como `mestre_setup()`/`mestre_loop()`; `main.cpp`
      vira `#include "node.h"` + `setup(){ mestre_setup(); }` +
      `loop(){ mestre_loop(); }`.
- [x] 1.2 Mesma extração para `analisador/src/main.cpp` →
      `analisador/src/node.h` (`analisador_setup`/`analisador_loop`).
- [x] 1.3 Mesma extração para `escravo/src/main.cpp` →
      `escravo/src/node.h` (`escravo_setup`/`escravo_loop`).
- [x] 1.4 Mesma extração para `jammer/src/main.cpp` →
      `jammer/src/node.h` (`jammer_setup`/`jammer_loop`).
- [x] 1.5 `pio run -e mestre -e analisador -e escravo -e jammer -e
      validacao` e `pio test -e native_test` continuam passando sem
      diferença de comportamento (regressão da extração).

## 2. Callbacks de alerta (mestre ↔ analisador)

- [x] 2.1 Em `analisador/src/node.h`, adicionar
      `analisador_set_alert_sink(void (*sink)(bool))`; se não configurado,
      manter o `digitalWrite(PIN_ALERT, ...)` atual como default.
- [x] 2.2 Em `mestre/src/node.h`, adicionar
      `mestre_set_alert_source(bool (*source)())`; se não configurado,
      manter o `digitalRead(PIN_ALERT_IN)` atual como default.
- [x] 2.3 Confirmar que `env:mestre`/`env:analisador` standalone (sem
      registrar callback) continuam usando GPIO exatamente como antes.

## 3. Placa A (mestre + analisador)

- [x] 3.1 Criar `placa_a/src/shared_state.h` com `volatile bool
      g_alert_shared` e funções de acesso simples (`set`/`get`).
- [x] 3.2 Criar `placa_a/src/main.cpp`: incluir
      `../../mestre/src/node.h` e `../../analisador/src/node.h`; no
      `setup()`, chamar `mestre_setup()`, registrar
      `mestre_set_alert_source`/`analisador_set_alert_sink` apontando para
      `shared_state.h`, e criar a task do analisador via
      `xTaskCreatePinnedToCore` (core diferente do `loop()` padrão,
      stack ~8192 bytes, prioridade `tskIDLE_PRIORITY + 1`) rodando
      `analisador_setup()` uma vez seguido de `for(;;) { analisador_loop();
      vTaskDelay(1); }`.
- [x] 3.3 `loop()` de `placa_a/src/main.cpp` chama `mestre_loop()`
      (nó primário no `loop()` padrão do Arduino).
- [x] 3.4 Adicionar `[env:placa_a]` a `platformio.ini`
      (`build_src_filter = -<*> +<placa_a/src/>`, `extends =
      esp32_common`).

## 4. Placa B (escravo + jammer)

- [x] 4.1 Criar `placa_b/src/main.cpp`: incluir
      `../../escravo/src/node.h` e `../../jammer/src/node.h`; no
      `setup()`, chamar `escravo_setup()` e criar a task do jammer via
      `xTaskCreatePinnedToCore` (mesmo padrão de stack/prioridade da
      seção 3), rodando `jammer_setup()` seguido de `for(;;) {
      jammer_loop(); vTaskDelay(1); }`.
- [x] 4.2 `loop()` de `placa_b/src/main.cpp` chama `escravo_loop()`
      (nó primário no `loop()` padrão do Arduino).
- [x] 4.3 Adicionar `[env:placa_b]` a `platformio.ini`.
- [x] 4.4 `pio run -e placa_a -e placa_b` compila sem erros.

## 5. Documentação

- [ ] 5.1 Atualizar `docs/pinout.md` (seção "Cenário alternativo: 2
      placas") para apontar para `env:placa_a`/`env:placa_b` como a
      implementação real, removendo a nota de "ainda não implementado".
- [ ] 5.2 Atualizar `openspec/changes/spectrum-attack-classification/design.md`
      (nota de desvio) para referenciar este change como a implementação
      do cenário de 2 placas, em vez de só descrevê-lo como hipotético.
- [ ] 5.3 Atualizar `README.md` (seção de hardware) mencionando a opção de
      2 placas com link para `docs/pinout.md`.

## 6. Validação manual em bancada

- [ ] 6.1 Flashear `placa_a` e `placa_b`, confirmar handshake
      mestre↔escravo funcionando igual ao cenário de 4 placas.
- [ ] 6.2 Confirmar no log serial do mestre que o alerta do analisador
      (via flag compartilhada) chega corretamente ao ligar/desligar o
      jammer em modo targeted.
- [ ] 6.3 Rodar por uma sessão longa (vários minutos) e confirmar que não
      há reset por task watchdog nem crash de stack overflow em nenhuma
      das duas placas.
- [ ] 6.4 Medir PLR com o analisador/jammer da task secundária
      ligado-e-parado vs. ligado-e-rodando, para isolar se alguma queda de
      PLR é causada por contenção de CPU em vez de RF real (ver Risks em
      `design.md`).
- [ ] 6.5 Recalibrar `LIMIAR_ALVO`/`LIMIAR_CONCENTRACAO`/`ATTACK_STREAK_N`
      (`spectrum-attack-classification`) se a proximidade física entre os
      dois módulos da Placa A distorcer a classificação em relação ao
      cenário de 4 placas.
