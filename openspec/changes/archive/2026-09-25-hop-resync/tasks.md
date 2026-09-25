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

- [x] 2.1 Soak de 10 min com jammer desligado (Wi-Fi ambiente atual): meta
      **0 `loss_of_sync`**; registrar quantos `resync_park`/`resync_ok`
      ocorreram e a distribuição de `hop_after - hop_before` (confirma o
      caso "ack perdido" = escravo adiantado).
      **2026-09-25 00:52-01:02 (com 1.8)**: 0 loss_of_sync em 10 min, 49/49
      resync_ok (mediana 2,2 s), 2.612 msg_done, 77 trocas de máscara
      aplicadas nos dois lados. (Esta soak ainda rodou com os experimentos
      1.6/1.7 presentes no firmware -- removidos depois, ver notas neles.)
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
- [x] 1.5 *(achado de 2.1)* Escolha do canal de estacionamento por
      qualidade: contadores ok/miss por posição da tabela com decaimento
      simples; função pura `resync_pick_park_channel(ok, miss, mask,
      fallback)` em `shared/resync.h` (score tipo `ok - 2×miss`, só canais
      habilitados com `ok > 0`, desempate determinístico) + testes nativos;
      `resync_park` ganha `score=`.
- [x] 1.6 (experimento) A/B de recuperação do rádio na entrada do
      estacionamento: mesmo com canal de qualidade escolhido (1.5), o log
      do mestre confirma TX no canal estacionado (82/94/125, limpos)
      durante a janela de park sem nenhuma recepção do lado do escravo (0
      `resync_ok`) -- achado anterior (pairing-handshake-hardening) viu o
      mesmo tipo de "surdez" pós-hop com registradores idênticos ao boot,
      só limpa por `radio_reinit()` completo ou reboot; alterna
      `startListening()` simples ("listen") vs. `radio_reinit()` completo
      ("reinit") a cada entrada em park, com dump de registradores
      (`tlm_radio_regs`, `why=park_pre`) antes de aplicar e `fix=` em
      `resync_park`/`resync_ok` pra comparar qual variante recupera a
      recepção.
      Removido após 1.8 (soak: as duas variantes recuperaram igual, 49/49)
      -- experimento baseado em diagnóstico errado.
- [x] 1.7 (experimento) mestre reinicializa o rádio após 3 hops sem ack:
      evidência de bancada 2026-09-25 -- mesmo com o escravo estacionado e
      recém-reiniciado (1.6), ele não recebe nada enquanto o log do mestre
      mostra TX no mesmo canal, sugerindo que é o rádio do MESTRE que trava
      (TX NO_ACK "sucede" sem nada ser irradiado); `radio_reinit()` a cada 3
      misses consecutivos (3, 9, 15, 21, sempre antes de `LOSS_HOPS`=25),
      sem tocar no estado de salto.
      Removido após 1.8 (soak: as duas variantes recuperaram igual, 49/49)
      -- experimento baseado em diagnóstico errado.
- [x] 1.8 (bug de bancada) escravo estacionado nunca lia o rádio: o
      `if (g_resync_parked) { if (timed_out) {...} } else if (timed_out)
      {...} else {...}` aninhado deixava o caso comum (parked==true,
      timed_out==false) sem nenhum `if` que disparasse, então o `else` de
      recepção (`available()`/`read`) jamais rodava enquanto estacionado;
      corrigido calculando `timed_out` uma vez e testando
      `timed_out && g_resync_parked` / `timed_out` / `else` no mesmo nível.
      Isso invalida as conclusões dos experimentos 1.6/1.7 -- a "surdez"
      pós-hop observada ali não era o rádio travado, era o escravo
      estacionado simplesmente nunca chamando `available()`.
- [x] 2.2 Jammer `adaptive` e `targeted` por 2-3 min cada: registrar
      quedas, ressincronizações e PLR contra o mesmo teste sem este change
      (`targeted-jammer` 4.5, sessão 2026-09-24 15:39).
      **Resultado (2026-09-25 01:08-01:20, PA_LOW, 3 min por fase)**:

      | Fase | miss % | quedas | resync ok/park | máscaras aplicadas | msg_done |
      |---|---|---|---|---|---|
      | off | 9,5 | 0 | 17/16 | 18 | 775 |
      | targeted | 43,6 | 0 | 69/70 | 86 | 125 |
      | adaptive | 20,8 | 3 | 26/30 | 19 | 356 |
      | sweep 2-80 | 57,0 | 6 | 30/37 | 25 | 63 |

      Antes deste change (sessão 2026-09-24 15:39, `adaptive`): 3 quedas em
      65 s e 2 quedas em 20 s *sem* jammer. Agora o enlace sobrevive ao
      `targeted` sem nenhuma queda. Perfil de miss por canal: no `sweep`
      (só 2-80) os canais 94-125 também perdem 29-63% — dessensibilização
      de campo próximo do escravo pelo jammer na mesma placa (ver
      `targeted-jammer` 3.2/4.5), contra a qual blacklist não tem para
      onde fugir.
- [x] 2.3 Copiar as sessões que qualificarem para `logs/working/`.
      `session-20260925-004847` (soak) e `session-20260925-010250` (modos do jammer).

## 3. Documentação

- [x] 3.1 ROADMAP; sincronizar spec; validar; arquivar.
