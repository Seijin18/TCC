## Context

O escravo já espelha o hopping do mestre: mantém seu próprio
`g_hop_counter`/seed e calcula `get_next_channel(seed, counter)` a cada
`HOP_INTERVAL_MS` (220 ms) para saber em qual canal escutar
(`escravo/src/main.cpp`, espelha `mestre/src/main.cpp:396-401`). O
`FragmentPacket` recebido não carrega o contador de hop do mestre — só
`msg_id`, `fragment_index`, `total_fragments`. Ou seja, o escravo não pode
descobrir "quantos hops o mestre já deu" a partir do conteúdo do pacote; só
pode observar, para os hops que ele mesmo percorre em paralelo, se recebeu
algo válido ou não.

## Goals / Non-Goals

**Goals:**
- Medir PLR usando só informação que o escravo já tem localmente (seu
  próprio hop counter), sem exigir mudança no formato dos pacotes do
  mestre.
- Expor o PLR em uma janela deslizante curta (para refletir degradação em
  tempo real sob ataque) e também um PLR acumulado desde o início da sessão
  (para comparação de cenários na Fase 5).
- Log serial em formato estável e parseável (uma linha por janela).

**Non-Goals:**
- Não implementa a troca de seed/blacklisting em resposta ao PLR alto —
  isso é `dynamic-seed-blacklist`.
- Não implementa a automação de cenários que consome esse log — isso é
  `scenario-automation`.
- Não altera o protocolo on-air (`FragmentPacket`/`FragmentAck`).

## Decisions

- **Janela de medição = por hop, não por fragmento**: a cada
  `HOP_INTERVAL_MS`, o escravo já decide (ou deveria decidir) se ficou no
  canal certo tempo suficiente para receber um fragmento. Um contador
  `g_window_expected`/`g_window_received` é incrementado a cada hop
  (esperado sempre) e a cada recepção válida (recebido). Isso evita
  depender de sequência numérica que não existe no pacote hoje.
- **Duas métricas, não uma**: `plr_window` (últimos N hops, ex. N=50 ≈ 11s)
  para refletir ataque em andamento, e `plr_session` (acumulado) para o
  relatório final da Fase 5. Ambas emitidas na mesma linha de log para
  simplificar o parser.
- **Log a cada janela fechada, não a cada hop**: evita inundar a serial em
  220 ms/linha (seria ~4.5 linhas/s) e mantém o log legível durante testes
  manuais.
- **Sem novo campo no pacote**: manter compatibilidade com o mestre atual;
  a métrica é 100% local ao escravo. Isso significa que ela mede "hops em
  que o escravo não recebeu nada", não distingue "mestre não enviou" de
  "pacote se perdeu no ar" — para os fins do TCC (medir degradação sob
  jamming) essa distinção não importa, o efeito observável é o mesmo.

## Risks / Trade-offs

- Contar "hop esperado" a cada `HOP_INTERVAL_MS` do relógio local do
  escravo pode divergir levemente do relógio do mestre ao longo de uma
  sessão longa (sem beacon dedicado, Fase 1 nota isso como gap conhecido).
  Para o N pequeno da janela deslizante (~11s) o desvio acumulado é
  desprezível; não é necessário resolver isso neste change.
- Se o escravo perder o link completamente (handshake caiu), PLR deveria
  saturar em 100% em vez de travar — tratar esse caso explicitamente no
  cálculo (ex.: se não há seed válida, não emitir janela, ou emitir com
  flag `link_down`).
