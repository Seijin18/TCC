## Why

A Fase 2 do guia pede dois modos de jammer: *Sweep* (varredura ampla) e
*Targeted* (mira nos canais da tabela de hopping, simulando um atacante que
já descobriu parte do padrão). Hoje só o Sweep existe
(`jammer/src/main.cpp`, `validacao/src/main.cpp`) — ambos varrem toda a
banda BT (2–80) ou um canal aleatório dentro dela, nunca especificamente os
16 canais de `shared/hopping.h`. Sem o modo Targeted, o cenário B da Fase 5
("Hopping Fixo vs Jammer Target") não tem jammer nenhum para rodar contra, e
a heurística de classificação de ataque da Fase 3
(`spectrum-attack-classification`) não tem um sinal de "ataque dirigido"
real para diferenciar de ruído aleatório.

## What Changes

- `jammer/src/main.cpp` ganha um modo `TARGETED` que varre round-robin
  apenas os 16 canais de `HOPPING_CHANNELS` (`shared/hopping.h`), em vez da
  faixa BT inteira — reaproveitando a mesma lógica de dwell/dual-rádio já
  existente, só trocando a lista de canais percorridos.
- Seleção de modo em runtime via comando serial (`mode sweep` / `mode
  targeted`), não só compile-time — para poder alternar durante uma sessão
  de coleta sem regravar o firmware.
- `validacao/src/main.cpp` ganha o mesmo comando de modo, reaproveitado
  pela função `stepJammer`/`jammerStart` que hoje só sorteia canal dentro
  de `BT_CH_MIN..BT_CH_MAX`.
- Nenhuma sincronização com a seed do mestre é implementada aqui — o
  guia descreve o Targeted como mira na *tabela conhecida* de 16 canais,
  não como perseguição reativa do hop atual (isso ficaria fora de escopo,
  não é pedido pelo guia).

## Capabilities

### New Capabilities
- `targeted-jamming`: modo de jamming que ataca especificamente os canais
  da tabela de hopping compartilhada, selecionável em runtime via serial,
  disponível tanto no jammer standalone quanto no harness de validação.

### Modified Capabilities
(nenhuma)

## Impact

- `jammer/src/main.cpp`: novo estado de modo, novo parser de comando
  serial (hoje não existe nenhum), nova função de avanço de canal
  restrita à tabela.
- `validacao/src/main.cpp`: extensão do `handleCommand` já existente com
  o novo comando `mode`, nova função `randomTargetedChannel()` análoga a
  `randomBtChannel()`.
- `shared/hopping.h`: nenhuma mudança de conteúdo, só passa a ser incluído
  por `jammer/` e `validacao/` (já está no include path global do
  `platformio.ini`).
