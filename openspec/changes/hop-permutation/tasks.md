## 1. Implementação

- [ ] 1.1 `shared/hopping.h`: xorshift32 + Fisher-Yates; `get_next_active_channel()`
      devolve `ativos[perm_seed[counter % n]]` (design.md D1-D3), mesma
      assinatura e mesma guarda de `mask == 0`.
- [ ] 1.2 Testes nativos: determinismo; cada canal ativo exatamente uma
      vez por ciclo; seeds diferentes ⇒ ordens diferentes (para a maioria
      das seeds); máscara respeitada; seed 0 não degenera na ordem da
      tabela; revisar testes existentes que assumem ordem da tabela.
- [ ] 1.3 `pio run` (todos os envs) e `pio test` verdes.

## 2. Validação em bancada

- [ ] 2.1 Soak de 10 min sem jammer: sem regressão (0 `loss_of_sync`,
      `resync_ok` ≈ `resync_park`).
- [ ] 2.2 Bateria A/B/C com `targeted` (mesmos parâmetros da de
      2026-09-25 01:37): comparar a perda do cenário B com a anterior
      (16,7%).

## 3. Documentação

- [ ] 3.1 Registrar resultado; ROADMAP; validar; arquivar.
