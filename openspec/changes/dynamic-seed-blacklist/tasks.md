## 1. `shared/hopping.h` — canal ativo com máscara

- [x] 1.1 Adicionar `get_next_active_channel(seed, counter, mask)`,
      filtrando os índices habilitados por `mask` antes de aplicar o
      módulo; manter `get_next_channel` existente intacto (usado até a
      primeira máscara ser confirmada, ou aposentado se mestre/escravo
      passarem a usar a variante com máscara desde o início com
      `mask = 0xFFFF`).
- [x] 1.2 Definir os novos tipos de pacote de controle: `ControlSeedPacket
      { type, new_seed }`, `ControlBlacklistPacket { type, mask }`,
      `ControlAck { type, echo }` — com `static_assert` de tamanho ≤ 32
      bytes, seguindo o padrão de `SeedPacket`/`SeedAckPacket`.

## 2. `mestre/src/main.cpp` — geração e envio de controle

- [x] 2.1 Ler o pino de alerta (de `spectrum-attack-classification`) no
      loop principal e detectar a transição LOW→HIGH.
- [x] 2.2 Ao detectar a transição, gerar nova seed aleatória e enfileirar
      `ControlSeedPacket` com prioridade sobre o próximo fragmento.
- [x] 2.3 Adicionar `g_channel_fail[TABLE_SIZE]`, incrementado em cada
      timeout de ACK (`wait_fragment_ack` retornando falha) no índice do
      canal atual, decrementado em cada sucesso.
- [x] 2.4 Enquanto o alerta estiver ativo, avaliar `g_channel_fail` contra
      o limiar configurável a cada hop; ao exceder, atualizar
      `g_channel_mask` local (respeitando a guarda de "nunca esvaziar
      todos os canais") e enfileirar `ControlBlacklistPacket`.
- [x] 2.5 Implementar a fila de prioridade simples: se há controle
      pendente (seed ou blacklist), enviar esse pacote no hop atual em vez
      de um fragmento de dado; usar a mesma função de espera de ACK já
      existente para os fragmentos, adaptada para os tipos de controle.
- [x] 2.6 Só aplicar (usar na próxima chamada de
      `get_next_active_channel`) a nova seed/máscara depois do `ControlAck`
      correspondente ser recebido.

## 3. `escravo/src/main.cpp` — recepção e aplicação de controle

- [x] 3.1 Adicionar handler para `ControlSeedPacket`/`ControlBlacklistPacket`
      no loop de recepção, respondendo com `ControlAck` e só então
      atualizando seed/máscara locais usadas por
      `get_next_active_channel`.
- [x] 3.2 Garantir que a atualização de estado (seed/máscara) e o envio do
      ACK aconteçam de forma atômica em relação ao próximo cálculo de
      canal (sem janela em que o escravo aplicou mas o mestre ainda não
      confirmou recepção do ACK).

## 4. Validação manual (self-healing)

- [x] 4.1 Com `plr-metric` e `spectrum-attack-classification` já
      integrados, rodar mestre+escravo sob `targeted-jammer` até o
      `plr_window` subir e o alerta disparar. **Confirmado em 2026-09-23**
      via `tools/validation/run_scenario.py --jammer-mode targeted
      --jammer-power high` (150s), após o fix do detector por-canal em
      `spectrum-attack-classification` (design.md "Achado de calibração
      2026-09-23, parte 3") — `plr_window` subiu de 0,2% pra 35%,
      `alert_on` disparou 2x com evidência real (`mask=0x0001`/canal 10,
      depois canal 22).
- [x] 4.2 Confirmar via log serial do mestre que `MUDAR_SEED` foi
      enfileirado, enviado e confirmado logo após o início do alerta.
      **Confirmado na mesma execução**: `rekey_tx` com `kind=seed`
      enfileirado logo após o primeiro `alert_on`.
- [x] 4.3 Confirmar que, ao longo do ataque contínuo, canais com falha
      severa entram na blacklist (`BLACKLIST_UPDATE` confirmado) e que a
      máscara nunca chega a zero canais habilitados. **Confirmado**: 13
      `rekey_tx` de blacklist, 18 `rekey_ack` confirmados, máscara final
      aplicada `mask=0xFFFB` (14 dos 16 canais habilitados — nunca zerou).
- [ ] 4.4 Registrar `plr_window` antes e depois da mitigação e confirmar
      queda visível (critério de "self-healing" para a monografia) —
      esses números alimentam o cenário C de `scenario-automation`.
      **Tentado em 2026-09-23, não passava**: a mitigação disparou
      corretamente (4.1-4.3), mas ~5s depois o enlace caiu de vez
      (`loss_of_sync`, 2x) e ficou preso tentando repareear pelo resto da
      janela de jamming (150s) sem se recuperar enquanto o ataque
      continuava ativo — sem PLR pós-mitigação pra registrar.

      **Causa raiz provável encontrada e corrigida em 2026-09-24**, ao
      rodar as duas placas reais lado a lado de novo (sessão que também
      produziu `blacklist-decay`): `escravo_advance_hop()` só contava um
      hop como "recebido" quando `g_hop_received_flag` estava setado, e
      esse flag só era setado no recebimento de FRAGMENTO -- nunca no
      recebimento de pacote de controle (`MSG_CTRL_SEED`/
      `MSG_CTRL_BLACKLIST`), mesmo quando o controle chegava e era
      ackado com sucesso (`escravo/src/node.h`). Sob rajada de controle
      -- exatamente o que uma sequência de `rekey_tx` de blacklist sob
      jamming real produz -- isso inflava `g_hops_since_rx` do escravo
      até `LOSS_HOPS` **mesmo o enlace estando funcional**, derrubando o
      pareamento por engano. Um segundo bug relacionado também corrigido:
      `g_hops_since_ack` do mestre nunca era atualizado no caminho de
      pacote de controle (nem em sucesso, nem em falha), então um
      controle que nunca fosse confirmado also nunca acionava o fallback
      de `LOSS_HOPS` do lado do mestre. Ver commit "correcao: 3 bugs
      reais de pareamento/enlace encontrados em bancada ao vivo" e
      `openspec/changes/archive/2026-09-24-adaptive-rendezvous/design.md` (um terceiro bug
      relacionado, de pareamento inicial, também encontrado na mesma
      sessão).

      **Ainda não re-validado sob jamming real** com os três fixes
      juntos -- a sessão de 2026-09-24 encontrou interferência de RF real
      no ambiente (alerta sustentado do analisador mesmo com jammer
      desligado e mitigação desativada) que impediu completar esta
      validação. Próximo passo: repetir `run_scenario.py --jammer-mode
      targeted` com as placas atuais assim que o ambiente permitir, e
      confirmar que `plr_window` cai de fato após a mitigação em vez de o
      enlace cair de vez.
- [ ] 4.5 Calibrar `g_channel_fail` threshold e `ATTACK_STREAK_N` (de
      `spectrum-attack-classification`) em conjunto, já que os dois
      interagem no tempo de reação total. `ATTACK_STREAK_N` mudou de 3
      pra 2 como parte do redesenho do detector (2026-09-23), mas não
      houve calibração conjunta deliberada com `CHANNEL_FAIL_THRESHOLD`
      ainda.
