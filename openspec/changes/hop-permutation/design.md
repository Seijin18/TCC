## Context

`shared/hopping.h`:

```cpp
uint8_t pos = (uint8_t)((seed + counter) % active_count);
return HOPPING_CHANNELS[active_indices[pos]];
```

A seed só escolhe por onde a volta começa. `targeted` (jammer) percorre
`HOPPING_CHANNELS[idx++]` com 60 ms por canal — mesma ordem. Resultado de
bancada em `scenario-automation/tasks.md` 4.2.

## Decisões

### D1 — Permutação fixa por seed, não reembaralhar por ciclo

`perm = fisher_yates(0..n-1, prng(seed))`; canal = `ativos[perm[counter %
n]]`. Reembaralhar a cada ciclo (`counter / n`) deixaria a ordem ainda
menos previsível, mas o intervalo entre duas visitas ao mesmo canal
poderia chegar a `2n - 1` hops — estouraria o orçamento de estacionamento
de `hop-resync` (`RESYNC_PARK_MAX_MS = (TABLE_SIZE + 2) × HOP_TIMEOUT_MS`,
calculado para `n`). Com permutação fixa, o intervalo é sempre exatamente
`n`. Para o escopo do TCC (jammer que conhece a tabela mas não a seed),
basta.

### D2 — PRNG

Inteiro de 32 bits, determinístico e idêntico em ESP32 e no ambiente
nativo de teste (nada de `random()`/`esp_random()`): xorshift32 semeado
com `seed` (misturado com uma constante para a seed 0 não degenerar).
Fisher-Yates sobre no máximo 16 posições por hop — custo desprezível;
se medir diferente, dá para cachear por `(seed, mask)`.

### D3 — Máscara

A permutação é sobre as posições **ativas** (depois da máscara), então
trocar a máscara (blacklist) muda `n` e a permutação no mesmo hop nos dois
lados (a troca já é sincronizada por `apply_at_hop`).

## Riscos

- Sequência nova ⇒ gravar mestre e escravo juntos.
- O jammer `adaptive` não é afetado (ele sensoria em vez de seguir a
  tabela) — esperado; este change mira atacantes que seguem a tabela.
