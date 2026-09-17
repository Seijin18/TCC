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
  este change.

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
