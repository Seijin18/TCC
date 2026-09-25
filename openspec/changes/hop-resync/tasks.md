## 1. Implementação

- [x] 1.1 `shared/protocol.h`: `RESYNC_MISS_HOPS` (2), `RESYNC_PARK_MAX_MS`
      e o `static_assert` do orçamento (design.md D2).
- [x] 1.2 `escravo/src/node.h`: `g_last_good_channel` (atualizado a cada
      `FragmentPacket` válido), contador de timeouts consecutivos, estado
      estacionado (escuta contínua no canal bom, sem `escravo_advance_hop()`
      por timeout; hops "virtuais" por tempo para PLR/`g_hops_since_rx`);
      ao receber fragmento válido estacionado: sai do estado, adota
      `hop_counter`, acka, avança — mesmo caminho de sucesso de hoje.
      Estacionamento esgotado ⇒ `reset_to_scanning()`.
- [x] 1.2a `ControlSeedPacket`/`ControlBlacklistPacket` ganham
      `hop_counter` (design.md D3a); mestre preenche, escravo adota ao
      receber (estacionado ou não) e sai do estacionamento com eles também.
- [x] 1.3 Telemetria `resync_park` (`ch`, `hop`) e `resync_ok` (`ch`,
      `hop_before`, `hop_after`, `ms`); logger leva os dois ao
      `events.log`.
- [x] 1.4 Se a lógica de decisão couber numa função pura (ex. "estacionar
      agora?"/"orçamento"), extrair para `shared/` e testar em
      `native_test`; `pio run` (todos os envs) e `pio test` verdes.

## 2. Validação em bancada

- [ ] 2.1 Soak de 10 min com jammer desligado (Wi-Fi ambiente atual): meta
      **0 `loss_of_sync`**; registrar quantos `resync_park`/`resync_ok`
      ocorreram e a distribuição de `hop_after - hop_before` (confirma o
      caso "ack perdido" = escravo adiantado).
      **1ª tentativa (2026-09-24 15:56-15:59, interrompida)**: 11
      `resync_park`, **0 `resync_ok`**, toda estacionada seguida de
      `loss_of_sync` ~4 s depois (limite do estacionamento). 9 de 11
      estacionaram em ch 34 (e as outras em 70), justamente os canais
      colados no Wi-Fi: no caso "ack perdido", o último canal onde o
      escravo recebeu **é** o canal ruidoso cujo ack se perdeu, então
      `g_last_good_channel` aponta para o pior canal e a única passada do
      mestre por ele (~220 ms a cada ~16 hops) falha de novo. Correção
      planejada (tarefa 1.5): escolher o canal de estacionamento pela
      qualidade de recepção recente por canal, não pelo mais recente.
- [ ] 1.5 *(achado de 2.1)* Escolha do canal de estacionamento por
      qualidade: contadores ok/miss por posição da tabela com decaimento
      simples; função pura `resync_pick_park_channel(ok, miss, mask,
      fallback)` em `shared/resync.h` (score tipo `ok - 2×miss`, só canais
      habilitados com `ok > 0`, desempate determinístico) + testes nativos;
      `resync_park` ganha `score=`.
- [ ] 2.2 Jammer `adaptive` e `targeted` por 2-3 min cada: registrar
      quedas, ressincronizações e PLR contra o mesmo teste sem este change
      (`targeted-jammer` 4.5, sessão 2026-09-24 15:39).
- [ ] 2.3 Copiar as sessões que qualificarem para `logs/working/`.

## 3. Documentação

- [ ] 3.1 ROADMAP; sincronizar spec; validar; arquivar.
