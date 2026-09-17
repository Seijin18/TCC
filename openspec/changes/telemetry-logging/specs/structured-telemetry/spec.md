## ADDED Requirements

### Requirement: Formato de evento único e serializado
Todo evento de telemetria emitido por qualquer nó DEVE (MUST) seguir o
formato `@<src>;t=<millis>;e=<evento>;<chave>=<valor>;...`, e escritas
concorrentes na mesma UART (duas tasks na mesma placa combinada) DEVEM
(MUST) sair como uma linha atômica, sem entrelaçamento de caracteres de
eventos diferentes.

#### Scenario: Duas tasks na mesma placa não entrelaçam linhas
- **WHEN** a task do mestre e a task do analisador (Placa A) chamam
  `tlm_printf()` em instantes próximos
- **THEN** cada linha aparece completa e intacta na saída serial, na ordem
  em que as chamadas obtiveram o lock

### Requirement: Persistência contínua em disco
O sistema de captura DEVE (MUST) persistir cada linha recebida das placas
em um arquivo em disco no momento em que é recebida, sobrevivendo ao
fechamento de qualquer visualizador (GUI) e a reinícios de sessão.

#### Scenario: Sessão sobrevive ao fechamento da GUI
- **WHEN** o daemon de captura está rodando e o dashboard é fechado
- **THEN** o arquivo de sessão continua recebendo novas linhas sem
  interrupção

#### Scenario: Nada é descartado por não reconhecido
- **WHEN** uma linha recebida não segue o formato `@...`
- **THEN** ela ainda é persistida no arquivo de sessão, marcada como evento
  bruto (`raw`)

### Requirement: Estado derivado consultável sem reprocessar a sessão inteira
O sistema DEVE (MUST) manter um snapshot do estado atual do sistema
(enlace, canal, seed, máscara de blacklist, PLR, alerta, modo do jammer)
atualizado periodicamente e legível sem reprocessar o histórico completo.

#### Scenario: Consulta de estado após uma sessão longa
- **WHEN** uma sessão de captura já gerou um arquivo de várias horas
- **THEN** o estado atual do sistema pode ser lido em tempo constante a
  partir do snapshot derivado, sem varrer o arquivo de sessão inteiro

### Requirement: Canal de comando desacoplado da porta serial exclusiva
Quando o daemon de captura é o dono exclusivo de uma porta serial, DEVE
(MUST) existir um canal indireto (arquivo ou equivalente) pelo qual outros
processos possam enviar comandos às placas sem abrir a porta diretamente.

#### Scenario: Dashboard envia comando com o daemon rodando
- **WHEN** o dashboard não tem acesso direto à porta serial (daemon já a
  possui)
- **THEN** o comando enviado pelo usuário ainda chega à placa através do
  canal indireto
