# adaptive-mitigation Specification

## Purpose
TBD - created by archiving change dynamic-seed-blacklist. Update Purpose after archive.
## Requirements
### Requirement: Troca de seed disparada por alerta
O mestre DEVE (MUST) gerar uma nova seed pseudoaleatória e enfileirar um pacote de controle `MUDAR_SEED` com prioridade sobre fragmentos de dados pendentes assim que detectar a transição do alerta de ataque (de inativo para ativo, conforme `spectrum-attack-classification`).

#### Scenario: Início de alerta durante transmissão de dados
- **WHEN** o mestre está transmitindo fragmentos de dados e o alerta de
  ataque transiciona para ativo
- **THEN** o próximo pacote enviado no hop seguinte é `MUDAR_SEED` (não um
  fragmento de dado), e a transmissão de fragmentos é retomada de onde
  parou somente após o `MUDAR_SEED` ser confirmado

### Requirement: Confirmação de seed antes de aplicar
Nem mestre nem escravo DEVEM (MUST) passar a calcular canais com a nova seed antes
de o escravo confirmar recepção de `MUDAR_SEED` via ACK dedicado.

#### Scenario: MUDAR_SEED perdido no primeiro envio
- **WHEN** o mestre envia `MUDAR_SEED` em um hop e não recebe ACK dentro do
  timeout
- **THEN** o mestre continua usando a seed antiga para calcular o próximo
  canal e reenvia `MUDAR_SEED` no hop seguinte, sem aplicar a nova seed
  localmente

#### Scenario: MUDAR_SEED confirmado
- **WHEN** o escravo recebe `MUDAR_SEED` e envia o ACK correspondente, e o
  mestre recebe esse ACK
- **THEN** ambos os nós passam a usar a nova seed a partir do próximo hop

### Requirement: Contagem de falhas de ACK por canal
O mestre DEVE (MUST) manter um contador de falhas de ACK por canal (indexado pela
posição do canal em `HOPPING_CHANNELS`), incrementado a cada timeout de
ACK ocorrido naquele canal e decrementado (sem ficar negativo) a cada
sucesso de ACK naquele canal, independentemente do estado do alerta.

#### Scenario: Falhas seguidas no mesmo canal
- **WHEN** o mestre falha em obter ACK em 3 hops seguidos que caem no
  mesmo índice de canal
- **THEN** o contador de falha daquele índice reflete as 3 falhas
  acumuladas

### Requirement: Blacklisting de canal sob alerta ativo
O mestre DEVE (MUST) avaliar continuamente os contadores de falha por canal e, para qualquer canal habilitado cujo contador exceda um limiar configurável, adicionar esse canal à máscara de blacklist e enfileirar `BLACKLIST_UPDATE` com a máscara atualizada, com prioridade sobre fragmentos de dados pendentes (mas não sobre um `MUDAR_SEED` já enfileirado) — independentemente de o alerta de ataque estar ativo ou não.

#### Scenario: Canal com falha severa durante ataque
- **WHEN** o alerta de ataque está ativo e o contador de falha de um canal habilitado excede o limiar configurado
- **THEN** esse canal é marcado como blacklisted na máscara local do mestre e `BLACKLIST_UPDATE` é enfileirado

#### Scenario: Falha severa sem alerta ativo
- **WHEN** o contador de falha de um canal habilitado excede o limiar configurado e o alerta de ataque NÃO está ativo
- **THEN** esse canal também é marcado como blacklisted e `BLACKLIST_UPDATE` é enfileirado — a decisão depende só da evidência própria do mestre (contador de falha por canal), não do estado do alerta

### Requirement: Máscara nunca esvazia todos os canais
O mestre NÃO DEVE (MUST NOT) permitir que a máscara de blacklist desabilite todos os
16 canais simultaneamente; se aplicar o próximo blacklist resultaria em
zero canais habilitados, esse canal específico é mantido habilitado apesar
do limiar excedido.

#### Scenario: Último canal saudável restante
- **WHEN** 15 dos 16 canais já estão blacklisted e o contador de falha do
  16º canal excede o limiar
- **THEN** esse último canal permanece habilitado na máscara, e nenhum
  `BLACKLIST_UPDATE` que o remova é enviado

### Requirement: Confirmação de blacklist antes de aplicar
Nem mestre nem escravo DEVEM (MUST) passar a calcular canais com uma máscara de
blacklist nova antes de o escravo confirmar recepção de `BLACKLIST_UPDATE`
via ACK dedicado — mesmo padrão de confirmação usado para `MUDAR_SEED`.

#### Scenario: BLACKLIST_UPDATE confirmado
- **WHEN** o escravo recebe `BLACKLIST_UPDATE` e envia o ACK
  correspondente, e o mestre recebe esse ACK
- **THEN** ambos os nós passam a calcular o próximo canal usando a nova
  máscara a partir do próximo hop

### Requirement: Recuperação de PLR observável após mitigação
O `plr_window` reportado pelo escravo (capability `packet-loss-metric`) DEVE (MUST) apresentar tendência de queda nas janelas subsequentes a um `MUDAR_SEED` e/ou `BLACKLIST_UPDATE` confirmados enquanto o jammer continua ativo, comparado ao `plr_window` medido imediatamente antes da mitigação.

#### Scenario: Recuperação após troca de seed sob ataque contínuo
- **WHEN** o jammer permanece ativo e uma nova seed é confirmada por
  ambos os nós
- **THEN** o `plr_window` medido nas janelas seguintes é menor do que o
  `plr_window` medido na janela imediatamente anterior à confirmação

