## Why

A Fase 5 do guia pede uma bateria de 3 cenários comparativos nomeados (A:
sem hopping vs. jammer; B: hopping fixo vs. jammer targeted; C: sistema
completo adaptativo vs. jammer) com gráficos de throughput e latência para
a monografia. Hoje `tools/spectrum/main.py` já grava CSV de espectro e liga
o jammer via comando serial automatizado por tempo (`RecordTab`,
`_desired_jammer_on`/`_send_jammer_command`), mas: (1) não existe modo
"sem hopping" no firmware do mestre/escravo para rodar o Cenário A, (2) o
jammer só liga/desliga, não seleciona sweep vs. targeted (isso é
`targeted-jammer`), (3) a ferramenta só lê a serial do
`analisador`/`validacao` (espectro), não a do escravo (PLR, de
`plr-metric`), então hoje não dá pra correlacionar espectro com qualidade
de link em uma única gravação, e (4) não existe orquestração dos 3
cenários nem geração dos gráficos de throughput/latência pedidos pelo
guia — só o heatmap de espectro existe.

Este change depende de todos os anteriores (`plr-metric`,
`targeted-jammer`, `spectrum-attack-classification`,
`dynamic-seed-blacklist`) porque cada cenário exercita uma combinação
diferente dessas capabilities.

## What Changes

- Mestre/escravo ganham um modo de teste "canal fixo" (sem hopping,
  permanece em um único canal da tabela) para viabilizar o Cenário A —
  selecionável em runtime, sem exigir novo firmware para o Cenário B/C.
- Mestre passa a logar o tempo de round-trip de cada fragmento confirmado
  (proxy de latência) e a taxa de fragmentos confirmados por segundo
  (proxy de throughput), reaproveitando o cronômetro que já envolve
  `wait_fragment_ack`.
- `tools/spectrum/main.py` (ou um script novo dedicado a cenários) passa a
  ler **duas portas seriais simultaneamente** (analisador/validacao para
  espectro+controle do jammer, escravo para PLR/latência/throughput),
  correlacionando por timestamp.
- Um orquestrador roda os 3 cenários nomeados em sequência por duração
  configurável, rotulando cada amostra com o nome do cenário, salvando um
  CSV consolidado por execução.
- Gera os gráficos pedidos pelo guia: throughput × tempo, latência × tempo
  (antes/durante/depois do ataque), reaproveitando a infraestrutura de
  plot já existente em `ViewTab`.

## Capabilities

### New Capabilities
- `comparative-scenarios`: modo de teste sem hopping no firmware +
  logging de latência/throughput + orquestração e relatório automatizado
  dos 3 cenários comparativos do guia.

### Modified Capabilities
(nenhuma — não altera requisitos de `packet-loss-metric`,
`targeted-jamming`, `attack-detection` ou `adaptive-mitigation`, só os
consome)

## Impact

- `mestre/src/main.cpp` / `escravo/src/main.cpp`: novo modo de operação
  (canal fixo), novo log de latência por fragmento.
- `tools/spectrum/main.py`: leitura multi-porta serial, novo modo de
  orquestração de cenários, novos gráficos de throughput/latência.
- Depende de `plr-metric`, `targeted-jammer`, `spectrum-attack-classification`
  e `dynamic-seed-blacklist` — é o último change da sequência, fecha o
  roadmap do guia.
