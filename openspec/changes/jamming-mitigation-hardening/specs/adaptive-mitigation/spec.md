## ADDED Requirements

### Requirement: Blacklist persiste através do re-pareamento sob jamming
Quando um retorno ao pareamento é causado por jamming (perda de sincronismo com
pelo menos um canal já acumulando falhas dominantes), o sistema DEVE (MUST)
preservar o estado de blacklist (canais banidos e suas contagens/temporizadores
de recuperação) através do re-pareamento, em vez de zerá-lo. O decaimento de
blacklist continua responsável por reincluir os canais quando o jammer sai
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
Quando a fração de canais banidos ultrapassa um limiar e o PLR permanece alto
(indicando um atacante que a blacklist sozinha não contém — follower ou
cobertura de vários canais), o sistema DEVE (MUST) escalonar para troca de seed
(reseed), reembaralhando a sequência de salto, em vez de continuar banindo até
quase esvaziar o conjunto de canais.

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
