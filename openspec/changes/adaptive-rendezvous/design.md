## Context

Pareamento hoje: mestre e escravo escutam/sondam um único canal fixo
(`PAIRING_CHANNEL = 76`). Isso é uma superfície de ataque óbvia — não faz
parte de `shared/hopping.h` (16 canais espalhados 10–125), então nenhuma
defesa de hopping se aplica antes do primeiro handshake. `jammer/src/node.h`
varre 2–80 continuamente; 76 está bem no meio dessa faixa.

## Goals / Non-Goals

**Goals:**
- Encontro confiável mesmo com um jammer sweep ativo sobre parte da faixa
  2–80, sem coordenação prévia além dos candidatos fixos compilados nos
  dois firmwares.
- Mestre e escravo com papéis assimétricos deliberados (acampar vs. varrer)
  para convergirem em poucos ciclos sem handshake de descoberta adicional.
- Recuperação automática quando o enlace se perde depois de estabelecido
  (não só no boot) — mesma máquina de estados serve os dois casos.
- Contador de hop compartilhado, condição de contorno para qualquer
  comutação de canal sincronizada entre os dois lados.

**Non-Goals:**
- Não decide o gatilho de rekey nem o formato do pacote de
  seed/blacklist — isso é `dynamic-seed-blacklist`. Este change só garante
  que, se e quando esse pacote existir, os dois lados já concordam sobre o
  hop atual.
- Não implementa nenhuma forma de autenticação/anti-spoofing do sinal de
  pareamento — fora de escopo do TCC (rádio aberto, sem criptografia).
- Não muda `HOPPING_CHANNELS` (tabela de 16 canais do hopping em operação) —
  só a fase de pareamento anterior a ele.

## Decisions

### Candidatos de pareamento fixos, maioria fora de 2–80
`PAIRING_CANDIDATES[8] = {76, 90, 96, 101, 108, 114, 119, 124}` — mantém 76
como primeiro candidato (compatibilidade com comportamento observado até
aqui) mas adiciona 7 fora da faixa varrida pelo jammer sweep. Alternativa
considerada: candidatos = subconjunto de `HOPPING_CHANNELS`. Rejeitada para
manter pareamento e hopping operacional como fases com canais distintos —
mais fácil de diagnosticar em log qual fase o sistema está.

### Assimetria: mestre acampa (rotação lenta), escravo varre (ciclo rápido)
Se os dois varressem no mesmo ritmo, a probabilidade de estarem no mesmo
canal ao mesmo tempo seria baixa e imprevisível. Fixando o mestre parado por
mais tempo em cada candidato (`PAIR_CAMP_MS` ≈ 2 s) e o escravo passando
rápido por todos (`PAIR_DWELL_MS` ≈ 250 ms, ciclo completo ~2 s), o escravo
garante passar pelo canal do mestre pelo menos uma vez por ciclo dele.

### Mestre mede antes de acampar (mesma técnica do analisador)
Reusa `startListening()` + `testCarrier()` (como `analisador/src/node.h`
`measureChannel`) para escolher o candidato mais silencioso antes de gastar
uma janela de `PAIR_CAMP_MS` nele — evita acampar 2 segundos inteiros num
canal já sob ataque quando outro está livre.

### Fallback de perda serve pareamento e recuperação pós-rekey com a mesma máquina de estados
Em vez de ter uma lógica de descoberta só para o boot e outra para
recuperação de falha, ambos os lados tratam "N hops sem tráfego válido"
(`LOSS_HOPS` ≈ 25, a ~220 ms/hop ≈ 5,5 s) como transição de volta ao estado
de pareamento. Isso significa que uma renegociação de `dynamic-seed-blacklist`
que falhe (nenhum `ControlAck` recebido dentro da janela) degrada de forma
segura para o mesmo mecanismo já validado do primeiro encontro, em vez de
precisar de um caminho de recuperação dedicado.

### `hop_counter` explícito no `FragmentPacket`, não implícito por tempo
Alternativa considerada: sincronizar por tempo (`millis()` compartilhado via
handshake). Rejeitada — deriva de clock entre dois ESP32 seria cumulativa e
seria preciso um protocolo de time-sync separado. Enviar o contador junto
com cada fragmento é uma correção contínua de baixo custo (4 bytes) que já
viaja no pacote existente.

