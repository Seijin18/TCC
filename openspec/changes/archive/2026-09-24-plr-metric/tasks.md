## 1. Contadores de janela no escravo

- [x] 1.1 Adicionar estado global (`g_window_expected`, `g_window_received`,
      `g_session_expected`, `g_session_received`, `g_window_size = 50`) em
      `escravo/src/main.cpp`, resetado quando uma nova sessão de hopping
      começa (seed nova aceita).
- [x] 1.2 No loop de hopping do escravo, ao avançar de hop
      (`HOP_INTERVAL_MS` decorrido), incrementar `g_window_expected` e
      `g_session_expected` incondicionalmente.
- [x] 1.3 Ao receber um `FragmentPacket` válido dentro do hop atual,
      incrementar `g_window_received` e `g_session_received` (uma vez por
      hop, não por retransmissão).

## 2. Cálculo e log de PLR

- [x] 2.1 Ao fechar uma janela (`g_window_expected >= g_window_size`),
      calcular `plr_window` e `plr_session`, emitir linha de log serial no
      formato definido no spec (ex.:
      `PLR;t_ms=<millis>;window=<pct>;session=<pct>;recv=<n>;exp=<n>`), e
      resetar os contadores de janela (mantendo os de sessão).
- [x] 2.2 Garantir que nenhuma linha de PLR seja emitida antes do handshake
      de seed completar (guard na função de log).
- [x] 2.3 Tratar divisão por zero / janela vazia sem travar o loop.

## 3. Validação manual

- [x] 3.1 Rodar mestre+escravo sem jammer por alguns minutos e confirmar
      `plr_window` ≈ 0% de forma consistente no log serial. **Confirmado
      em 2026-09-18** via `tools/validation/run_scenario.py` (fase
      baseline) — `plr_window` entre 0% e 0,6% em várias execuções.
- [x] 3.2 Rodar com o jammer sweep atual (`jammer/src/main.cpp`) ativo e
      confirmar que `plr_window` sobe visivelmente enquanto o ataque está
      ativo, servindo de baseline para os changes seguintes
      (`dynamic-seed-blacklist`, `scenario-automation`). **Confirmado em
      2026-09-18**: `plr_window` subiu para 3,5–12,4% sob jamming (sweep e
      targeted, potência baixa e alta) — ver
      `openspec/changes/targeted-jammer/tasks.md` 3.3 para o número sob
      targeted especificamente.
