## ADDED Requirements

### Requirement: Modo de emissão em canal fixo (hold)
O firmware do jammer DEVE (MUST) oferecer um modo de emissão que mantenha a
portadora constante parada num único canal, sem saltar, acionável por comando
serial e preservando a interface existente (`jam on|off`, `power`) e a
telemetria de estado (`jam_state_on/off`).

Este modo remove o confundidor de dwell dos modos que saltam (`targeted`,
`sweep`, `adaptive`), dando a qualquer sonda a melhor chance de detectar a
emissão, e serve de atacante de canal fixo determinístico para o Cenário A da
bateria de validação.

#### Scenario: Comando ativa o hold num canal
- **WHEN** o operador envia `mode hold` (canal default alto) ou
  `mode hold <ch>` e depois `jam on`
- **THEN** o jammer emite portadora constante fixa no canal escolhido, sem
  trocar de canal, e emite telemetria `jam_mode_hold`

#### Scenario: Hold preserva a interface existente
- **WHEN** o jammer está em `mode hold`
- **THEN** `jam off`, `power <lvl>` e a telemetria `jam_state_on/off`
  continuam funcionando como nos demais modos, sem quebrar
  `run_scenario.py`/`run_battery.py`

### Requirement: Emissão robusta em módulo clone
O firmware do jammer DEVE (MUST) sustentar a emissão de forma confiável em
módulos nRF24 clone (Si24R1/PA+LNA), que podem cair fora do modo de portadora
constante e exigir um bit adicional para atingir potência máxima. Para isso o
firmware reaplica periodicamente a configuração de rádio (`RF_SETUP` +
CONT_WAVE + PLL_LOCK) durante a emissão e telemetra o `RF_SETUP` efetivo sob
`jam on` para verificação.

#### Scenario: Portadora não cai fora em clone
- **WHEN** o jammer está emitindo por tempo prolongado num clone que tende a
  sair do modo CONT_WAVE
- **THEN** o firmware reaplica a configuração dentro do loop de emissão, e a
  portadora permanece ativa de forma sustentada

#### Scenario: Potência máxima verificável por telemetria
- **WHEN** o jammer é habilitado a `power max`
- **THEN** uma telemetria `@jammer;...e=regs;why=jam_on;...rf_setup=...` é
  emitida, permitindo confirmar que o `RF_SETUP` está no nível máximo
  (incluindo o bit extra de potência do Si24R1)

### Requirement: Eficácia de jamming verificada por negação de comunicação
A validação do jammer como atacante DEVE (MUST) medir **negação de
comunicação**, não apenas presença de RF. Detecção de presença por RPD/
`testCarrier` (que dispara com portadora fraca) NÃO é suficiente para
declarar o jammer eficaz. A eficácia é confirmada quando, com o enlace preso
num canal e o jammer emitindo nesse mesmo canal a `power max`, o receptor
(escravo) registra `loss_of_sync` e/ou aumento claro de PLR/`miss` em relação
à fase sem jammer.

#### Scenario: Ataque no canal do enlace nega comunicação
- **WHEN** o enlace está preso (`mode fixed`) no canal X e o jammer emite
  (`hold` ou inundação de payload) no canal X a `power max`
- **THEN** o escravo registra `loss_of_sync` e/ou o PLR/`miss` sobe claramente
  vs a fase com o jammer desligado

#### Scenario: Presença de RF sem negação não conta como eficaz
- **WHEN** o analisador/RPD detecta a portadora do jammer mas o enlace no
  canal atacado não sofre aumento de PLR nem `loss_of_sync`
- **THEN** o jammer é classificado como emissor fraco/ineficaz, e o resultado
  encaminha para fortalecer a emissão (alimentação, re-arme, inundação de
  payload) ou trocar o módulo, antes de qualquer conclusão de validação sob
  ataque

### Requirement: Mitigação valida contra ataque que nega comunicação
A mitigação DEVE (MUST) ser demonstrada restaurando o enlace sob um ataque que
de fato nega comunicação (alerta, depois blacklist/reseed, depois
recuperação), fechando a ambiguidade entre "mitigação venceu o ataque" e "não
havia ataque".

#### Scenario: Enlace se recupera sob ataque eficaz
- **WHEN** o jammer nega comunicação num canal e a mitigação está habilitada
- **THEN** o sistema detecta, aplica blacklist/reseed nos dois lados, e o
  enlace volta a trafegar (PLR cai), com a sequência observável na telemetria
