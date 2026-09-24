## ADDED Requirements

### Requirement: Escravo recupera sincronismo de salto sem re-parear
O escravo DEVE (MUST), depois de um número definido de hops consecutivos
sem receber nenhum pacote válido, parar de avançar de canal por conta
própria e escutar continuamente no último canal em que recebeu um
fragmento válido, até receber um fragmento do mestre — adotando então o
contador de hop desse fragmento e voltando a saltar normalmente — ou até
um limite de tempo que termina antes do mestre declarar perda de
sincronismo.

#### Scenario: Ack perdido deixa o escravo adiantado
- **WHEN** o escravo recebe um fragmento e avança, mas o ack se perde e o
  mestre continua no canal anterior até o fim da janela do hop, deixando
  os dois lados em canais diferentes nos hops seguintes
- **THEN** após os hops sem recepção o escravo estaciona no último canal
  bom, recebe o fragmento do mestre quando este passa por aquele canal, e
  o enlace continua sem `loss_of_sync` nem re-pareamento

#### Scenario: Estacionamento sem sucesso cai no caminho de perda
- **WHEN** o escravo está estacionado e nenhum fragmento válido chega
  dentro do limite de estacionamento
- **THEN** o escravo volta ao estado de pareamento pelo mesmo caminho de
  perda de sincronismo já existente

### Requirement: Todo pacote do mestre durante o salto carrega o contador de hop
Todo pacote que o mestre envia durante o salto — fragmento de dado ou
pacote de controle — DEVE (MUST) carregar o contador de hop corrente do
mestre, e o escravo DEVE adotá-lo ao receber qualquer um deles.

#### Scenario: Controle pendente durante a ressincronização
- **WHEN** o escravo está estacionado e o mestre está enviando um pacote
  de controle no lugar do fragmento
- **THEN** o escravo, ao receber o pacote de controle, adota o contador
  de hop dele e sai do estacionamento

### Requirement: Orçamento de ressincronização verificado em compilação
O tempo máximo de ressincronização do escravo (hops até estacionar mais o
limite de estacionamento) DEVE (MUST) ser menor que o tempo que o mestre
leva para declarar perda de sincronismo, com essa relação verificada em
tempo de compilação.

#### Scenario: Constante que estouraria o orçamento
- **WHEN** uma constante de temporização é alterada de forma que a
  ressincronização do escravo possa durar mais que a tolerância de perda
  do mestre
- **THEN** o firmware não compila
