## Why

A change `jammer-emission-check` respondeu de forma decisiva: **o jammer não
irradia energia de RF observável** quando habilitado (`targeted` + `power
max`). O teste de isolamento (enlace preso em `mode fixed` no ch 10, olhar só
a banda limpa 94-125) deu fração combinada ON=0.000 vs OFF=0.008, e o escravo
co-locado a centímetros do jammer **não perdeu sincronismo nenhuma vez** a
`power max` — dois sinais independentes.

O diagnóstico de boot mostrou que o rádio do jammer é **P-variant** (`p=1`, 1
rádio), então `startConstCarrier` já usa o caminho correto de portadora
sustentada. **Não é o bug de firmware não-P** que a change anterior supunha:
a causa raiz é de hardware. Distância também não é (placas a 30-50 cm, e o
co-locado igualmente não sente nada).

Enquanto o atacante não emitir de fato, toda a linha de validação sob ataque
fica ambígua ("mitigação venceu o ataque" e "não havia ataque" são
indistinguíveis) — bloqueia `targeted-jammer` 3.2,
`spectrum-attack-classification` 3.1/3.3, `two-board-topology` 6.2,
`dynamic-seed-blacklist` 4.5 e a bateria A/B/C de `scenario-automation`.

Um confundidor ainda não eliminado: em `targeted` o jammer segura cada canal
só ~60 ms e percorre 16 canais, então mesmo emitindo bem cada canal só
apareceria em ~6% das varreduras do analisador. Antes de concluir que o
hardware está morto, é preciso dar ao detector (analisador + escravo
co-locado) a **melhor chance possível**: uma portadora **parada** num único
canal por tempo prolongado. Esse modo não existe no firmware (nem `targeted`,
nem `sweep`, nem `adaptive` ficam parados) — e a falta dele também trava a
decisão de metodologia do Cenário A (`scenario-automation` 4.2/4.3: "nenhum
modo atual fica parado no canal fixo").

## What Changes

- **Modo de jammer em canal fixo (firmware)**: novo comando serial
  `mode hold [ch]` (ou equivalente) que trava a portadora constante num único
  canal (default: um canal alto da tabela, ex. 125) sem saltar, mantendo a
  interface (`jam on|off`, `power`) e a telemetria (`jam_state_on/off`,
  novo `jam_mode_hold`). Remove o confundidor de dwell e serve de atacante de
  canal fixo para o Cenário A.
- **Reteste de emissão com portadora parada**: repetir o teste de isolamento
  de `jammer-emission-check` (via `tools/validation/emission_check.py`, com
  suporte ao novo modo) com o jammer em `hold` no canal 125 por ≥ 120 s.
  Resposta binária de novo: irradia / não irradia, agora sem o confundidor de
  dwell.
- **Diagnóstico de hardware (manual, guiado)**: se mesmo parado não irradia,
  checklist de hardware — antena/conexão do módulo nRF24 do jammer, medir com
  SDR/analisador de espectro externo, comparar RF_SETUP/PA real, avaliar
  troca do módulo do jammer por um sabidamente bom.

## Capabilities

### Modified Capabilities
- `jammer-emission`: acrescenta o modo de emissão em canal fixo e reintroduz
  a meta de emissão observável — agora condicionada a verificação de hardware,
  não a firmware de clone não-P (essa hipótese foi descartada).

## Impact

- `jammer/src/node.h`: novo modo `hold` (canal fixo, sem hopping da
  portadora) e comando serial correspondente; telemetria `jam_mode_hold`.
- `tools/validation/emission_check.py`: opção para acionar `mode hold` no
  lugar de `targeted` na fase ON.
- Destrava a decisão de metodologia A/B/C (`scenario-automation` 4.2/4.3) ao
  fornecer um atacante de canal fixo, **e** fecha a ambiguidade de toda a
  série de validação sob ataque quando a emissão for confirmada (ou o rádio
  trocado).
