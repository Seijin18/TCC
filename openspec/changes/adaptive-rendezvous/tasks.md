## 1. `shared/protocol.h` — extração sem mudança de comportamento

- [x] 1.1 Mover `MessageType`, `FragmentPacket`, `FragmentAck`, `LinkPacket`,
      `SeedAckPacket`, `FRAGMENT_DATA_SIZE` e os `static_assert` de
      `mestre/src/node.h`/`escravo/src/node.h` para `shared/protocol.h`
      (definição única, incluída pelos dois).
- [x] 1.2 Confirmar que `pio run -e mestre -e escravo` compila sem mudança
      de comportamento (mesmo canal 76 fixo ainda funciona) antes de tocar
      em qualquer lógica nova.

## 2. Candidatos de pareamento e assimetria acampar/varrer

- [x] 2.1 `PAIRING_CANDIDATES[8]` em `shared/protocol.h`.
- [x] 2.2 Mestre: medir atividade de cada candidato (`testCarrier()`) antes
      de escolher onde acampar; reenviar `MSG_LINK_PROBE` a
      `LINK_RETRY_MS`; sem ACK após `PAIR_CAMP_MS`, rodar para o próximo
      candidato limpo.
- [x] 2.3 Escravo: varrer os candidatos, `PAIR_DWELL_MS` escutando em cada.
- [x] 2.4 Emitir telemetria (`@<src>;e=pair_scan`/`pair_ok`, se
      `telemetry-logging` já estiver disponível; caso contrário
      `Serial.printf` equivalente) em cada mudança de candidato e ao
      confirmar link.

## 3. Fallback de perda de sincronismo

- [x] 3.1 Escravo: contador de hops sem `FragmentPacket` válido; ao atingir
      `LOSS_HOPS`, voltar ao estado de varredura (mesmo caminho de 2.3).
- [x] 3.2 Mestre: contador de hops sem `FragmentAck`; ao atingir
      `LOSS_HOPS`, voltar ao estado de acampar (mesmo caminho de 2.2).
- [x] 3.3 Garantir que o reset de estado limpa seed/hop_counter/máscara
      locais, para o próximo pareamento não herdar estado da sessão
      anterior.

## 4. Tráfego contínuo e seed não-bloqueante

- [x] 4.1 Mestre: remover `g_done`; ao completar uma mensagem, incrementar
      `msg_id`, zerar `g_current_fragment` e continuar enviando.
- [x] 4.2 `wait_for_seed()`: janela curta (~3 s) com valor default; aceitar
      seed digitada nesse intervalo sem bloquear `setup()` indefinidamente.
- [x] 4.3 Adicionar comando runtime `seed <n>` (mestre) para trocar a seed
      inicial sem reiniciar a placa — útil para os testes graduais.

## 5. Resync de contador de hop

- [x] 5.1 Adicionar `uint32_t hop_counter` ao `FragmentPacket`
      (`shared/protocol.h`); confirmar `static_assert(sizeof(...) <= 32)`
      continua válido.
- [x] 5.2 Escravo: ao receber um `FragmentPacket` válido, adotar
      `pkt.hop_counter` como seu próprio `hop_counter` em vez de manter
      contagem independente.
- [x] 5.3 Mestre: preencher `hop_counter` a cada envio com o valor de
      `g_hop_counter` corrente.

## 6. Validação manual em bancada

- [x] 6.1 Jammer desligado: confirmar pareamento no candidato 76 (primeiro
      da lista) igual ao comportamento anterior. Confirmado em
      2026-09-17 após remover `enableAckPayload()` (bug real, ver
      design.md "Correção") — `pair_ok`/`seed` em ambos os lados,
      repetível. Fase de hopping ativo (6.4) segue bloqueada por um
      problema separado e ainda não resolvido (ver design.md).
- [ ] 6.2 Jammer sweep ativo **antes** do boot das duas placas: confirmar
      que o escravo varre, evita candidatos ruidosos, e o par converge num
      candidato fora de 2–80.
- [ ] 6.3 Forçar perda de sincronismo (desligar uma placa por alguns
      segundos e religar): confirmar que a outra volta ao estado de
      pareamento e o enlace se recupera sem intervenção manual.
- [ ] 6.4 Rodar por vários minutos com tráfego contínuo, confirmar PLR
      mensurável de forma sustentada (pré-requisito para `plr-metric` seção
      3 e para qualquer teste de `dynamic-seed-blacklist`).
- [ ] 6.5 Confirmar em log que o `hop_counter` do escravo converge para o
      do mestre mesmo partindo de valores diferentes após um reboot
      assimétrico.
