## ADDED Requirements

### Requirement: Emissão do jammer é observável no espectro
O jammer habilitado (`jam on`) em modo `targeted` a `power max` DEVE (MUST)
produzir energia de RF mensurável pelo analisador nos canais da tabela de
hopping, distinguível do ruído de fundo da bancada.

A verificação é feita numa faixa livre de confundidores: com o enlace
mestre-escravo desligado, comparar a atividade medida pelo analisador
(`logs/spectrum.csv`) nos canais altos da tabela (94, 100, 105, 110, 115,
120, 125 — fora do Wi-Fi da bancada e fora da faixa de sweep 2-80) entre
jammer OFF e jammer ON.

#### Scenario: Faixa alta limpa sobe com o jammer ligado
- **WHEN** o enlace está desligado, o jammer em `mode targeted` + `power max`,
  e o analisador varrendo a faixa completa (0-125)
- **THEN** pelo menos um dos canais {94,100,105,110,115,120,125} apresenta
  atividade média na fase ON claramente acima da fase OFF (proposta:
  ON ≥ 2× OFF e acima do ruído de fundo da faixa em OFF)

#### Scenario: Jammer desligado não eleva a faixa alta
- **WHEN** o jammer está desligado (`jam off`) e o enlace desligado
- **THEN** a atividade média nos canais 94-125 fica no piso de ruído da
  bancada, sem elevação atribuível ao jammer

### Requirement: Telemetria de variante do rádio no boot do jammer
O firmware do jammer DEVE (MUST) emitir no boot a telemetria de
registradores do(s) rádio(s) (`isPVariant`, `RF_SETUP`), no mesmo formato de
`tlm_radio_regs` que mestre e escravo já usam, para permitir diagnosticar
emissão fraca/ausente.

#### Scenario: Boot do jammer registra a variante
- **WHEN** a placa do jammer inicializa e detecta o rádio
  (`isChipConnected()`)
- **THEN** uma linha de telemetria `@jammer;...e=regs;...` com `isPVariant` e
  `RF_SETUP` é emitida pela serial, por rádio presente

### Requirement: Emissão sustentada independente da variante do chip
O firmware DEVE (MUST), quando o rádio do jammer for um clone não-P (em que
`startConstCarrier` não sustenta a portadora), usar um método de emissão que
produza sinal sustentado no canal alvo — transmissão contínua de payloads
fixos sem espera de ACK — mantendo inalterada a interface serial
(`mode`/`jam`/`power`) e a telemetria de estado (`jam_state_on/off`,
`jam_mode_*`).

#### Scenario: Clone não-P emite via inundação de payload
- **WHEN** o rádio reporta não ser variante P e o jammer é habilitado em modo
  targeted
- **THEN** o jammer transmite continuamente no canal alvo por inundação de
  payload (auto-ack desligado), e a Requirement "Emissão do jammer é
  observável no espectro" passa a ser satisfeita

#### Scenario: Variante P mantém o caminho de portadora constante
- **WHEN** o rádio reporta ser variante P
- **THEN** o jammer continua usando `startConstCarrier`/`hopCarrier` (sem
  regressão do hardware que já funciona)
