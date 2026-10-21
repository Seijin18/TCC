## ADDED Requirements

### Requirement: O salto de frequência avança por tentativa, desacoplado do ACK
O firmware DEVE (MUST) avançar o `hop_counter` (que seleciona o canal via
`get_next_active_channel`) a cada TENTATIVA de transmissão de fragmento, tenha
ou não sido confirmada por ACK; e DEVE (MUST) avançar o contador de dados
`fragment_index` SOMENTE quando um ACK é confirmado. Assim mestre e escravo
avançam em lockstep na mesma cadência, sem relógio compartilhado, e um ACK
perdido não deixa o mestre preso num canal antigo.

#### Scenario: ACK perdido não trava o mestre nem dessincroniza
- **WHEN** o mestre envia um fragmento e não recebe o ACK dentro da janela por
  tentativa (`ATTEMPT_TIMEOUT_MS`)
- **THEN** o mestre avança o `hop_counter` (novo canal) e reenvia o MESMO
  fragmento no próximo canal, sem repetir no canal antigo, e sem avançar o
  `fragment_index`

#### Scenario: ACK confirmado avança o dado
- **WHEN** o mestre recebe o ACK do fragmento corrente
- **THEN** o `fragment_index` avança para o próximo fragmento e o `hop_counter`
  avança para o próximo canal (uma tentativa = um hop)

### Requirement: Retransmissão no próximo canal com deduplicação por sequência
Um fragmento não confirmado DEVE (MUST) ser retransmitido no PRÓXIMO canal (onde
o receptor já está), e o receptor DEVE (MUST) deduplicar por
`(msg_id, fragment_index)` — re-ackando a retransmissão sem remontar o fragmento
duas vezes. É o papel do número de sequência (SEQN).

#### Scenario: Retransmissão duplicada é re-ackada mas não reprocessada
- **WHEN** o receptor recebe de novo um fragmento que já havia recebido (uma
  retransmissão, porque o ACK anterior se perdeu)
- **THEN** o receptor responde ACK novamente mas NÃO conta o fragmento duas
  vezes na remontagem da mensagem

### Requirement: Cadência por tentativa em lockstep nos dois lados
O firmware DEVE (MUST) usar a mesma janela curta (`ATTEMPT_TIMEOUT_MS`) para a
escuta por hop do escravo e para a espera de ACK do mestre, de modo que os dois
lados avancem o `hop_counter` aproximadamente juntos. O `hop_counter` carregado
em cada pacote continua sendo a correção fina de sincronismo (o receptor o adota
em toda recepção válida).

#### Scenario: Os dois lados avançam juntos sob um canal jammed
- **WHEN** o canal atual está jammed e nenhum dos lados troca pacote nele
- **THEN** ambos expiram após `ATTEMPT_TIMEOUT_MS` e avançam para o próximo
  canal, permanecendo alinhados (o hop jammed custa uma tentativa curta, não
  uma janela longa nem um estacionamento)

### Requirement: Constantes de contagem de hop preservam os orçamentos de tempo
O firmware DEVE (MUST) re-derivar as constantes expressas em CONTAGEM de hops
(perda de sincronismo, ressincronização, propagação de controle, cooldown de
blacklist, janelas de métricas) para a nova cadência, preservando os mesmos
ORÇAMENTOS DE TEMPO de antes, e DEVE (MUST) manter a invariante de orçamento de
ressincronização verificada em compilação (`static_assert`).

#### Scenario: Orçamento de perda de sincronismo em tempo é preservado
- **WHEN** a cadência de hop muda de até ~220 ms para ~30 ms por hop
- **THEN** `LOSS_HOPS` (e demais limites em hops) são ajustados para que o tempo
  até declarar perda de sincronismo permaneça equivalente ao anterior
