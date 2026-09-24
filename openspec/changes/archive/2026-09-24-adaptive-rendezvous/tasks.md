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
      repetível.
**Achado de bancada (2026-09-24), relevante para 6.3-6.4**: reproduzido um
deadlock real de pareamento -- `try_establish_link()` no mestre espera
`MSG_LINK_ACK` por `LINK_ACK_WAIT_MS`, mas o escravo declarava
`link_established = true` assim que CONSEGUIA TRANSMITIR esse ack
(`radio_send()` é NO_ACK, sem confirmação real de entrega), sem esperar
nenhuma confirmação de que o mestre de fato recebeu. Se o ack chegasse
fora da janela do mestre (perda de pacote, timing), o escravo ficava preso
permanentemente esperando uma seed que nunca chegaria — ele só volta a
responder a `MSG_LINK_PROBE` no ramo `if (!link_established)`, que nunca
mais era reavaliado. Reproduzido em bancada: escravo silencioso por >1 min
após um `pair_ok` que o mestre nunca confirmou (nenhum `pair_ok`/`seed`
correspondente do lado do mestre). **Corrigido**: `SEED_WAIT_TIMEOUT_MS`
(`shared/protocol.h`) — o escravo volta a escanear se a seed não chegar a
tempo, dando ao par uma nova chance. Ver commit "correcao: 3 bugs reais de
pareamento/enlace encontrados em bancada ao vivo".

Também observado na mesma sessão (não corrigido, natureza incerta):
`measure_and_pick_candidate()` no mestre convergiu quase sempre (>90%)
para o mesmo candidato (76) em duas sessões de boot distintas, mesmo após
aumentar `PAIR_MEASURE_DWELL_US` de 130 para 500 (mesmo valor já usado no
salto de canal de dados, testado por hipótese de PLL não assentado — não
resolveu). Testado também com `mitigation off` (elimina qualquer
tráfego de controle do mestre) sem melhora, e o analisador mostrava
alerta sustentado o tempo todo (mesmo sem jammer e sem mitigação reagindo)
— aponta para interferência de RF real no ambiente físico naquele
momento, não um bug de software, mas fica registrado sem conclusão
definitiva porque não foi possível inspecionar o ambiente fisicamente
durante a sessão.

**Correção (2026-09-24, mais tarde)**: o parágrafo acima e o "achado"
anterior sobre interferência de RF ambiente estavam errados — ver
`pairing-handshake-hardening` (design.md "Evidências" e tasks.md 1.8/2.5):
rádio do escravo surdo pós-salto (só power-cycle limpa), `SeedAck` perdido
por print entre envio e escuta, e acampamento menor que o ciclo de
varredura. As tarefas 6.2-6.5 abaixo foram fechadas com o firmware daquele
change.

- [x] 6.2 Jammer sweep ativo **antes** do boot das duas placas: confirmar
      que o escravo varre, evita candidatos ruidosos, e o par converge num
      candidato fora de 2–80.
      Fechado com critério revisado — ver `pairing-handshake-hardening`
      5.4: na placa B o jammer não pode estar ativo antes do boot (volta
      desligado e só arma após o 1º handshake); com sweep PA_LOW ativo
      durante re-pareamentos, o pareamento convergiu 7/7, em geral em ch 76
      (dentro da faixa, mas com dado fluindo de fato ali).
- [x] 6.3 Forçar perda de sincronismo (desligar uma placa por alguns
      segundos e religar): confirmar que a outra volta ao estado de
      pareamento e o enlace se recupera sem intervenção manual.
      `pairing-handshake-hardening` 5.1 (20/20 perdas forçadas) e 5.3 (5/5
      reboots assimétricos, placa segurada em reset 5 s).
- [x] 6.4 Rodar por vários minutos com tráfego contínuo, confirmar PLR
      mensurável de forma sustentada (pré-requisito para `plr-metric` seção
      3 e para qualquer teste de `dynamic-seed-blacklist`). Hopping por
      evento (2026-09-18, ver design.md "Correção 2") já mostrou PLR de
      sessão caindo pra ~0,6% num teste curto (~1-2 min, jammer
      desligado) — falta confirmar isso sustentado por vários minutos
      antes de marcar concluído.
      `logs/working/session-20260918-031422.jsonl`: 725 s de enlace
      contínuo sem perda, 12.742 `msg_done`, 642 linhas `plr` — janela
      média 0,29%, sessão final 0,3% (pico de janela isolado 26%).
- [x] 6.5 Confirmar em log que o `hop_counter` do escravo converge para o
      do mestre mesmo partindo de valores diferentes após um reboot
      assimétrico.
      Evidência indireta (o escravo não loga o próprio contador): o canal
      de cada hop é função de `(seed, hop_counter)`, e após cada um dos 5
      reboots assimétricos de `pairing-handshake-hardening` 5.3 o escravo
      voltou a completar mensagens (`msg_done`) em ~1 s — impossível com
      contadores divergentes.

## 7. Blacklist de candidatos de pareamento (adicionado 2026-09-24, ver design.md "Implementado depois")

- [x] 7.1 Generalizar `update_channel_mask()` (`shared/hopping.h`) com um
      parâmetro `count` explícito (default `TABLE_SIZE`), preservando o
      comportamento de todas as chamadas existentes.
- [x] 7.2 2 novos testes em `test/test_hopping.cpp` confirmando exclusão/
      reinclusão/guarda com `count=8` (menor que `TABLE_SIZE`).
- [x] 7.3 `mestre/src/node.h`: `g_candidate_mask`/`g_candidate_fail`/
      `g_candidate_quarantined_since`/`g_pair_round`, `note_candidate_result()`
      chamado em (a) janela de camping esgotada sem `pair_ok`, (b)
      `send_seed_to_slave()` falhando (seed nunca confirmada), (c) seed
      confirmada com sucesso (zera o histórico do candidato).
      `measure_and_pick_candidate()` só considera candidatos habilitados
      em `g_candidate_mask`.
- [x] 7.4 `pio run -e mestre -e placa_a` + `pio test -e native_test` (20/20)
      limpos.
- [x] 7.5 Validação em bancada: confirmado via telemetria `pair_mask` real
      que exclusão/reinclusão/guarda funcionam corretamente em hardware
      (`0xFF→0xFE→0xFC→0xF8→0xF0→...→0xE1→0xE2→0xE4`, nunca chegou a 0).
      **Ainda não confirmado**: se isso, sozinho, é suficiente para reduzir
      o tempo médio de pareamento em condições de RF ruidosas mas não sob
      ataque deliberado — a sessão de validação encontrou dificuldade
      persistente de pareamento mesmo com este mecanismo funcionando
      corretamente (ver design.md, aponta pra ruído afetando vários
      candidatos simultaneamente, não pra este mecanismo estar errado).
