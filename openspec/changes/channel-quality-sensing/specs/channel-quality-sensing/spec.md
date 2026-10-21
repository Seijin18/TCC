## ADDED Requirements

### Requirement: Qualidade por canal medida independentemente da causa
O analisador DEVE (MUST) classificar cada canal da tabela de hopping como
degradado ou utilizável a partir da ocupação medida, com histerese, sem
tentar distinguir jamming de congestionamento.

#### Scenario: Canal tomado por jammer
- **WHEN** um jammer mantém portadora num canal da tabela de hopping
- **THEN** a ocupação desse canal ultrapassa `T_on` e o canal é marcado
  degradado

#### Scenario: Canal tomado por Wi-Fi
- **WHEN** tráfego Wi-Fi mantém a ocupação de um canal da tabela acima de
  `T_on`
- **THEN** o canal é marcado degradado, com o mesmo tratamento dado a um
  jammer

#### Scenario: Ocupação oscilando perto do limiar
- **WHEN** a ocupação de um canal oscila entre `T_off` e `T_on`
- **THEN** o estado do canal não muda; só muda ao cruzar `T_on` (entrada) ou
  `T_off` (saída)

### Requirement: Blacklist funde evidência do enlace e do espectro
O mestre DEVE (MUST) compor a máscara de canais ativos a partir da sua
própria evidência de falha por canal e da máscara de canais degradados do
analisador, mantendo sempre pelo menos 4 canais ativos.

#### Scenario: Analisador antecipa um canal ruim
- **WHEN** o analisador marca um canal degradado antes de o enlace acumular
  falhas nele
- **THEN** o canal é retirado da máscara (respeitando histerese) e a nova
  máscara é aplicada nos dois lados do enlace

#### Scenario: Guarda de banda mínima
- **WHEN** a combinação das duas fontes deixaria menos de 4 canais ativos
- **THEN** a máscara mantém os 4 canais de melhor histórico

#### Scenario: Canal volta a ficar limpo
- **WHEN** o analisador volta a marcar um canal como utilizável e o enlace
  não tem falha recente nele
- **THEN** o canal volta à máscara depois do cooldown de reinclusão

### Requirement: Troca de seed reservada para quando a blacklist não basta
O mestre DEVE (MUST) disparar troca de seed apenas quando muitos canais
estiverem degradados ao mesmo tempo ou quando a perda do enlace continuar
alta depois de aplicada a blacklist, e não a cada canal degradado.

#### Scenario: Um canal degradado
- **WHEN** um único canal é marcado degradado e a blacklist é aplicada
- **THEN** a seed não muda

#### Scenario: Blacklist insuficiente
- **WHEN** a perda do enlace continua acima do limiar por várias janelas
  depois de aplicada a blacklist
- **THEN** o mestre agenda uma troca de seed
