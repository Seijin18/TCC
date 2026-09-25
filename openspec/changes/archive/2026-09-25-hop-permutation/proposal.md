## Why

A primeira bateria comparativa A/B/C (`scenario-automation` 4.2,
`logs/battery-20260925-041711`) expôs uma fraqueza do próprio FHSS: a
sequência de salto é `HOPPING_CHANNELS` percorrida **sempre na ordem da
tabela** — `get_next_active_channel(seed, counter, mask)` devolve
`ativos[(seed + counter) % n]`, então a seed só desloca a fase, nunca a
ordem. O jammer `targeted`, que também percorre a tabela na mesma ordem
(60 ms por canal), colidiu sistematicamente com o enlace: com hopping sem
mitigação (cenário B) a perda sob jamming foi 16,7%, enquanto o canal
fixo (cenário A) passou ileso (0%). Qualquer atacante que conheça a
tabela (ela está no código público do TCC) e ande na mesma ordem
acompanha o enlace sem precisar sensoriar nada.

## What Changes

- A ordem de visita dos canais ativos passa a ser uma **permutação
  pseudoaleatória determinística derivada da seed** (Fisher-Yates sobre as
  posições ativas, com um PRNG inteiro simples semeado pela seed), fixa
  para uma dada `(seed, máscara)`. Canal do hop `n` =
  `ativos[perm_seed[n % ativos]]`.
- Continua valendo: cada canal ativo aparece **exatamente uma vez por
  ciclo** de `ativos` hops (necessário para o orçamento de estacionamento
  de `hop-resync`, que assume que o mestre volta a qualquer canal ativo em
  ≤ `ativos` hops); mestre e escravo calculam a mesma sequência (função
  pura em `shared/hopping.h`); trocar a seed (rekey) troca a ordem, não só
  a fase.
- Nenhum pacote muda; mestre e escravo precisam ser gravados juntos
  (sequência nova).

## Capabilities

### New Capabilities
- `hop-sequence`: ordem de salto dependente da seed.

## Impact

- `shared/hopping.h`: `get_next_active_channel()` (mesma assinatura).
- `test/test_hopping`: testes que dependam da ordem da tabela precisam
  ser revistos; novos testes de permutação.
- Validação: repetir a bateria A/B/C com `targeted` (esperado: B deixa de
  ser acompanhado pelo jammer) e o soak de 10 min sem jammer (sem
  regressão de `hop-resync`).
