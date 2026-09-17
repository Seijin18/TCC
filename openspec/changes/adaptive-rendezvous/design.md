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
