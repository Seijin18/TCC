## Why

O guia de desenvolvimento (Fase 1) exige uma métrica de qualidade de link
(*Packet Loss Rate* — PLR) calculada no escravo. Hoje ela não existe: só há
um comentário (`escravo/src/main.cpp:13`) mencionando "Cálculo de QoS", sem
nenhum contador real. Sem PLR, não é possível validar quantitativamente nem
a Fase 1 (estabilidade do link básico), nem a Fase 4 (comprovar
"self-healing" após troca de seed/blacklist), nem a Fase 5 (comparar os 3
cenários do guia com um número em vez de observação qualitativa). É a
lacuna que bloqueia a instrumentação de todas as fases seguintes.

## What Changes

- Escravo passa a saber quantos fragmentos eram esperados por janela de
  hopping (ele já segue a mesma sequência de canais do mestre via
  `get_next_channel(seed, counter)`) e quantos de fato chegaram, calculando
  PLR = 1 - (recebidos / esperados) por janela deslizante.
- Escravo loga o PLR periodicamente pela serial em formato parseável (CSV)
  para que `tools/spectrum/main.py` (ou uma ferramenta futura da Fase 5)
  possa capturá-lo.
- Nenhuma mudança de protocolo on-air é necessária: a detecção de perda usa
  o hop counter que o escravo já mantém localmente para seguir o mestre,
  não exige campo novo no `FragmentPacket`.

## Capabilities

### New Capabilities
- `packet-loss-metric`: escravo mede e reporta a taxa de perda de pacotes
  (PLR) por janela de tempo, com saída serial parseável.

### Modified Capabilities
(nenhuma — não há spec existente sendo alterada)

## Impact

- `escravo/src/main.cpp`: novo estado (contadores de janela), nova lógica
  no loop de hopping, novo log serial periódico.
- `tools/spectrum/main.py`: consumidor futuro do log de PLR (fora do
  escopo deste change; ver `scenario-automation`).
- Nenhum impacto em `mestre/`, `jammer/`, `analisador/` ou em
  `shared/hopping.h`.
