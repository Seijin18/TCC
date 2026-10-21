# combined-node-runtime Specification

## Purpose
TBD - created by archiving change two-board-topology. Update Purpose after archive.
## Requirements
### Requirement: Reuso de lógica sem duplicação
A implementação combinada de cada placa DEVE (MUST) reutilizar as
funções `setup`/`loop` extraídas de cada nó original (`mestre`, `analisador`,
`escravo`, `jammer`) em vez de duplicar sua lógica, de forma que o
comportamento de cada nó permaneça idêntico ao do seu firmware standalone.

#### Scenario: Comportamento do mestre inalterado na Placa A
- **WHEN** a Placa A (mestre + analisador) está rodando
- **THEN** a sequência de handshake, envio de seed e hopping do mestre
  segue exatamente a mesma lógica usada pelo `env:mestre` standalone

#### Scenario: Comportamento do analisador inalterado na Placa A
- **WHEN** a Placa A está rodando
- **THEN** a varredura de espectro e a classificação de ataque do
  analisador seguem exatamente a mesma lógica usada pelo `env:analisador`
  standalone

### Requirement: Agendamento por duas tasks pinadas a núcleos distintos
Cada placa combinada DEVE (MUST) rodar seu nó primário (mestre na Placa A,
escravo na Placa B) no `loop()` padrão do Arduino e seu nó secundário
(analisador na Placa A, jammer na Placa B) numa task FreeRTOS pinada
explicitamente ao núcleo diferente do usado pelo `loop()` padrão.

#### Scenario: Task secundária não bloqueia o núcleo do nó primário
- **WHEN** a task do nó secundário está em execução
- **THEN** ela cede CPU periodicamente (ex. via `vTaskDelay`) e não impede
  o `loop()` do nó primário de rodar no núcleo dele

### Requirement: Alerta de ataque via flag compartilhada na Placa A
O mestre e o analisador DEVEM (MUST) propagar o alerta de ataque ativo
(`spectrum-attack-classification`) por uma flag compartilhada em memória
entre as duas tasks quando rodam na mesma placa, em vez de um pino GPIO
físico — preservando a mesma semântica observável (nível lógico mantido
enquanto o ataque persistir) descrita no `design.md` daquele change.

#### Scenario: Analisador sinaliza início de ataque à task do mestre
- **WHEN** a task do analisador declara "ataque ativo"
- **THEN** a flag compartilhada é atualizada e a próxima leitura feita pela
  task do mestre observa a transição, sem depender de nenhum GPIO físico

#### Scenario: Firmware standalone continua usando GPIO
- **WHEN** `env:mestre` ou `env:analisador` são compilados e rodados de
  forma standalone (fora da Placa A combinada)
- **THEN** o alerta continua sendo sinalizado/lido via GPIO físico
  (`PIN_ALERT`/`PIN_ALERT_IN`), sem nenhuma mudança de comportamento

