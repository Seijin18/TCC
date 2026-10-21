## Why

O projeto assume 4 placas ESP32 físicas separadas (mestre, escravo, jammer,
analisador), mas a bancada disponível tem apenas **2 placas**. `docs/pinout.md`
já documenta um arranjo alternativo (mestre+analisador numa placa, escravo+jammer
noutra) e já lista as implicações, mas deixa explícito que "isto ainda não está
implementado, é o firmware combinado a ser escrito". Sem esse firmware, nenhum
dos changes que dependem de dois nós rodando simultaneamente
(`spectrum-attack-classification`, que já foi implementado assumindo GPIO físico
entre duas placas distintas, e qualquer cenário de `scenario-automation` com
jammer + mestre/escravo) pode ser validado em bancada.

## What Changes

- Cada par de nós compartilhando placa (mestre+analisador na Placa A,
  escravo+jammer na Placa B) passa a rodar como **duas tasks FreeRTOS
  pinadas a núcleos diferentes** (`xTaskCreatePinnedToCore`) em vez de dois
  `main.cpp` independentes — cada nó mantém sua própria lógica de `setup`/
  `loop`, só migrando de "dono exclusivo do `loop()` do Arduino" para "dono
  de uma task num núcleo".
- Dois novos environments/pastas de firmware: `placa_a/` (mestre + analisador)
  e `placa_b/` (escravo + jammer), cada um linkando a lógica existente dos
  `main.cpp` de origem (reaproveitada, não duplicada) como funções chamadas
  pela task de cada núcleo.
- O alerta de ataque (`spectrum-attack-classification`) deixa de ser um GPIO
  físico entre duas placas e passa a ser uma **flag compartilhada
  thread-safe** (ex. `volatile bool` + seção crítica simples, ou
  `xSemaphoreTake/Give`) lida pela task do mestre e escrita pela task do
  analisador, ambas no mesmo processo/placa.
- `jammer` (Placa B) mantém o comando serial `mode sweep|targeted`
  (`targeted-jammer`) e `escravo` mantém sua lógica de PLR (`plr-metric`)
  inalterados — só a forma como cada um é agendado (task vs. `loop()`
  exclusivo) muda.
- Os `main.cpp` originais de `mestre/`, `analisador/`, `escravo/`, `jammer/`
  continuam existindo e compiláveis como estão, para quem tiver o hardware de
  4 placas — este change é aditivo (nova topologia opcional), não substitui a
  topologia de referência.
- **BREAKING** (só para quem usa 2 placas): o wiring de GPIO 4
  analisador→mestre implementado em `spectrum-attack-classification` deixa de
  se aplicar nesse cenário — passa a ser uma dependência lógica interna à
  Placa A, não um fio físico. Quem já cabeou o GPIO 4 entre duas placas
  separadas (cenário de 4 placas) não é afetado.

## Capabilities

### New Capabilities
- `combined-node-runtime`: infraestrutura de duas tasks FreeRTOS pinadas a
  núcleos, cada uma hospedando o `setup`/`loop` de um nó existente
  (mestre+analisador ou escravo+jammer) na mesma placa ESP32, preservando o
  comportamento observável de cada nó individual.

### Modified Capabilities
(nenhuma — `attack-detection`, de `spectrum-attack-classification`, ainda não
foi arquivada como spec principal; a adaptação do seu mecanismo de sinalização
para o cenário de 2 placas é tratada como parte da implementação deste change,
não como uma modificação formal de capability já publicada)

## Impact

- Novo: `placa_a/src/main.cpp`, `placa_b/src/main.cpp` (ou estrutura
  equivalente), novos `[env:placa_a]`/`[env:placa_b]` em `platformio.ini`.
- `mestre/src/main.cpp`, `analisador/src/main.cpp`: extraídos para funções
  reaproveitáveis por uma task (sem duplicar lógica), e o alerta de
  `spectrum-attack-classification` (hoje `PIN_ALERT`/`PIN_ALERT_IN` via GPIO
  4) ganha um caminho alternativo de flag compartilhada usado só pela task
  combinada — o `main.cpp` standalone de cada um continua usando o GPIO físico
  para quem tiver 4 placas.
- `escravo/src/main.cpp`, `jammer/src/main.cpp`: mesma extração para reuso via
  task, sem mudança de comportamento.
- `docs/pinout.md`: seção "Cenário alternativo: 2 placas" passa de descritiva
  para referenciar a implementação real (este change).
- `platformio.ini`: dois novos `[env:...]`, `build_src_filter` cobrindo as
  pastas de origem reaproveitadas além da pasta da placa combinada.
- Hardware: nenhuma peça nova — reorganiza os módulos já existentes nas 2
  placas (VSPI + HSPI por placa, como já documentado em `docs/pinout.md`).
