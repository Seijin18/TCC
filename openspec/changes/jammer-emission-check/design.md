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
| Tráfego do próprio enlace | onde o mestre está saltando | sim, com enlace em `mode fixed` (ch 10) |
| Sweep do jammer (2-80) | 2-80 MHz | sim (faixa não coberta pelo sweep) |

O modo `targeted` **hita** os 7 canais altos (é a tabela inteira). Então:
enlace preso no canal 10 + jammer targeted + olhar só 94-125 ⇒ qualquer
energia ali é do jammer e de mais nada.

### Correção do método original (revisão 2026-09-25): NÃO desligar o enlace

O plano inicial (task 2.1) mandava "desligar o enlace" para o mestre não
poluir a faixa. Isso está **errado** contra o firmware atual e produziria um
falso "não irradia":

1. **Não existe comando "link off".** O mestre só aceita `seed`,
   `mode fixed|hopping`, `mitigation on|off`, `reset`, `regs`
   (`mestre/src/node.h`).
2. **O jammer só emite com o enlace confirmado.** Na placa combinada, o
   jammer só liga a portadora quando o escravo reporta `ready`
   (`jammer/src/node.h`, `g_jammer_ready_source`); esse flag cai em todo
   `loss_of_sync` (`escravo/src/node.h`, `notify_ready(false)`). Sem enlace,
   `jam on` fica só esperando — nada é transmitido.
3. **Mestre despareado polui a própria faixa alta.** Sem par, o mestre fica
   sondando os `PAIRING_CANDIDATES = {76,90,96,101,108,114,119,124}`
   (`shared/protocol.h`), seis deles a 1-3 canais da faixa 94-125.

**Método corrigido**: manter o enlace **pareado** e pôr os dois lados em
`mode fixed` (trava o enlace no `TEST_FIXED_CHANNEL = HOPPING_CHANNELS[0] =
10`, fora da faixa de teste). O jammer só checa o enlace **ao iniciar** a
portadora; uma vez ligado, continua emitindo mesmo que o enlace caia depois.
Se o enlace **cair** durante a fase ON (o escravo é co-locado com o jammer),
isso por si só já é evidência de que o jammer irradia — registrar esses
`loss_of_sync` e computá-los no veredito.

## Critério de decisão (o que conta como "irradia")

### Por que "ON ≥ 2× OFF" não serve

Na faixa 94-125 o OFF é ~0, então "ON ≥ 2× OFF" passa com um único hit
espúrio. Pior: em `targeted`, um rádio só está num canal por vez — segura
cada canal ~60 ms e percorre os 16, enquanto o analisador amostra cada canal
~1x por sweep (~0,9 s). Logo, mesmo irradiando forte, **cada** canal alto só
acende em ~6% dos sweeps (~12% se a placa B tiver o segundo rádio do jammer).
São ~4 hits por canal em 60 s. Um limiar por-canal absoluto interpretaria
isso como ruído.

### Critério corrigido (revisão 2026-09-25)

Fases de **120 s** cada (não 60 s), para acumular contagem estatística.
Fatiar `spectrum.csv` por epoch em OFF e ON e computar:

1. **Por canal** {94,100,105,110,115,120,125}: média da atividade e fração de
   sweeps com atividade > 0, OFF vs ON (tabela vai pro TCC de qualquer jeito).
2. **Combinado nos 7 canais**: fração de sweeps com **qualquer** atividade > 0
   nos 7, OFF vs ON. Este é o discriminante principal.
3. **`loss_of_sync` durante ON vs OFF**: queda do enlace co-locado na fase ON
   é evidência direta de emissão.

**Veredito "irradia"** se qualquer um valer:
- fração combinada ON claramente acima da OFF (esta ~0), consistente com o
  ~6-12% esperado, com ≥ 1 canal cuja média ON supera nitidamente a OFF; **ou**
- o enlace cai (`loss_of_sync`) na fase ON e não na OFF.

**"Não irradia / fraco demais"**: fração combinada ON indistinguível da OFF
**e** enlace estável nas duas fases.

Guardar os números crus (média + fração por canal, por fase) no relatório —
independentemente do veredito, entram no TCC como caracterização do atacante.

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
