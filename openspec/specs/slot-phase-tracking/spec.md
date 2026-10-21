# slot-phase-tracking Specification

## Purpose
TBD - created by archiving change slot-phase-servo. Update Purpose after archive.
## Requirements
### Requirement: Fase de slot rastreada continuamente
O escravo DEVE (MUST) corrigir continuamente a fase do seu relógio de slots
em relação à do mestre, a partir de cada fragmento válido recebido, sem
depender de detectar o escorregamento de um slot inteiro.

#### Scenario: Deriva constante entre cristais
- **WHEN** os relógios do mestre e do escravo diferem em ritmo por até
  50 ppm e o enlace troca fragmentos continuamente
- **THEN** o desvio de fase filtrado permanece em no máximo 3 ms e nenhum
  `loss_of_sync` ocorre por deriva

#### Scenario: Throughput estável em operação longa
- **WHEN** o enlace opera por 15 min sem jammer e sem re-pareamento
- **THEN** o throughput medido pelo mestre não cai mais que 10% em relação
  ao valor inicial

### Requirement: Mestre informa a própria fase no slot
O mestre DEVE (MUST) incluir em cada `FragmentPacket` a sua posição dentro
do slot corrente, em milissegundos, no instante do envio.

#### Scenario: Fragmento carrega a fase do mestre
- **WHEN** o mestre transmite um fragmento
- **THEN** `slot_phase_ms` contém o tempo decorrido desde o início do slot
  corrente do mestre, entre 0 e `SLOT_MS − 1`

### Requirement: Correção limitada e observável
Cada correção de fase DEVE (MUST) ser limitada em tamanho e frequência e
ser registrada em telemetria periódica, e o servo DEVE se desligar se o
desvio divergir apesar das correções.

#### Scenario: Passo limitado
- **WHEN** o desvio filtrado excede o limiar de correção
- **THEN** a âncora é ajustada em no máximo 1 ms, e não mais de uma vez por
  intervalo mínimo

#### Scenario: Divergência desliga o servo
- **WHEN** o desvio filtrado continua crescendo além de 10 ms apesar das
  correções
- **THEN** o escravo desliga o servo, emite `e=phase;state=disabled` e
  volta ao comportamento de reancoragem por slot inteiro

