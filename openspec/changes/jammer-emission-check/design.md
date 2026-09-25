# Design — validação de emissão do jammer

## Contexto que a próxima sessão precisa ter em mãos

- **Placas** (não mover para este teste — a graça é justamente o escravo
  co-locado): placa A = mestre + analisador (CP2102); placa B = escravo +
  jammer (CH343). Distância A↔B: 30-50 cm, linha de visada limpa.
- **Logger**: `.venv/bin/python tools/logger/main.py` roda e é dono das duas
  seriais. **Não reiniciar** (reinício reseta placa A). Comandos vão por
  `echo "<placa> <cmd>" >> logs/cmd.in`. Sessão corrente = `logs/session-*.jsonl`
  mais recente.
- **Comandos do jammer** (placa B): `mode targeted|sweep|adaptive`,
  `jam on`, `jam off`, `power min|low|high|max`. É `power max`, **não**
  `jam power max`.
- **spectrum.csv**: `logs/spectrum.csv`, uma linha por sweep do analisador
  (~1 Hz), 126 colunas indexadas pelo número de canal RF cru 0-125, cada
  linha com epoch do mesmo relógio da sessão. Já validado nesta bancada
  (relatório de 2026-09-25).
- **Canais de hopping** (`shared/hopping.h`, `HOPPING_CHANNELS`): 10, 22, 34,
  46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27. Os **sete acima
  de 80** — 94, 100, 105, 110, 115, 120, 125 — são a faixa de teste limpa.

## Por que a faixa 94-125 é o probe certo

Três coisas confundiram todas as medidas anteriores. A faixa alta remove as
três de uma vez:

| Confundidor | Onde vive | Some em 94-125? |
|---|---|---|
| Wi-Fi real da bancada | ~ch 26-45 | sim (fora da faixa) |
| Tráfego do próprio enlace | onde o mestre está saltando | sim, se **enlace desligado** |
| Sweep do jammer (2-80) | 2-80 MHz | sim (faixa não coberta pelo sweep) |

O modo `targeted` **hita** os 7 canais altos (é a tabela inteira). Então:
enlace OFF + jammer targeted + olhar só 94-125 ⇒ qualquer energia ali é do
jammer e de mais nada. Não existe comando de "canal fixo" no firmware atual;
targeted percorrendo os 16 é suficiente porque a energia acumula nesses 7 ao
longo de muitos sweeps.

## Critério de decisão (o que conta como "irradia")

Para cada canal em {94,100,105,110,115,120,125}, calcular a média da coluna
correspondente do `spectrum.csv` nas linhas da fase OFF e nas da fase ON.

- **Irradia**: pelo menos um dos 7 canais sobe de forma clara ON vs OFF
  (proposta de limiar: ON ≥ 2× OFF **e** ON acima do ruído de fundo médio da
  faixa 94-125 na fase OFF). Registrar o delta por canal.
- **Não irradia / fraco demais**: nenhum dos 7 sobe além do ruído.

Guardar os números crus (média por canal, por fase) no relatório —
independentemente do veredito, eles entram no TCC como caracterização do
atacante.

## Ramo condicional depois do veredito

```
teste de isolamento (Tarefa 2)
   │
   ├─ irradia bem ──► Tarefa 3 (só diagnóstico de boot, barato) ──► conclusão:
   │                   "jammer OK; CW estreito é escapável por hopping rápido"
   │                   (resultado de TCC válido; fecha a ambiguidade das baterias)
   │
   └─ ausente/fraco ─► Tarefa 3 (diagnóstico de boot: confirmar isPVariant)
                        │
                        ├─ reporta P-variant ──► investigar antena/potência/
                        │                         dwell antes de mexer na emissão
                        │
                        └─ reporta não-P ──────► Tarefa 4 (emissão robusta para
                                                  clone) ──► repetir Tarefa 2
```

## Detalhe do fix de clone não-P (Tarefa 4, só se necessário)

`startConstCarrier` sustenta a portadora via payload dummy + `reUseTX()`
apenas em variante P (`RF24.cpp:2103`, guardas `if (isPVariant())`). Em
não-P, alternativa que funciona em clones:

- desligar auto-ack (`setAutoAck(false)`) e retries (`setRetries(0,0)`);
- em loop apertado no canal alvo, `setChannel(ch)` (com o toggle de CE que o
  `hopCarrier` já faz) seguido de escrita contínua de um payload fixo de 32
  bytes **sem esperar ACK** (`writeFast`/`startFastWrite` + `txStandBy`, ou
  `write(..., multicast=true)`), mantendo o rádio em TX o tempo todo;
- preservar a interface serial (`mode`/`jam on|off`/`power`) e a telemetria
  (`jam_state_on/off`, `jam_mode_*`) intactas, para não quebrar
  `run_scenario.py`/`run_battery.py`.

Esse método enche a banda do canal com um sinal modulado real (mais efetivo
que CW para negar recepção) e não depende de `CONT_WAVE`/`reUseTX`. Manter o
caminho `startConstCarrier` atrás do resultado de `isPVariant()` (usar CW em
P, payload-flood em não-P) em vez de remover, para não regredir hardware que
funcione.

## O que NÃO fazer nesta change

- Não reposicionar placas (é o teste do escravo co-locado; e a aproximação do
  analisador é outra pendência, `targeted-jammer` 3.2 visual).
- Não rodar bateria A/B/C aqui — esta change só decide se vale a pena rodar.
- Não mexer na lógica de `mode targeted` (quais canais) — isso é
  `targeted-jamming`, já arquivável.