## Achado de bancada: bug real em RF24::write() (RF24 1.6.2)

Ao validar em bancada (2026-09-17), o mestre travava permanentemente na
primeira sonda de pareamento sempre que o escravo não estava
imediatamente ouvindo no canal (o cenário normal aqui, já que o escravo
varre em vez de escutar um canal fixo). Diagnóstico por telemetria
bisectando cada chamada confirmou: `RF24::write(buf, len, multicast)`
nesta versão da lib tem
`do { startFastWrite(...); } while (errHandler(&doRecovery) < 0);`, e
`errHandler(bool* doRecovery)` testa `if (doRecovery)` (o ponteiro,
sempre não-nulo) em vez de `if (*doRecovery)` (o valor) — todo timeout de
95ms interno retorna -1 de novo, e o do-while nunca sai se TX_DS/TX_DF
nunca setarem. Isso não é exclusivo do pareamento: qualquer `write()` de
fragmento/controle durante jamming ativo (perda real de pacote,
exatamente a condição que este TCC precisa testar) correria o mesmo
risco de travar um nó pra sempre no meio de uma sessão.

**Fix aplicado**: `shared/protocol.h` ganhou `radio_send()`, usando
`writeFast()+txStandBy(timeout)` (bound explícito, caminho de erro
diferente) em vez de `write()`, sempre com `multicast=true` — a
confirmação de qualquer troca do protocolo já vem de um pacote de
resposta lido separadamente (LinkAck/SeedAck/FragmentAck/ControlAck), a
sonda em si só precisa terminar sem travar. `radio_send()` também limpa
a FIFO de TX antes de cada envio (um payload preso ali travaria
`writeFast()` em `TX_FULL` para sempre). Aplicado em **todo** `write()`
de mestre e escravo, não só no pareamento.

Um segundo bug de convergência foi encontrado e corrigido junto:
`PAIR_DWELL_MS` (250ms) era menor que `LINK_RETRY_MS` (300ms) — por
pigeonhole, a janela de escuta do escravo podia sistematicamente cair
sempre entre duas sondas consecutivas do mestre e nunca convergir,
dependendo só da fase relativa (fixa, sem realinhamento) entre os dois
relógios livres. Corrigido subindo `PAIR_DWELL_MS` para 400ms.

**Bloqueio de bancada em aberto (não é bug de código)**: mesmo depois
dos dois fixes acima, com mestre e escravo os dois fixos no MESMO canal
76 continuamente (nem varredura, nem retry envolvidos — o cenário mais
simples possível), a sonda não chega ao escravo. Testado também com o
código standalone original (pré-`adaptive-rendezvous`, canal fixo, sem
nenhuma das mudanças deste change) na mesma bancada: mesmo resultado.

Isolado em duas rodadas de diagnóstico (2026-09-17):

1. `env:validacao` (jammer+analisador no mesmo ESP32) mostrou a Placa B
   passando (analisador detecta o próprio jammer da mesma placa) e a
   Placa A falhando (analisador HSPI lê zero em todos os 126 canais,
   qualquer condição). Isso apontava inicialmente pro HSPI da Placa A.
2. `Tests/AntennaCross/` (par TX/RX standalone mínimo, sem FHSS/
   pareamento — ver `openspec/ROADMAP.md` para o procedimento) testou
   cada rádio de cada placa como transmissor E como receptor,
   isoladamente, em canais fixos. Resultado, testado nas duas direções e
   em dois canais diferentes pro VSPI (76 e 100), pra descartar
   interferência de canal específico:

   | Direção | VSPI | HSPI |
   |---|---|---|
   | Placa A → Placa B | **falha** (0–1 de ~30+ enviados) | 100% |
   | Placa B → Placa A | 100% | 100% |

   Única combinação que falha: **o rádio VSPI da Placa A especificamente
   como transmissor** — seu próprio receptor funciona (confirmado
   quando a Placa A vira receptora na segunda direção), e o rádio HSPI
   da mesma placa transmite normalmente. O teste 1 (mesma placa) tinha
   sido um falso direcionamento: `testCarrier()`/RPD é um caminho de
   detecção diferente de recepção normal de pacote, e não teria pego o
   problema real (que é só no lado de transmissão do VSPI).

