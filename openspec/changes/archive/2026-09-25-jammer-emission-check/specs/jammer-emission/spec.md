## ADDED Requirements

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
- **AND** uma linha `@jammer;...e=variant;p=<0|1>;p2=<0|1|-1>;radios=<1|2>`
  registra a variante e a contagem de rádios

> Nota de resultado (2026-09-25): esta requirement foi implementada e
> verificada em bancada — o rádio do jammer reporta P-variant (`p=1`,
> `radios=1`). As outras duas requirements originalmente propostas nesta
> change ("Emissão do jammer é observável no espectro" e "Emissão sustentada
> independente da variante") NÃO foram satisfeitas: o teste de isolamento
> mostrou que o jammer não irradia observavelmente, e como o rádio é P o fix
> de clone não-P não se aplica. A meta de emissão observável foi transferida
> para a change de investigação de hardware do jammer (`jammer-emission-hardware`).
