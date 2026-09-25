## ADDED Requirements

### Requirement: Modo de emissão em canal fixo (hold)
O firmware do jammer DEVE (MUST) oferecer um modo de emissão que mantenha a
portadora constante parada num único canal, sem saltar, acionável por comando
serial e preservando a interface existente (`jam on|off`, `power`) e a
telemetria de estado (`jam_state_on/off`).

Este modo remove o confundidor de dwell dos modos que saltam (`targeted`,
`sweep`, `adaptive`), dando a qualquer scanner a melhor chance de detectar a
emissão, e serve de atacante de canal fixo para o Cenário A da bateria de
validação.

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

### Requirement: Emissão do jammer verificada em hardware
A emissão de RF do jammer DEVE (MUST) ser verificada por medição — não
assumida a partir do firmware. Com a portadora parada (`mode hold`) num canal
alto limpo a `power max` por ≥ 120 s, a emissão é confirmada se o analisador
registrar atividade clara naquele canal (ON ≫ OFF) OU se um receptor
co-locado sintonizado no mesmo canal perder sincronismo.

Se, mesmo com a portadora parada, não houver emissão detectável nem pelo
analisador nem pelo receptor co-locado, a causa é de hardware (antena,
potência real do módulo, ou módulo defeituoso) e DEVE (MUST) ser diagnosticada
por medição externa (SDR/analisador de espectro) antes de qualquer conclusão
de validação sob ataque.

#### Scenario: Portadora parada é detectável
- **WHEN** o jammer está em `mode hold` num canal alto limpo a `power max`
  por ≥ 120 s e o analisador varre esse canal
- **THEN** a atividade média ON naquele canal fica claramente acima da OFF,
  OU um receptor co-locado no mesmo canal registra `loss_of_sync`

#### Scenario: Ausência de emissão aponta hardware, não firmware
- **WHEN** a portadora parada a `power max` não produz atividade detectável
  nem no analisador nem no receptor co-locado
- **THEN** o resultado é registrado como falha de hardware do jammer e
  encaminhado para diagnóstico por medição externa/troca de módulo, sem
  atribuir a firmware (o rádio é P-variant e o caminho de portadora está
  correto)
