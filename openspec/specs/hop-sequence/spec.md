# hop-sequence Specification

## Purpose
TBD - created by archiving change hop-permutation. Update Purpose after archive.
## Requirements
### Requirement: Ordem de salto depende da seed
O sistema DEVE (MUST) visitar os canais ativos numa ordem pseudoaleatória
determinada pela seed — não apenas deslocada por ela —, calculada da
mesma forma pelo mestre e pelo escravo a partir de `(seed, contador de
hop, máscara)`.

#### Scenario: Seeds diferentes produzem ordens diferentes
- **WHEN** duas seeds diferentes são usadas com a mesma máscara
- **THEN** as sequências de canais de um ciclo diferem na ordem, e não
  apenas no ponto de partida

#### Scenario: Troca de seed muda a ordem
- **WHEN** o mestre aplica uma nova seed num hop acordado
- **THEN** a partir desse hop os dois lados seguem a nova ordem

### Requirement: Cada canal ativo aparece uma vez por ciclo
O sistema DEVE (MUST) garantir que, para uma seed e máscara fixas, cada
canal ativo apareça exatamente uma vez a cada `n` hops consecutivos (onde
`n` é o número de canais ativos).

#### Scenario: Ressincronização continua dentro do orçamento
- **WHEN** o escravo estaciona num canal ativo para ressincronizar
- **THEN** o mestre passa por esse canal em no máximo `n` hops