### Requirement: Blacklist persiste através do re-pareamento sob jamming
O sistema DEVE (MUST) preservar o estado de blacklist (canais banidos e suas
contagens/temporizadores de recuperação) através de um re-pareamento causado
por jamming, em vez de zerá-lo. Considera-se causado por jamming um retorno ao
pareamento com pelo menos um canal já acumulando falhas dominantes. O decaimento
de blacklist continua responsável por reincluir os canais quando o jammer sai
(o ban não se torna permanente). Um reset limpo (boot ou comando do operador)
continua zerando o estado.

#### Scenario: Canal atacado continua banido após re-pareamento
- **WHEN** o enlace perde sincronismo sob jamming em um canal já próximo do
  limiar de ban e re-pareia
- **THEN** esse canal permanece banido após o re-pareamento, e o enlace não
  volta a saltar nele até o cooldown de recuperação vencer

#### Scenario: Reset limpo zera o estado
- **WHEN** o operador reinicia ou envia `reset` sem jamming
- **THEN** a blacklist é zerada (máscara volta a todos os canais)

### Requirement: Canal persistentemente ruim é banido antes do colapso
Um canal que acumula um número dominante de falhas DEVE (MUST) ser banido de
forma imediata, sem depender do alerta do analisador nem de o enlace atingir o
limiar de perda de sincronismo (`LOSS_HOPS`). A troca de máscara é propagada e
confirmada num canal não atacado.

#### Scenario: Ban rápido de um canal jammed
- **WHEN** um único canal concentra as falhas de ACK de forma dominante
- **THEN** o canal é banido antes de o enlace colapsar em re-pareamento, e a
  máscara converge para excluir apenas o(s) canal(is) realmente atacado(s)

### Requirement: Jamming de um canal não é tratado como perda de sincronismo
Um miss em um canal já sinalizado como ruim NÃO DEVE (MUST NOT) contar para o
gatilho de re-pareamento por perda de sincronismo, e um único hop falho NÃO
DEVE (MUST NOT) desalinhar o contador de salto entre mestre e escravo. A
atribuição de falha a um canal só ocorre com evidência de que os dois lados
estavam nesse canal (contador de hop coincidente).

#### Scenario: Um canal jammed não derruba o enlace
- **WHEN** o jammer nega um único canal do conjunto de hopping e os demais
  estão limpos
- **THEN** o enlace continua trafegando nos demais canais sem escalar para
  re-pareamento, e as falhas são atribuídas ao canal atacado, não a canais
  inocentes

### Requirement: Escalonamento para reseed quando a blacklist satura
O sistema DEVE (MUST) escalonar para troca de seed (reseed), reembaralhando a
sequência de salto, quando a fração de canais banidos ultrapassa um limiar e o
PLR permanece alto — em vez de continuar banindo até quase esvaziar o conjunto
de canais. Esse caso indica um atacante que a blacklist sozinha não contém
(follower ou cobertura de vários canais).

#### Scenario: Reseed sob saturação de blacklist
- **WHEN** a fração de canais banidos passa do limiar e o PLR continua alto
- **THEN** um reseed é disparado e a sequência de salto muda, com histerese
  para não alternar indefinidamente entre banir e reseedar

### Requirement: Recuperação efetiva sob jammer forte de canal fixo
O enlace com mitigação habilitada DEVE (MUST) recuperar para perto do
throughput de baseline sob um jammer confiável parado num canal do conjunto de
hopping (portadora fixa a potência máxima), com a máscara convergindo e
estável, e não apenas uma fração pequena.

#### Scenario: Mitigação restaura o enlace sob hold em canal fixo
- **WHEN** o enlace está em hopping, mitigação ON, e o jammer emite em
  `mode hold` num canal do conjunto a `power max`
- **THEN** o throughput recupera para uma fração alta do baseline (meta ≥ 80%),
  a máscara converge para excluir o canal atacado e não há ciclos contínuos de
  re-pareamento

### Requirement: Recuperação automática de canal blacklisted
Um canal atualmente excluído da máscara DEVE (MUST) voltar a ser incluído assim que tiver permanecido excluído por pelo menos um número configurável de hops (`RECOVERY_COOLDOWN_HOPS`), independentemente do estado do alerta de ataque e sem exigir nenhuma nova tentativa de transmissão nesse canal.

#### Scenario: Cooldown de recuperação atingido
- **WHEN** um canal está excluído da máscara há pelo menos `RECOVERY_COOLDOWN_HOPS` hops
- **THEN** esse canal é reincluído na máscara e `BLACKLIST_UPDATE` é enfileirado com a máscara atualizada

#### Scenario: Cooldown de recuperação ainda não atingido
- **WHEN** um canal está excluído da máscara há menos de `RECOVERY_COOLDOWN_HOPS` hops
- **THEN** esse canal permanece excluído e nenhuma atualização é enfileirada só por causa dele

#### Scenario: Reinclusão zera a evidência de falha anterior
- **WHEN** um canal é reincluído na máscara após cumprir o cooldown
- **THEN** o contador de falha desse canal é zerado, exigindo evidência de falha totalmente nova (não a que causou a exclusão original) para excluí-lo de novo

### Requirement: Estado de recuperação não sobrevive a um retorno ao pareamento
Ao voltar ao estado de pareamento (perda de sincronismo), o mestre DEVE (MUST) zerar tanto a máscara de blacklist quanto qualquer estado de cooldown de recuperação associado a cada canal, para que a contagem de hops de uma sessão anterior não seja reaproveitada incorretamente na sessão seguinte.

#### Scenario: Reset de sincronismo com canais em cooldown
- **WHEN** o mestre volta ao estado de pareamento enquanto um ou mais canais estavam excluídos aguardando cooldown de recuperação
- **THEN** a máscara volta a habilitar todos os 16 canais e o estado de cooldown de cada canal é zerado, para a próxima sessão de hopping recalcular blacklist/recuperação do zero

