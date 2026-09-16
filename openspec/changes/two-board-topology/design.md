## Context

Bancada real: **2 placas ESP32**, não as 4 do cenário de referência
(`README.md`). `docs/pinout.md` já define o agrupamento (Placa A: mestre +
analisador; Placa B: escravo + jammer) e por quê (o analisador só existe para
alertar o mestre; o escravo só segue o mestre e tolera dividir CPU com o
jammer). Cada `main.cpp` atual (`mestre/`, `analisador/`, `escravo/`,
`jammer/`) assume que é o único ocupante do `setup()`/`loop()` do Arduino —
nenhum foi escrito para dividir a CPU com outro nó.

`spectrum-attack-classification` já foi implementado assumindo 4 placas: o
alerta do analisador ao mestre é hoje um GPIO físico (`PIN_ALERT`/
`PIN_ALERT_IN`, ambos GPIO 4) entre duas placas separadas. Numa mesma placa
isso não faz sentido — vira comunicação entre duas tasks do mesmo processo.

## Goals / Non-Goals

**Goals:**
- Rodar mestre+analisador numa placa e escravo+jammer noutra, cada nó
  preservando seu comportamento observável atual (mesmo protocolo, mesmos
  logs, mesma lógica de PLR/hopping/jamming/classificação).
- Reaproveitar o código-fonte de cada nó sem duplicá-lo — a lógica de
  `mestre/src/main.cpp` deve continuar sendo a única fonte de verdade do
  comportamento do mestre, usada tanto no `env:mestre` standalone (4 placas)
  quanto no `env:placa_a` combinado (2 placas).
- Substituir o GPIO de alerta por uma flag compartilhada **só** no cenário
  combinado, sem alterar o comportamento do `env:analisador`/`env:mestre`
  standalone (que continuam usando o GPIO físico, para quem tiver 4 placas).
- Não travar o watchdog do FreeRTOS em nenhuma das duas tasks (cada uma deve
  ceder CPU periodicamente).

**Non-Goals:**
- Não otimiza os riscos de RF/alimentação já documentados em
  `docs/pinout.md` (acoplamento entre módulos vizinhos, picos de corrente) —
  esse change só entrega o firmware; a mitigação física (blindagem, fonte
  externa) é responsabilidade de quem monta a bancada.
- Não muda a lógica interna de nenhum nó (PLR, hopping, jamming, classificação
  de ataque) — é só uma mudança de *como* cada um é agendado na CPU e como o
  alerta trafega entre mestre/analisador.
- Não remove nem descontinua os `main.cpp`/environments standalone de 4
  placas — este change é aditivo.

## Decisions

- **Extrair cada nó para `<node>/src/node.h` com `<node>_setup()`/
  `<node>_loop()`** (ex.: `mestre_setup()`/`mestre_loop()` em
  `mestre/src/node.h`), mantendo toda a lógica/estado atual (as variáveis
  `static`/globais de cada `main.cpp` continuam como estão, só dentro do
  header). O `<node>/src/main.cpp` de cada nó vira um arquivo fino:
  ```cpp
  #include "node.h"
  void setup() { <node>_setup(); }
  void loop()  { <node>_loop(); }
  ```
  Isso preserva 100% do comportamento e do build standalone (`env:mestre`,
  `env:analisador`, `env:escravo`, `env:jammer` continuam compilando exatamente
  a mesma lógica), e permite que `placa_a`/`placa_b` chamem as mesmas funções
  sem copiar código. Alternativa descartada: duplicar a lógica dentro de
  `placa_a/src/main.cpp` — rejeitada por violar "fonte única de verdade" e
  criar risco de as duas cópias divergirem ao longo do TCC.
- **Um nó por placa continua no `loop()` padrão do Arduino (core 1, pinado
  pelo próprio framework); o outro nó vira uma task FreeRTOS pinada ao core
  0**, criada dentro do `setup()` combinado via
  `xTaskCreatePinnedToCore(..., 0)`. Placa A: mestre no `loop()` padrão
  (sensível a timing, ver `docs/pinout.md`), analisador na task do core 0.
  Placa B: escravo no `loop()` padrão, jammer na task do core 0. Alternativa
  descartada: duas tasks explícitas pinadas (nenhuma usando o `loop()`
  padrão) — mais simétrico, mas exige desabilitar a `loopTask` default do
  Arduino (`vTaskDelete`) e não traz benefício real já que o `loop()` padrão
  já roda pinado ao core 1.
- **Task secundária estruturada como `for(;;) { <node>_loop(); vTaskDelay(1);
  }`**, chamando o `<node>_setup()` uma única vez antes do loop. O
  `vTaskDelay(1)` (1 tick, ~1ms em configTICK_RATE_HZ padrão) cede CPU para o
  scheduler e evita disparar o task watchdog do FreeRTOS — nenhum `<node>_loop()`
  atual bloqueia por mais que alguns ms de uma vez, então 1 tick de folga por
  iteração é suficiente sem descaracterizar o timing de cada nó.
