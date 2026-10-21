## ADDED Requirements

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
Enquanto o alerta de ataque estiver ativo, o mestre DEVE (MUST) avaliar os
contadores de falha por canal e, para qualquer canal cujo contador exceda
um limiar configurável, adicionar esse canal à máscara de blacklist e
enfileirar `BLACKLIST_UPDATE` com a máscara atualizada, com prioridade
sobre fragmentos de dados pendentes (mas não sobre um `MUDAR_SEED` já
enfileirado).

#### Scenario: Canal com falha severa durante ataque
- **WHEN** o alerta de ataque está ativo e o contador de falha de um canal
  excede o limiar configurado
- **THEN** esse canal é marcado como blacklisted na máscara local do
  mestre e `BLACKLIST_UPDATE` é enfileirado

#### Scenario: Falha severa sem alerta ativo
- **WHEN** o contador de falha de um canal excede o limiar configurado mas
  o alerta de ataque não está ativo
- **THEN** nenhum canal é blacklisted nem `BLACKLIST_UPDATE` é enviado

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
