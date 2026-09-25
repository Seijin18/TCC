# Design — emissão e eficácia de jamming do atacante

## Contexto reconciliado (o que já se sabe, de duas sessões)

- **`jammer-emission-check` (arquivada, 2026-09-25)**: teste de isolamento com
  o analisador da Placa A a 30-50 cm — banda limpa 94-125, `targeted`+`power
  max`, 120 s. Combinado ON=0.000 vs OFF=0.008; zero `loss_of_sync` no escravo
  co-locado. Rádio do jammer é **P-variant** (`p=1`, `radios=1`), `rf_setup=0B`
  no boot (a PA_LOW).
- **Diagnóstico de antena (commit `19c7352`, 2026-09-17)**: harness
  `validacao/src/main.cpp` (jammer VSPI + analisador HSPI **no mesmo ESP32**).
  **Placa B passa**: o analisador vê o próprio jammer com salto sustentado em
  quase todos os canais. **Placa A falha**: analisador HSPI lê zero absoluto
  em tudo (inclusive ruído ambiente) — bloqueio de RF isolado à Placa A,
  hipótese de alimentação compartilhada dos dois rádios (aviso já em
  `docs/pinout.md`).

**Conclusão reconciliada**: o jammer emite (provado de perto), mas fraco
demais para negar comunicação ou alcançar 30-50 cm. O null de
`jammer-emission-check` é em parte **confundido** pela sonda ter sido o
analisador da Placa A (o mesmo que já leu zero em tudo). A sonda confiável de
emissão é o harness co-locado `validacao/` e o escravo co-locado.

## Presença de RF ≠ eficácia de jamming (o ponto central)

`testCarrier()`/RPD dispara em ~-64 dBm — basta uma portadora fraca para
"detectar presença". **Negar recepção** exige que o jammer supere o sinal do
mestre no receptor do escravo, no canal e no instante certos. São barras
diferentes por ordens de grandeza. Por isso esta change separa duas coisas que
`jammer-emission-check` misturava:

1. **Emissão observável** (presença) — mede-se com analisador/RPD.
2. **Eficácia de jamming** (negação) — mede-se pelo enlace: `loss_of_sync` /
   PLR do escravo quando o jammer bate no canal dele. **É esta que valida o
   TCC** e a que precisa passar.

## Como jammers nRF24 de estudo open-source lidam com clones fracos

Levantamento na web (setembro/2026) de projetos abertos de jammer nRF24 para
fins educacionais/pesquisa, e o que cada um ensina sobre emissão fraca em
clones PA+LNA (Si24R1):

| Projeto | Técnica relevante |
|---|---|
| Jammox (nRF24+PA+LNA, Si24R1) | Reaplica a config `0x3E` (`RF_SETUP`) **dentro do loop** — "overdrive protection" — para o clone não cair fora do CONT_WAVE |
| CiferTech nRF-jammer | Varredura rápida da banda com múltiplos módulos; documenta o hardware PA+LNA e a necessidade de alimentação robusta |
| Mr-subProcess/Nrf24_Jammer | Jammer com PA; ênfase em `setPALevel(MAX)` e hop rápido |
| AntonBronnfjell (ESP32-S3, 2×nRF24) | Plataforma multi-TX educacional; port do RF24 para ESP-IDF, três caminhos de TX simultâneos para cobrir mais banda |
| Guia circuitdigest / fóruns MySensors/Arduino | PA+LNA puxa ~115 mA de pico em TX → regulador 3V3 fraco derruba a emissão; Si24R1 precisa de bit extra no `RF_SETUP` para potência máxima (padrão ~2-3 dBm, com o bit ~7 dBm) |

Lições aplicáveis aqui, em ordem de custo/risco:

1. **Alimentação primeiro** (barato, provável): dar ao módulo PA+LNA do jammer
   uma fonte 3V3 que sustente ~115 mA (regulador dedicado ou capacitor de bulk
   de 100-470 µF junto ao módulo), e consertar a alimentação da Placa A. É a
   explicação única que fecha os dois sintomas (Placa A cega + jammer fraco).
2. **Re-arme de CONT_WAVE no loop** (firmware, barato): reescrever periodicamente
   `RF_SETUP`+CONT_WAVE+PLL_LOCK durante a emissão, para clones que caem fora.
