# time-slotted-hopping Specification

## Purpose
TBD - created by archiving change time-slotted-hopping. Update Purpose after archive.
## Requirements
### Requirement: O salto de frequência avança por tempo decorrido desde uma âncora comum
O firmware DEVE (MUST) derivar o número do slot de hopping do TEMPO decorrido
desde uma âncora comum estabelecida no pareamento — `slot = (millis() −
hop_start_at) / SLOT_MS` — nos DOIS lados, e DEVE (MUST) selecionar o canal por
`get_next_active_channel(seed, slot, mask)`. Assim mestre e escravo estão no
mesmo canal por construção, tenha ou não passado pacote, sem depender de eventos
de ack/RX para avançar o salto.

#### Scenario: Um slot jammed não desalinha os dois lados
- **WHEN** o canal do slot corrente está jammed e nenhum pacote atravessa
- **THEN** os dois lados avançam para o próximo slot no mesmo instante de tempo
  (fronteira `SLOT_MS`) e caem no mesmo próximo canal, sem park nem resync

#### Scenario: O canal é função só do tempo e da seed/máscara
- **WHEN** os dois lados têm a mesma seed, a mesma máscara e âncoras `T0`
  alinhadas dentro da guarda do slot
- **THEN** ambos calculam o mesmo canal para o mesmo instante, sem troca de
  mensagem adicional

### Requirement: O escravo reancora o relógio a partir do hop_counter recebido
O firmware do escravo DEVE (MUST) corrigir sua âncora `hop_start_at` quando o
slot local escorregar em relação ao `hop_counter` que chega no pacote do mestre
(|slot_local − hop_counter| ≥ 1), de modo que a deriva de cristal entre os dois
relógios seja zerada a cada recepção válida. A correção NÃO DEVE (MUST NOT)
disparar quando os dois já concordam no slot.

#### Scenario: Deriva de cristal é corrigida no RX
- **WHEN** o relógio do escravo escorregou um slot em relação ao mestre e um
  pacote válido chega com `hop_counter = N`
- **THEN** o escravo ajusta `hop_start_at` para voltar ao slot N e loga a
  reancoragem, seguindo alinhado a partir dali

#### Scenario: Sem RX, o escravo ainda avança pelo tempo
- **WHEN** o escravo passa vários slots sem receber nada (canal jammed)
- **THEN** ele continua avançando o slot pelo relógio (sem estacionar) e volta a
  receber assim que cai num canal limpo, reancorando se tiver derivado

### Requirement: A recuperação de enlace é por tempo sem recepção, não por park
O firmware DEVE (MUST) declarar perda de enlace e voltar ao pareamento quando
passar `LINK_LOSS_MS` sem nenhuma recepção válida, e NÃO DEVE (MUST NOT) manter
o mecanismo de estacionamento/ressincronização por timeout de hop (não há deriva
de fase a recuperar no salto por tempo).

#### Scenario: Blackout longo re-pareia; blackout curto é atravessado
- **WHEN** o escravo fica sem recepção válida por menos de `LINK_LOSS_MS`
- **THEN** ele continua saltando por tempo e retoma sem re-parear
- **WHEN** o escravo fica sem recepção válida por `LINK_LOSS_MS` ou mais
- **THEN** ele volta ao estado de pareamento (mesma máquina do primeiro encontro)

### Requirement: O avanço do dado permanece desacoplado do avanço do slot
O firmware DEVE (MUST) avançar o `fragment_index` (progresso de dados) SOMENTE
quando um ACK é confirmado, independente do avanço do slot (que é por tempo), e
DEVE (MUST) reenviar no próximo slot um fragmento não confirmado, deduplicado
pelo `received_bitmap` no escravo (SEQN).

#### Scenario: Fragmento não-ackado é reenviado no próximo slot e deduplicado
- **WHEN** um fragmento é recebido mas seu ACK se perde
- **THEN** o mestre reenvia o mesmo fragmento no próximo slot e o escravo o
  re-acka sem contá-lo duas vezes na remontagem

