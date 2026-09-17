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

- [ ] 4.1 Com `plr-metric` e `spectrum-attack-classification` já
      integrados, rodar mestre+escravo sob `targeted-jammer` até o
      `plr_window` subir e o alerta disparar.
- [ ] 4.2 Confirmar via log serial do mestre que `MUDAR_SEED` foi
      enfileirado, enviado e confirmado logo após o início do alerta.
- [ ] 4.3 Confirmar que, ao longo do ataque contínuo, canais com falha
      severa entram na blacklist (`BLACKLIST_UPDATE` confirmado) e que a
      máscara nunca chega a zero canais habilitados.
- [ ] 4.4 Registrar `plr_window` antes e depois da mitigação e confirmar
      queda visível (critério de "self-healing" para a monografia) —
      esses números alimentam o cenário C de `scenario-automation`.
- [ ] 4.5 Calibrar `g_channel_fail` threshold e `ATTACK_STREAK_N` (de
      `spectrum-attack-classification`) em conjunto, já que os dois
      interagem no tempo de reação total.