O bloqueio está isolado a um componente específico — o estágio de
transmissão (provavelmente o amplificador de potência) do módulo VSPI
da Placa A, que é exatamente a posição do mestre no firmware principal.
Validação manual (seção 6, abaixo) permanece bloqueada até esse módulo
ser trocado ou reparado.

## Correção (2026-09-17, mesma sessão): não era hardware — `enableAckPayload()` quebrava a entrega

A conclusão acima (módulo VSPI da Placa A com defeito de transmissão)
**estava errada**, e a rodada 2 do diagnóstico tinha uma variável não
controlada: o usuário trocou fisicamente os 4 módulos entre as sondas de
diagnóstico ao longo da sessão (antenas/adaptadores realocados entre VSPI
e HSPI, entre placa A e B) tentando isolar o problema por eliminação. Uma
bateria final e completa (`Tests/AntennaCross`, os 4 módulos já
embaralhados pelas trocas do usuário, cada um testado como TX e como RX
nas duas direções) confirmou **os 4 módulos físicos funcionando nos dois
sentidos, ~100% de entrega** — não havia mais nenhum módulo
identificável como defeituoso. Isso invalidou a conclusão anterior: o
"padrão" de falha só-VSPI-só-Placa-A observado antes era um artefato de
qual módulo físico estava fisicamente conectado em qual barramento
naquele momento, não uma propriedade fixa do barramento/placa.

Com o hardware validado, a suspeita voltou pro software. Isolado com o
mesmo `Tests/AntennaCross` (incrementando a config do teste até bater com
o firmware real, um item por vez): `setAutoAck(true)` sozinho entrega
~100%; adicionar `enableAckPayload()` (presente em `mestre/src/node.h` e
`escravo/src/node.h` desde antes desta sessão) derruba a entrega pra
~0%, **mesmo sem `writeAckPayload()` nunca ser chamado em nenhum lugar
do protocolo**. Comportamento visto nestes módulos clone (base SI24R1
comum em módulos NRF24L01+ de baixo custo) — o bit de ack payload
habilitado no `FEATURE` register muda como o chip trata pacotes NO_ACK
(`W_TX_PAYLOAD_NO_ACK`, usado por `radio_send()`/`writeFast(...,
multicast=true)`, o único caminho de envio deste projeto), fazendo o
receptor descartar o pacote antes de `available()` sequer disparar.

**Fix**: `enableAckPayload()` removido dos dois lados (era herança do
código original, nunca teve uso real — a confirmação de qualquer troca
deste protocolo sempre veio de pacotes de resposta explícitos:
LinkAck/SeedAck/FragmentAck/ControlAck). Com a remoção, **pareamento e
troca de seed funcionam de forma confiável pela primeira vez em toda a
sessão** — tarefa 6.1 abaixo considerada cumprida.

**O que continua em aberto**: com o pareamento funcionando, a fase de
hopping ativo (envio de fragmentos com troca de canal a cada
`HOP_INTERVAL_MS`) ainda falha na esmagadora maioria dos hops (~0–10% de
entrega, tipicamente 0–2 acks bem-sucedidos a cada ~25–30 hops antes do
`loss_of_sync`). Já descartados como causa única: nível de PA (testado
`PA_MAX`, sem melhora), payload dinâmico vs. estático (DPL isolado
funcionou ~100% no teste standalone), assentamento de PLL insuficiente
após `setChannel()` (130µs → 500µs, melhora marginal só), contenção do
mutex de telemetria atrasando `startListening()` (prints reordenados
para depois das operações críticas — ver commit de firmware desta
sessão), a "rajada" de hops na largada do relógio (corrigida via
`hop_clock_primed`, eliminada e confirmada por timestamp, mas sem
melhora na entrega), e — de forma decisiva — o próprio offset de fase
residual entre os relógios livres do mestre e do escravo (aumentar
`HOP_INTERVAL_MS` para 2000ms com ~76 reenvios do mesmo fragmento por
hop, o que deveria garantir sobreposição temporal independente de
qualquer offset, ainda assim só teve 2/14 hops bem-sucedidos). A causa
raiz da falha específica da fase de hopping permanece desconhecida;
áreas ainda não investigadas: possível necessidade de `flush_rx()`
explícito após cada `setChannel()` (RX FIFO potencialmente contendo lixo
de antes da troca de canal), ou alguma interação de protocolo específica
do `FragmentPacket` (30 bytes, DPL) sob trocas de canal rápidas e
sucessivas que os testes isolados em canal fixo não reproduziram.