- **Alerta do analisador ao mestre via flag compartilhada `volatile bool
  g_alert_shared`** (namespace/arquivo novo, `placa_a/src/shared_state.h`),
  protegida por escrita/leitura atômica de `bool` (não precisa mutex — um
  único escritor, um único leitor, tipo primitivo, ambos os núcleos do Xtensa
  fazem load/store atômico de 1 byte). O `node.h` de cada nó ganha um ponto de
  extensão:
  - `analisador/src/node.h`: função de callback opcional
    `analisador_set_alert_sink(void (*sink)(bool))`; se não configurado,
    comportamento padrão (`digitalWrite(PIN_ALERT, ...)`) é preservado.
  - `mestre/src/node.h`: função de callback opcional
    `mestre_set_alert_source(bool (*source)())`; se não configurado,
    comportamento padrão (`digitalRead(PIN_ALERT_IN)`) é preservado.
  `placa_a/src/main.cpp` registra os dois callbacks apontando para
  `g_alert_shared` antes de iniciar as tasks. Alternativa descartada: sempre
  usar GPIO, ligando fisicamente GPIO4↔GPIO4 na mesma placa com um jumper —
  funcionaria, mas desperdiça um pino e reintroduz uma dependência de hardware
  que a flag compartilhada elimina de graça dentro da mesma placa.
- **Novos environments `[env:placa_a]`/`[env:placa_b]`** em `platformio.ini`,
  seguindo o padrão já usado (`build_src_filter = -<*> +<placa_a/src/>`).
  `placa_a/src/main.cpp` inclui `../../mestre/src/node.h` e
  `../../analisador/src/node.h` (mesmo padrão relativo de include já usado por
  `mestre/src/main.cpp` para `shared/hopping.h`); idem para `placa_b/` com
  `escravo/` e `jammer/`.
- **Pinagem física reaproveita exatamente a tabela já em `docs/pinout.md`**
  (VSPI para o nó primário, HSPI para o nó secundário) — nenhum pino novo,
  esta seção do documento passa a apontar para a implementação real em vez de
  ser só descritiva.

## Risks / Trade-offs

- [Contenção de CPU atrasando o timing do mestre] → mitigado por manter o
  mestre no `loop()` padrão (prioridade implícita do Arduino) e o analisador
  numa task de prioridade igual ou menor (`tskIDLE_PRIORITY + 1`) — se ainda
  assim distorcer o PLR medido, é um risco já assumido e documentado em
  `docs/pinout.md` (comparar PLR com o nó secundário ligado-e-parado vs.
  ligado-e-rodando antes de atribuir queda ao jammer/ataque).
- [Acoplamento de RF entre os dois módulos da mesma placa] → sem mitigação de
  firmware; documentado como limitação física em `docs/pinout.md`
  (distância/blindagem física é responsabilidade da montagem).
- [Divergência entre `node.h` e o `main.cpp` standalone antigo durante a
  refatoração] → mitigado pela extração ser mecânica (renomear `setup`→
  `<node>_setup`, `loop`→`<node>_loop`, mover para header, `main.cpp` vira
  1-liner) e por `pio run` continuar cobrindo os 4 environments standalone
  como teste de regressão (mesmo binário lógico, só reorganizado).
- [Task watchdog do FreeRTOS disparando na task secundária] → mitigado pelo
  `vTaskDelay(1)` a cada iteração do loop secundário (ver Decisions);
  validar em bancada que nenhum warning de watchdog aparece no log serial
  durante uma sessão longa.
- [Stack overflow na task secundária] → `xTaskCreatePinnedToCore` recebe um
  tamanho de stack explícito (ex. 8192 bytes, generoso para o que
  `analisador_loop`/`jammer_loop` usam hoje — sem recursão, buffers pequenos);
  validar em bancada não há crash/reset silencioso.

## Migration Plan

1. Extrair `node.h` de cada um dos 4 nós, mantendo `env:mestre`,
   `env:analisador`, `env:escravo`, `env:jammer` buildando sem mudança de
   comportamento (regressão via `pio run` nos 4 environments existentes +
   `pio test -e native_test`).
2. Adicionar os callbacks opcionais de alerta em `mestre/node.h`/
   `analisador/node.h` (default = comportamento GPIO atual, sem quebrar o
   standalone).
3. Criar `placa_a/`, `placa_b/` e os dois novos `[env:...]`.
4. Validar em bancada (fora do escopo de código): flashear as 2 placas,
   confirmar handshake mestre↔escravo, alerta do analisador chegando ao
   mestre via flag, jammer alternando sweep/targeted, sem watchdog reset.

Rollback: os environments/pastas standalone não são tocados em
comportamento, então reverter é só não usar `env:placa_a`/`env:placa_b` — o
cenário de 4 placas nunca fica quebrado por este change.

## Open Questions

- Prioridade relativa das duas tasks por placa (`tskIDLE_PRIORITY + 1` vs.
  `+ 2` para o nó secundário) — decidir empiricamente em bancada observando
  se o PLR do mestre degrada com o analisador/jammer rodando vs. parado
  (task 4.x do `tasks.md`).
- Se `ATTACK_STREAK_N`/limiares calibrados em `spectrum-attack-classification`
  (pensados para 4 placas, sem acoplamento de RF entre mestre e analisador)
  precisam de recalibração no cenário de 2 placas, dado o aviso de possível
  saturação do receptor do analisador pela proximidade com o mestre
  (`docs/pinout.md`) — só decidível em bancada.
