# Design — servo de fase de slot

## O problema em uma figura

```
slot k (mestre)    |=========== 30 ms ===========|
slot k (escravo)        |=========== 30 ms ===========|   <- desliza com o tempo
sobreposição            |=======================|        <- só aqui passa dado
```

Com a deriva, a sobreposição encolhe; quando zera, os dois estão em canais
diferentes o slot inteiro → `loss_of_sync`.

## Por que o reanchor atual não pega isso

`escravo_reanchor(master_slot)` só age se `hop_counter != master_slot`.
Mas o escravo só recebe pacotes do mestre enquanto os dois estão no mesmo
canal — isto é, no trecho em que os dois já estão no mesmo slot. Um pacote
com `master_slot` diferente do slot local está, por construção, em outro
canal. Logo o caminho de correção quase nunca dispara sob deriva gradual.

## Decisões

### D1 — O mestre informa a própria fase
O escravo não consegue saber o sinal do desvio só pela hora de chegada (o
mestre retransmite várias vezes dentro do slot). Com a fase do mestre no
pacote:
- escravo adiantado `d` ms: recebe no trecho `fase_mestre ∈ [0, 30−d]`, e
  `fase_escravo = fase_mestre + d` → `e = +d`;
- escravo atrasado `d` ms: `fase_escravo = fase_mestre − d` → `e = −d`.

O desvio sai com sinal, de qualquer pacote recebido.

### D2 — Resolução de 1 ms basta
Deriva típica ~2,4 ms/min → uma correção de 1 ms a cada ~25 s. Com ~100
fragmentos/s, o filtro tem centenas de amostras por correção; o ruído de
quantização (±1 ms de cada lado) some na média. Não precisa trocar a âncora
de `millis()` para `micros()`.

### D3 — Filtro e passo
- Média móvel exponencial do desvio, α = 1/8.
- Quando `|e_filtrado − e_alvo| ≥ 1 ms`: `hop_start_at += sinal` (1 ms) e o
  acumulador do filtro é deslocado de 1 ms.
- No máximo um passo a cada 200 ms (limite de taxa contra oscilação).
- Sinal: se `e` cresce (pacotes chegando mais tarde no slot do escravo),
  aumentar `hop_start_at` atrasa a fronteira de slot do escravo e reduz `e`.

### D4 — Alvo absorve a latência fixa
`e_alvo` = média dos primeiros N desvios medidos logo depois de
`time_sync` (âncora recém-alinhada). Absorve tempo de ar, settle de PLL e
processamento sem calibrar constante por placa.

### D5 — Guardas
- `|e| ≥ SLOT_MS/2` numa amostra isolada → ignorar (pacote atípico); o
  reanchor de slot inteiro continua tratando slip real.
- Se depois de correções o desvio filtrado continuar crescendo além de
  10 ms, desligar o servo, emitir `e=phase;state=disabled` e cair no
  comportamento atual (reanchor + re-pareamento) — proteção contra erro de
  sinal.

### D6 — Lógica pura
`phase_servo_update(state, e_ms, now_ms) -> adj_ms` em `shared/time_sync.h`,
sem Arduino, testada em `env:native_test`:
- converge para deriva constante de ±50 ppm simulada por 30 min;
- passo nunca maior que 1 ms nem mais frequente que o limite;
- ruído uniforme ±2 ms não gera correções espúrias;
- amostra ≥ meio slot é ignorada.

## Alternativa descartada
Reancorar com ganho 1 a cada pacote (`hop_start_at = agora − master_slot·SLOT_MS
− fase_mestre`): mais simples, mas transfere o jitter de cada pacote (±1 ms)
direto para a fronteira de slot. O filtro dá o mesmo resultado sem jitter.

## Riscos
- Períodos só com pacotes de controle (rekey): sem `FragmentPacket` o servo
  pausa — curtos, sem efeito prático.
- Jamming reduz amostras: o filtro só fica mais lento, a deriva é lenta.
- Tamanho do pacote: 31 bytes; `static_assert(sizeof(FragmentPacket) <= 32)`.