## Correção 2 (2026-09-18): hopping por evento resolveu a falha da fase de hopping

O usuário apontou que o hopping deveria ser **por evento** (canal só
avança quando o hop atual já foi confirmado recebido), não por relógio
periódico — e o código, desde o commit inicial do projeto (`git show
97fca79:mestre/src/main.cpp`), sempre foi por relógio: `g_hop_counter`
avançava a cada `HOP_INTERVAL_MS` (220ms) **incondicionalmente**, sucesso
ou falha, mantendo mestre e escravo em relógios livres independentes,
sincronizados só por sorte residual de fase. Essa era exatamente a
hipótese investigada e descartada como causa única na sessão anterior
(teste decisivo com dwell de 2000ms + ~76 reenvios/hop ainda só conseguiu
2/14 hops) — o que sugeria a causa não ser só timing. Em retrospecto, o
teste decisivo tinha uma falha: aumentar o dwell não muda a NATUREZA do
mecanismo (ainda avança incondicionalmente a cada tick, só que um tick
maior), então não isolava de fato "relógio vs. evento" como variável.

**Fix**: reescrito `mestre_loop()`/`escravo_loop()` para hopping
genuinamente orientado a evento, com fallback de timeout —
`HOP_INTERVAL_MS` renomeado para `HOP_TIMEOUT_MS` (mesmo valor, 220ms),
agora como teto por tentativa, não período fixo. Cada rodada de hop só
termina por (a) confirmação recebida (mestre: ack; escravo: fragmento ou
pacote de controle válido recebido e ackado) — os dois lados avançam
imediatamente para o MESMO próximo `hop_counter` (o escravo resincroniza
o dele a partir do valor recebido no pacote antes de avançar) — ou (b)
`HOP_TIMEOUT_MS` esgotado sem nada (fallback: avança mesmo assim, para
não travar para sempre num canal jamado). Não há mais nenhum relógio
livre rodando contra o wall-clock; o único estado compartilhado é
`hop_counter`, um inteiro que avança em exatamente 1 por rodada em cada
lado, então não diverge mesmo que as duas rodadas não terminem no mesmo
instante real. Adicionado também, no mesmo ponto de código,
`flush_rx()` logo após cada `setChannel()` nos dois lados — uma das
hipóteses não testadas listadas acima, mais relevante agora que hops de
sucesso podem ser bem mais rápidos que o antigo tick fixo.

**Resultado medido em bancada, imediatamente após o reflash**: PLR de
sessão caiu de ~90–100% (todas as tentativas anteriores) para ~0,6% e
caindo continuamente; `hop_counter` chegou a 4023 em ~97s de execução
(~41 hops/s, contra ~4,5 hops/s do relógio fixo antigo) — confirma que
hops de sucesso agora terminam quase instantaneamente em vez de esperar
o resto de uma janela fixa de 220ms. Nenhum `loss_of_sync` observado no
período observado (jammer desligado). Isso resolve o bloqueio descrito
na seção anterior — a causa raiz real era a arquitetura de agendamento
por relógio periódico em si, não nenhum dos fatores testados
individualmente (PA, DPL, PLL, mutex, rajada de largada, offset de
fase). Validação de soak mais longa (minutos, não segundos) e sob jammer
ativo ainda não foi feita — ver tasks.md seção 6.

## Implementado depois (2026-09-24): blacklist de candidatos de pareamento

**Motivação**: o achado de bancada da seção 6 de `tasks.md` (deadlock de
pareamento por `LINK_ACK` sem confirmação real, corrigido com
`SEED_WAIT_TIMEOUT_MS`) revelou um problema separado: mesmo depois de
corrigido o deadlock, um candidato podia medir "quieto" por RPD
(`measure_and_pick_candidate()`) e mesmo assim falhar repetidamente no
handshake real (seed nunca confirmada) — a métrica de energia não é o
mesmo que "um pacote de verdade atravessa". Sem memória entre tentativas,
o mestre podia voltar a medir e escolher o MESMO candidato ruim
indefinidamente, gastando toda a janela de retry nele.

