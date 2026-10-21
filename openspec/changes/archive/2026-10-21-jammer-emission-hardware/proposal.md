## Why

O jammer não está funcionando como atacante útil: nas baterias sob ataque a
taxa de `miss` do mestre não sobe quando ele liga, e o teste de isolamento de
`jammer-emission-check` (arquivada) não viu energia na banda limpa 94-125 com
o analisador da Placa A a 30-50 cm. **Mas "não irradia" é conclusão forte
demais** e precisa ser reconciliada com duas evidências anteriores:

1. **A emissão JÁ foi comprovada de perto** (commit `19c7352`, 2026-09-17):
   o harness `validacao/src/main.cpp` (jammer VSPI + analisador HSPI **no
   mesmo ESP32**, sem lógica de pareamento) mostrou o analisador da Placa B
   detectando o próprio jammer da mesma placa com **salto de atividade claro e
   sustentado em quase todos os canais**. Ou seja, o rádio do jammer da Placa
   B emite.
2. **O analisador da Placa A estava cego** naquele diagnóstico: lia zero
   absoluto nos 126 canais em toda condição (inclusive ruído ambiente). O
   bloqueio de RF foi isolado à Placa A, com hipótese de **alimentação
   compartilhada** entre seus dois rádios (já alertada em `docs/pinout.md`).
   O teste de `jammer-emission-check` usou justamente o analisador da Placa A
   como sonda — então o veredito "não irradia" está **confundido**.

Reconciliação correta: **o jammer emite, mas fraco demais** — detectável por
um detector de presença (RPD/`testCarrier`) co-locado a centímetros, porém
insuficiente para (a) negar recepção do escravo co-locado ou (b) ser visto a
30-50 cm. Detectar presença (RPD dispara em ~-64 dBm) é uma barra muito mais
baixa que **negar comunicação**, que é o que este TCC precisa demonstrar.

Causas prováveis de emissão fraca em módulos nRF24 PA+LNA clones (Si24R1),
documentadas por projetos open-source de jammer de estudo (ver design.md):

- **Alimentação insuficiente**: PA+LNA puxa ~115 mA de pico em TX; muitos
  reguladores 3V3 de placas ESP32 não sustentam isso, e a portadora colapsa
  sob carga. Bate com a hipótese de energia da Placa A e o aviso do pinout.
- **Clone cai fora do modo CONT_WAVE**: firmwares como o Jammox reaplicam a
  config (`RF_SETUP`/`0x3E`) dentro do loop ("overdrive protection") para o
  clone não sair da portadora constante.
- **Bit extra de potência do Si24R1**: o clone precisa de um bit adicional no
  `RF_SETUP` para chegar à potência máxima; o `setPALevel` da lib pode não o
  ativar de forma confiável.

Enquanto o jammer não **negar comunicação** de fato, toda a validação sob
ataque fica ambígua ("mitigação venceu" vs "não havia ataque"), bloqueando
`targeted-jammer` 3.2, `spectrum-attack-classification` 3.1/3.3,
`two-board-topology` 6.2, `dynamic-seed-blacklist` 4.5 e a bateria A/B/C.

## What Changes

- **Medir a força da emissão pelo caminho confiável** (não pelo analisador da
  Placa A): usar o harness `validacao/` (mesma placa, co-locado) e o escravo
  co-locado como sondas, quantificando o salto de atividade e — o que importa
  de fato — se o enlace **cai** quando o jammer bate no canal dele.
- **Modo de jammer em canal fixo (`hold`) no firmware de produção**: portadora
  parada num único canal, removendo o confundidor de dwell do `targeted`
  (cada canal só ~6% das varreduras) e servindo de atacante de canal fixo para
  o Cenário A. Preserva a interface (`jam on|off`, `power`) e a telemetria.
- **Fortalecer a emissão via firmware (técnicas de clone documentadas)**:
  reaplicar `RF_SETUP`/CONT_WAVE periodicamente no loop de emissão (anti
  drop-out do clone), confirmar por telemetria que o `RF_SETUP` durante
  `jam on` está em potência máxima (bit extra do Si24R1 incluído), e — se o
  hardware exigir — trocar a emissão CW estreita por **inundação de payload**
  numa faixa (sinal modulado real, mais efetivo para negar recepção que uma CW
  estreita, que é trivial de escapar por hopping).
- **Diagnóstico/mitigação de hardware (manual, guiado por prioridade)**: (1)
  alimentação dedicada/capacitor de bulk no módulo PA+LNA do jammer e o
  conserto da alimentação da Placa A; (2) medição externa por SDR/analisador
  de espectro; (3) troca do módulo. Só depois de esgotar (1)-(2).
- **Fechar a validação de jamming**: com o jammer negando comunicação de fato
  (enlace preso no canal atacado cai / PLR sobe), rodar o pipeline
  alerta→blacklist/reseed→recuperação e confirmar que a mitigação restaura o
  enlace — isto encerra a ambiguidade de toda a série sob ataque.

## Capabilities

### Modified Capabilities
- `jammer-emission`: acrescenta (a) o modo de emissão em canal fixo, (b) a
  exigência de que a **eficácia de jamming** (negação de comunicação) seja
  verificada por medição — não só a presença de RF — e (c) robustez de
  emissão para módulos clone (re-arme de CONT_WAVE, potência máxima
  confirmada, inundação de payload como alternativa à CW estreita).

## Impact

- `jammer/src/node.h`: modo `hold`; re-arme periódico de `RF_SETUP`/CONT_WAVE
  no loop; telemetria do `RF_SETUP` sob `jam on`; opcional caminho de
  inundação de payload.
- `tools/validation/emission_check.py`: acionar `mode hold`; medir eficácia
  via `loss_of_sync`/PLR do escravo com o enlace preso no canal atacado.
- `validacao/` (env): reusado como sonda de emissão co-locada de referência.
- `docs/pinout.md`: registrar a solução de alimentação do módulo do jammer.
- Destrava a decisão de metodologia A/B/C (`scenario-automation` 4.2/4.3) e a
  interpretação de toda a validação sob ataque.
