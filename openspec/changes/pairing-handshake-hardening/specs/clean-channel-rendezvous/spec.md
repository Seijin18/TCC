## ADDED Requirements

### Requirement: Estado de rádio conhecido ao entrar em pareamento
Mestre e escravo DEVEM (MUST) reinicializar o rádio para a mesma
configuração do boot e descartar qualquer pacote pendente na FIFO de
recepção sempre que entrarem no estado de pareamento, e DEVEM descartar
pacotes pendentes a cada troca de canal durante o pareamento.

#### Scenario: Re-pareamento após perda de enlace
- **WHEN** um nó volta ao estado de pareamento depois de ter operado no
  estado de salto (perda de sincronismo, timeout de handshake ou comando
  `reset`)
- **THEN** o rádio desse nó é reconfigurado como no boot, a FIFO de
  recepção é esvaziada, e o nó emite `e=regs` com o estado resultante

#### Scenario: Pacote antigo não atravessa troca de canal
- **WHEN** um pacote chegou à FIFO de recepção enquanto o nó escutava um
  canal de pareamento e o nó troca para outro canal
- **THEN** esse pacote é descartado e nunca é interpretado como resposta a
  uma tentativa no novo canal

### Requirement: Handshake de pareamento identificado por sessão e idempotente
O mestre DEVE (MUST) identificar cada tentativa de pareamento com um nonce
novo, transportado junto com a seed na sonda, e DEVE aceitar somente a
resposta que ecoe o nonce e a seed da tentativa corrente. O escravo DEVE
responder a toda sonda com o nonce da sessão que adotou, inclusive depois
de já ter respondido, até que o enlace seja confirmado pelo primeiro
fragmento de dado válido.

#### Scenario: Resposta perdida é recuperada por re-ack
- **WHEN** o escravo responde a uma sonda e essa resposta não chega ao
  mestre
- **THEN** o mestre reenvia a sonda com o mesmo nonce, o escravo responde
  de novo, e o pareamento conclui sem que nenhum dos lados passe por um
  timeout de sincronismo

#### Scenario: Resposta que não pertence à tentativa corrente é rejeitada
- **WHEN** o mestre recebe uma resposta cujo nonce, seed ou versão de
  protocolo difere dos da tentativa corrente
- **THEN** a resposta é descartada, contada como `stale_ack` em
  `e=pair_stats`, e o mestre continua sondando

#### Scenario: Mestre recomeça o pareamento
- **WHEN** o escravo está pareado-provisório numa sessão e recebe uma
  sonda válida com nonce diferente
- **THEN** o escravo adota a nova sessão (nonce e seed) e responde a ela

#### Scenario: Enlace provisório sem dados volta à varredura
- **WHEN** o escravo está pareado-provisório e nenhum fragmento de dado
  válido chega dentro do timeout de confirmação
- **THEN** o escravo volta a varrer os candidatos de pareamento

### Requirement: Cobertura temporal garantida do encontro
A temporização do pareamento DEVE (MUST) garantir que cada janela de
escuta do escravo contenha pelo menos duas sondas do mestre e que cada
acampamento do mestre dure pelo menos dois ciclos completos de varredura
do escravo, com essas duas invariantes verificadas em tempo de compilação.

#### Scenario: Mudança de constante que quebraria o encontro
- **WHEN** uma constante de temporização de pareamento é alterada de forma
  que uma das duas invariantes deixa de valer
- **THEN** o firmware não compila

### Requirement: Candidato de pareamento só é penalizado com evidência de canal ruim
O mestre DEVE (MUST) contar uma falha contra um candidato de pareamento
somente quando o escravo respondeu naquele candidato (resposta válida da
tentativa corrente) e o enlace não chegou a confirmar tráfego de dados; um
acampamento sem nenhuma resposta NÃO DEVE contar como falha do candidato.

#### Scenario: Escravo ausente não esvazia a lista de candidatos
- **WHEN** o escravo está desligado ou reiniciando e o mestre acampa em
  vários candidatos sem receber resposta nenhuma
- **THEN** nenhum candidato é excluído por isso, e `pair_mask` não muda

#### Scenario: Canal que aceita o handshake mas não sustenta dados
- **WHEN** o escravo responde à sonda num candidato mas o enlace perde o
  sincronismo antes de confirmar tráfego de dados, repetidamente, até o
  limiar de falhas
- **THEN** o candidato é excluído temporariamente pelo mesmo mecanismo de
  quarentena já existente