**Decisão**: reusar a lógica já validada de `blacklist-decay`
(`update_channel_mask()` em `shared/hopping.h`) para os 8
`PAIRING_CANDIDATES`, não só os 16 `HOPPING_CHANNELS`. Isso exigiu
generalizar `update_channel_mask()` com um parâmetro `count` explícito
(default `TABLE_SIZE`, preservando todas as chamadas existentes) em vez
de assumir 16 fixo — ver `shared/hopping.h` e os 2 novos testes em
`test/test_hopping.cpp` que confirmam o comportamento com `count=8`.

**O que conta como falha de um candidato** (`note_candidate_result()` em
`mestre/src/node.h`): (a) a janela inteira de `PAIR_CAMP_MS` passar sem
nunca conseguir `pair_ok` nesse candidato, OU (b) `pair_ok` foi
conseguido mas `send_seed_to_slave()` falhou (a seed nunca foi
confirmada) — o sinal mais específico e mais relevante para o problema
observado. Sucesso completo (seed confirmada) zera o histórico de falha
do candidato. `PAIR_FAIL_THRESHOLD=3` (menor que `CHANNEL_FAIL_THRESHOLD=5`
do hopping de dados, porque cada falha aqui já consumiu uma janela de
camping inteira, não só 1 hop) e `PAIR_RECOVERY_ROUNDS=12` "rounds" de
seleção de candidato (não hops, que não existem antes de parear) — ver
`shared/protocol.h`.

**Validado em bancada real em 2026-09-24**: telemetria `pair_mask`
confirmou o ciclo completo funcionando com dados reais — candidatos sendo
excluídos um a um conforme falhavam (`0xFF→0xFE→0xFC→0xF8→0xF0`) e
reincluídos após o cooldown vencer (`...→0xE1→0xE2→0xE4`), sem nunca
esvaziar a máscara. Mesmo assim o pareamento não convergia nessa sessão.

**Correção do diagnóstico (2026-09-24, mais tarde)**: a explicação
registrada aqui antes — "ruído/interferência de RF ambiente afetando
todos os candidatos" — **estava errada**. A investigação de
[pairing-handshake-hardening](../pairing-handshake-hardening/design.md)
mostrou três defeitos de protocolo/rádio: (1) depois da fase de salto, o
rádio do escravo (clone NRF24) ficava surdo com registradores idênticos
aos do boot — só um power-cycle (`powerDown()/powerUp()`) limpa;
(2) `send_seed_to_slave()` imprimia na serial entre transmitir e voltar a
escutar, perdendo o `SeedAck` quase sempre (handshake meio-aberto);
(3) acampamento de 2 s menor que o ciclo de varredura de 3,2 s. E a
blacklist de candidatos, contando "ninguém respondeu" como falha do canal,
culpava os canais pela surdez do escravo — o ciclo `pair_mask` acima era
sintoma, não prova de ruído. Com as correções daquele change: 20/20
re-pareamentos forçados (antes 0/6). As regras de falha/sucesso de
candidato descritas acima foram substituídas pela D5 de lá.

## Risks / Trade-offs

- **Candidatos fixos são conhecidos por qualquer jammer que leia o código
  público do TCC** — aceitável para o escopo (defesa contra jamming de
  banda/espectro, não contra um atacante com acesso ao firmware).
- **`PAIR_CAMP_MS`/`PAIR_DWELL_MS` mal calibrados** podem fazer o encontro
  demorar mais ciclos que o esperado — ajustar em bancada (task de
  validação) antes de fixar os valores finais no design.
- Se **todos** os 8 candidatos estiverem sob ataque simultâneo (sweep total
  da banda), o pareamento não converge — cenário aceito como limite do
  sistema (jammer com banda maior que a capacidade de fuga), não coberto por
  este change. (Uma observação de 2026-09-24 atribuída a isso por ruído
  ambiente foi depois explicada por defeitos de protocolo — ver a correção
  na seção acima.)

## Migration Plan

1. `shared/protocol.h` extraído primeiro (sem mudança de comportamento) —
   valida que mestre/escravo continuam se pareando no canal 76 fixo antes de
   qualquer lógica nova.
2. Candidatos + assimetria acampar/varrer implementados; testar isoladamente
   com jammer desligado (mesmo canal 76 deve seguir funcionando, é o
   primeiro candidato).
3. Fallback de perda de sincronismo adicionado; testar forçando perda
   manual (desligar uma placa e religar).
4. `hop_counter` no `FragmentPacket` — testar que o escravo converge para o
   contador do mestre mesmo partindo de valores diferentes.