3. **Confirmar potência máxima real** (firmware/diagnóstico): telemetrar
   `RF_SETUP` durante `jam on` e checar RF_PWR/bit extra; o boot mostrou
   `rf_setup=0B` a PA_LOW — precisamos ver o valor a PA_MAX.
4. **Inundação de payload em vez de CW estreita** (firmware): uma CW estreita é
   trivial de escapar por hopping; um sinal modulado real (payloads contínuos
   sem ACK) nega recepção com muito mais eficácia por canal — é o que a maioria
   dos jammers de negação usa. Já esboçado no design da change anterior; aqui
   deixa de ser "fix não-P" e passa a ser "emissão mais efetiva por canal".

## Por que um modo de canal fixo (`hold`) e como

Os três modos atuais saltam (`targeted` ~60 ms/canal em 16; `sweep` 2-80;
`adaptive` só trava em canal com atividade). Nenhum concentra energia num
canal por tempo prolongado, o que é péssimo tanto para detecção quanto para
negação. `hold`:

- Reusa `startCarrier`/`startCarrierOn` (CW sustentada, que no rádio P
  sustenta), sem chamar `hopCarrier` no loop.
- Comando `mode hold` (canal alto default) e `mode hold <ch>`; enum
  `JAM_MODE_HOLD`; `loopHold()` mantém a portadora (+ re-arme do item 2 acima)
  sem trocar canal; telemetria `jam_mode_hold`.
- Serve de atacante determinístico de canal fixo para o Cenário A (resolve a
  pendência #3 do ROADMAP: "nenhum modo atual fica parado no canal fixo").

## Sequência de validação (a espinha dorsal desta change)

```
A. Medir emissão pelo caminho confiável (validacao/ co-locado)
   - reflash env:validacao numa placa, jammer on, medir salto de atividade.
   - baseline da emissão ATUAL antes de qualquer fix.
      │
B. Aplicar fixes em ordem de custo até a emissão negar comunicação:
   - alimentação do módulo (hardware, manual)  ──► re-medir A
   - re-arme CONT_WAVE + RF_SETUP a PA_MAX (fw) ──► re-medir A
   - inundação de payload (fw)                  ──► re-medir A
      │
C. Validar EFICÁCIA (não só presença):
   - enlace preso em mode fixed no canal X; jammer em hold no canal X, PA_MAX.
   - critério de sucesso: escravo co-locado registra loss_of_sync / PLR sobe
     claramente (o ataque NEGA comunicação). Se sim, o atacante é válido.
      │
D. Fechar o pipeline de mitigação:
   - com o ataque negando de fato, confirmar alerta→blacklist/reseed→
     recuperação (enlace volta). Isto encerra a ambiguidade de toda a série.
```

## O que NÃO fazer nesta change

- Não concluir "hardware morto" a partir do analisador da Placa A — ele já
  provou ler zero em tudo; usar o `validacao/` co-locado como árbitro.
- Não tratar "RPD detectou" como "jamming funcionou" — são coisas diferentes.
- Não rodar a bateria A/B/C aqui — esta change entrega o atacante válido e
  desbloqueia a metodologia; a bateria é de `scenario-automation`.
- Não mexer em quais canais o `targeted` ataca (isso é `targeted-jammer`).

## Referências (jammers nRF24 open-source de estudo)

- Jammox: https://github.com/berat8027/Jammox
- CiferTech nRF-jammer: https://cifertech.net/nrf-jammer-wifi-jammer-with-nrf24l01/
- Mr-subProcess/Nrf24_Jammer: https://github.com/Mr-subProcess/Nrf24_Jammer
- AntonBronnfjell ESP32-S3 dual nRF24 (educacional): https://github.com/AntonBronnfjell/project-esp32-embedsyst-cpp_ble-jammer-nrf24l01
- Guia ESP32+nRF24 (circuitdigest): https://circuitdigest.com/tutorial/esp32-bluetooth-jammer-using-nrf24l01
- PA+LNA e alimentação (MySensors): https://forum.mysensors.org/topic/1142/nrf24l01-pa-lna
- CW carrier no nRF24 (Arduino forum): https://forum.arduino.cc/t/nrf24l01-arduino-trying-to-get-constant-carrier-wave-output-for-testing/322112
- Variantes/RF_SETUP Si24R1 (electricalflux): https://electricalflux.com/learn-guides/nrf24l01-module-buying-guide-variants
