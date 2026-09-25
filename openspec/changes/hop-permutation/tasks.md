## 1. Implementação

- [x] 1.1 `shared/hopping.h`: xorshift32 + Fisher-Yates; `get_next_active_channel()`
      devolve `ativos[perm_seed[counter % n]]` (design.md D1-D3), mesma
      assinatura e mesma guarda de `mask == 0`.
- [x] 1.2 Testes nativos: determinismo; cada canal ativo exatamente uma
      vez por ciclo; seeds diferentes ⇒ ordens diferentes (para a maioria
      das seeds); máscara respeitada; seed 0 não degenera na ordem da
      tabela; revisar testes existentes que assumem ordem da tabela.
- [x] 1.3 `pio run` (todos os envs) e `pio test` verdes.

## 2. Validação em bancada

- [x] 2.1 Soak de 10 min sem jammer: sem regressão (0 `loss_of_sync`,
      `resync_ok` ≈ `resync_park`).
      **2026-09-25 02:16-02:26**: 0 `loss_of_sync`, 41/41 resync, 2.913
      `msg_done`, 8,0% de miss (Wi-Fi em 27/34), 44 máscaras aplicadas.
- [ ] 2.2 Bateria A/B/C com `targeted` (mesmos parâmetros da de
      2026-09-25 01:37): comparar a perda do cenário B com a anterior
      (16,7%).
      **1ª tentativa (`logs/battery-20260925-044627`), inconclusiva**: B
      caiu para 7,1% — mas o cenário A (canal fixo, que nem usa a sequência
      de salto) foi de 0% para 22,5% de miss sob o "mesmo" jammer, e o
      perfil de miss por canal de B/C mudou por completo em relação à
      rodada anterior (antes ~12-19% espalhado; agora só o Wi-Fi em 34).
      Somado ao teste de modos das 01:08 (70-100% em 15-82), o `targeted`
      produziu três ataques diferentes no mesmo dia: o jammer não é
      reprodutível, então a queda de B não pode ser atribuída a este
      change. Ver `targeted-jammer` (retune em modo portadora constante).

## 3. Documentação

- [ ] 3.1 Registrar resultado; ROADMAP; validar; arquivar.
