# targeted-jamming Specification

## Purpose
TBD - created by archiving change targeted-jammer. Update Purpose after archive.
## Requirements
### Requirement: Modo targeted restrito à tabela de hopping
Quando o jammer estiver em modo `targeted`, ele DEVE (MUST) emitir portadora apenas
nos canais listados em `HOPPING_CHANNELS` (`shared/hopping.h`), nunca em
canais fora dessa tabela de 16 elementos.

#### Scenario: Sweep de canais em modo targeted (1 rádio)
- **WHEN** o jammer está em modo `targeted` com um único rádio disponível
- **THEN** cada canal visitado pertence ao conjunto `HOPPING_CHANNELS` e,
  ao longo de um ciclo completo, todos os 16 canais são visitados

#### Scenario: Modo targeted com dois rádios
- **WHEN** o jammer está em modo `targeted` com dois rádios disponíveis
- **THEN** os 16 canais são divididos entre os dois rádios (cobertura
  paralela), sem sobreposição desnecessária nem canais fora da tabela

### Requirement: Seleção de modo em runtime
O firmware DEVE (MUST) aceitar um comando serial `mode sweep` ou `mode targeted`
que troca o modo de operação sem exigir novo flash, com modo `sweep` como
padrão ao ligar.

#### Scenario: Troca de sweep para targeted em runtime
- **WHEN** o jammer está rodando em modo `sweep` e recebe `mode targeted`
  pela serial
- **THEN** nos próximos ciclos de dwell, os canais visitados passam a ser
  restritos a `HOPPING_CHANNELS`, e uma confirmação é impressa na serial

#### Scenario: Comando de modo inválido
- **WHEN** o firmware recebe um comando serial não reconhecido (ex.:
  `mode xyz`)
- **THEN** o modo atual é mantido inalterado e uma mensagem de erro é
  impressa na serial

### Requirement: Modo adaptive concentra emissão no canal em uso real
Quando o jammer estiver em modo `adaptive`, ele DEVE (MUST) sensoriar por
energia (RPD) os canais de `HOPPING_CHANNELS` em vez de percorrê-los em
round-robin cego, travar emissão contínua no primeiro canal com atividade
detectada, e revalidar periodicamente se esse canal continua ativo antes
de continuar concentrado nele.

#### Scenario: Trava no canal com atividade real
- **WHEN** o jammer está em modo `adaptive` e sensoria um canal de
  `HOPPING_CHANNELS` com atividade real (RPD positivo)
- **THEN** ele trava e emite continuamente nesse canal, em vez de avançar
  para o próximo candidato do round-robin

#### Scenario: Solta um canal que ficou quieto
- **WHEN** o jammer está travado (modo `adaptive`) num canal e a
  revalidação periódica não detecta mais atividade nele
- **THEN** ele solta o canal e volta a escanear os demais candidatos de
  `HOPPING_CHANNELS`

#### Scenario: Comando `mode adaptive` reconhecido em runtime
- **WHEN** o firmware recebe `mode adaptive` pela serial
- **THEN** o modo ativo muda para `adaptive` e uma confirmação é impressa,
  sem exigir novo flash

### Requirement: Modo targeted disponível no harness de validação
O harness `validacao/src/main.cpp` DEVE (MUST) suportar o mesmo comando de modo
(`mode sweep|targeted`) aplicado à sua lógica de seleção de canal do
jammer (`stepJammer`/`jammerStart`), preservando o comportamento existente
dos comandos `jammer on|off`.

#### Scenario: Jammer ligado em modo targeted no harness de validação
- **WHEN** o operador envia `mode targeted` seguido de `jammer on` ao
  harness `validacao`
- **THEN** os canais escolhidos por `stepJammer` pertencem a
  `HOPPING_CHANNELS`, e o comando `status` reporta o modo ativo

