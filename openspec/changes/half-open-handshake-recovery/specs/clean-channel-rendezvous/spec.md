## ADDED Requirements

### Requirement: Handshake meio-aberto é detectado e recuperado sem operador
Mestre e escravo DEVEM (MUST) detectar quando o pareamento fica meio-aberto
— um lado recebe o outro mas não é ouvido de volta — e recuperar sozinhos,
reinicializando o próprio rádio, sem depender de comando do operador.

#### Scenario: Mestre não recebe nenhuma resposta por vários acampamentos
- **WHEN** o mestre completa `HALF_OPEN_SILENT_CAMPS` acampamentos seguidos
  sem receber nenhum pacote
- **THEN** o mestre reinicializa o rádio, emite `e=regs;why=half_open`, e
  repete a reinicialização com backoff crescente (até 60 s) enquanto nenhum
  pacote chegar

#### Scenario: Escravo responde sondas mas o enlace nunca avança
- **WHEN** o escravo responde sondas de `HALF_OPEN_SESSIONS` sessões
  seguidas (nonces distintos) sem receber nenhum `FragmentPacket`
- **THEN** o escravo emite `e=half_open`, reinicializa o rádio e volta a
  varrer os candidatos de pareamento

#### Scenario: Recuperação dentro do prazo
- **WHEN** um handshake meio-aberto se instala depois de uma perda de enlace
  e o canal de pareamento está livre
- **THEN** o enlace volta a trocar fragmentos em no máximo 15 s, sem
  intervenção

### Requirement: Saúde do receptor é observável durante o pareamento
O mestre DEVE (MUST) relatar, a cada acampamento, quantos pacotes recebeu e
em quantas janelas de espera de resposta detectou portadora, para
distinguir "escravo ausente" de "escravo transmitindo e nada decodificado".

#### Scenario: Estatística de acampamento com indicador de portadora
- **WHEN** um acampamento de pareamento termina
- **THEN** `pair_stats` inclui `rx_total` e `rx_rpd` (janelas de espera de
  ack com portadora detectada)

### Requirement: Reinicialização de rádio é verificada
Depois de reinicializar o rádio, cada nó DEVE (MUST) ler de volta os
registradores críticos e, se algum divergir do esperado, executar a
reinicialização completa de boot.

#### Scenario: Registrador divergente após reinit
- **WHEN** o readback de `CONFIG`, `EN_RXADDR`, `RX_ADDR_P1`, `RF_CH`,
  `FEATURE`, `DYNPD` ou `RF_SETUP` não bate com o esperado depois de
  `radio_reinit()`
- **THEN** o nó executa a sequência completa de inicialização do rádio e
  emite `e=regs` com o estado resultante
