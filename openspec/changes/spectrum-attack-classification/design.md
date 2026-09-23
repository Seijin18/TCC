## Context

`analisador/src/main.cpp` roda `scanSpectrum()` + `sendCSV()` em loop livre
(`analisador/src/main.cpp:127-131`), preenchendo `channelData[126]` com
atividade 0-100% por canal a cada ciclo (~126 × 10 amostras × 130 µs ≈
164 ms/ciclo em modo completo, mais rápido em `FAST_BT_SCAN`). O analisador
e o mestre são placas ESP32 físicas separadas, cada uma com seu próprio
NRF24 dedicado (o do analisador ocupado escaneando; o do mestre ocupado
fazendo hopping) — não há um canal de dados óbvio e barato entre os dois
sem competir com essas duas funções. O guia sugere explicitamente "pino de
interrupção de hardware ou barramento serial" como opção.

## Goals / Non-Goals

**Goals:**
- Classificar cada ciclo de varredura como "ataque direcionado" ou "não
  ataque" usando só os dados que `scanSpectrum()` já produz.
- Exigir persistência (N ciclos consecutivos) antes de declarar ataque,
  para tolerar picos transitórios de Wi-Fi/BT que sobrepõem 1-2 canais da
  tabela por acaso.
- Alerta assíncrono simples (GPIO) que o mestre consegue ler sem depender
  do NRF24 dele estar livre no momento certo.

**Non-Goals:**
- Não implementa ação de mitigação no mestre (troca de seed, blacklist) —
  isso é `dynamic-seed-blacklist`.
- Não implementa classificação por ML — o guia permite heurística simples,
  e a natureza determinística do ataque (concentração nos 16 canais
  conhecidos) não exige um classificador estatístico complexo para os fins
  do TCC.
- Não tenta distinguir *qual* jammer (sweep vs targeted) está ativo —
  apenas "há atividade concentrada nos canais do hopping, sim ou não".

## Decisions

- **Métrica de classificação por ciclo**: `alvo_avg` = média de
  `channelData[c]` para `c` em `HOPPING_CHANNELS` (16 valores);
  `fora_avg` = média de `channelData[c]` para os demais canais varridos.
  Ataque candidato se `alvo_avg >= LIMIAR_ALVO` (ex. 70%) **e**
  `alvo_avg - fora_avg >= LIMIAR_CONCENTRACAO` (ex. 30 pontos percentuais)
  — a segunda condição é o que separa "ataque dirigido" de "espectro
  inteiro ruidoso" (ex. Wi-Fi saturado em tudo, que não deveria disparar
  alerta de jamming dirigido).
- **Persistência por contador de ciclos, não por tempo**: um contador
  `g_attack_streak` incrementa a cada ciclo candidato e reseta a cada
  ciclo não-candidato; alerta é declarado quando `g_attack_streak >= N`
  (ex. N=3 ciclos). Escolhido por ciclos (não ms) porque o tempo de ciclo
  já varia com `FAST_BT_SCAN`, e contar ciclos mantém a lógica agnóstica a
  esse modo.
- **Sinalização por GPIO digital, nível mantido (não pulso)**: o pino fica
  HIGH enquanto `g_attack_streak >= N` continuar verdadeiro em ciclos
  subsequentes, e volta a LOW assim que um ciclo deixar de ser candidato —
  isso permite ao mestre tanto usar interrupção (borda de subida) quanto
  polling (nível), e sinaliza o fim do ataque tão prontamente quanto o
  início (importante para medir self-healing na Fase 4/5).
- **Leitura no mestre por polling simples no loop principal**, não
  `attachInterrupt`: o loop do mestre já roda a cada `HOP_INTERVAL_MS`
  (220 ms), granularidade suficiente para reagir ao alerta sem a
  complexidade de uma ISR de hardware disputando com o timing do rádio.
- **Pino sugerido**: um GPIO livre de baixa prioridade em ambas as placas
  (definir em `tasks.md` observando os pinos já ocupados pelo NRF24 em
  cada `main.cpp`), com GND comum entre as duas placas (requisito de
  hardware, não só de software).
- **Desvio para hardware limitado (2 placas em vez de 4)**: o arranjo acima
  (GPIO 4↔GPIO 4 entre duas placas) é o que `PIN_ALERT`/`PIN_ALERT_IN`
  implementam por padrão em `analisador/src/node.h`/`mestre/src/node.h`, e
  continua sendo o comportamento dos environments standalone
  (`env:analisador`/`env:mestre`). Para quem só tem 2 ESP32 (bancada real
  deste TCC), `docs/pinout.md` documenta mestre e analisador na mesma placa
  (Placa A, VSPI/HSPI), e o change `two-board-topology`
  (`openspec/changes/two-board-topology/`) implementa esse arranjo: o
  alerta deixa de usar o GPIO físico e vira uma flag compartilhada
  (`placa_a/src/shared_state.h`) entre a task do mestre (`loop()` padrão,
  core 1) e a task do analisador (core 0) — via os callbacks opcionais
  `mestre_set_alert_source`/`analisador_set_alert_sink`, registrados só no
  `env:placa_a`. Isso resolve a limitação de hardware, mas reintroduz outras
  (ver "Implicações" em `docs/pinout.md`): risco de saturação do receptor do
  analisador pela proximidade extrema com a antena do próprio mestre, e
  risco de o escalonamento entre as duas tasks atrasar o timing do mestre e
  distorcer a medição de PLR — nenhum dos dois riscos existe no arranjo de
  referência com placas fisicamente separadas.

## Risks / Trade-offs

- Limiares (`LIMIAR_ALVO`, `LIMIAR_CONCENTRACAO`, N ciclos) são heurísticos
  e precisarão de calibração empírica em bancada com o `targeted-jammer`
  real antes de serem considerados definitivos — tratado como tarefa de
  validação, não decisão fechada.
- Acoplamento por fio físico entre as placas é uma limitação de laboratório
  (não escala para um deployment real), mas é a opção mais barata e
  determinística para o escopo do TCC, e é a que o guia sugere
  explicitamente.

## Referências externas (auditoria do projeto IC, 2026-09-02)

O projeto de IC que serviu de base para o TCC (`github.com/Seijin18/IC`,
pasta `RFmodule/`) foi atualizado após o início do TCC e publicou um artigo
(`Artigo/conference_101719.tex`) com dados empíricos de detecção de jamming
usando o mesmo hardware (ESP32 + NRF24L01). Achados relevantes para a
calibração deste change (tasks 3.1-3.3), sem exigir mudança de design:

- **Métrica "accumulated channel energy"**: soma das ocupações (%) de todos
  os canais de uma sub-banda em uma varredura, em unidades arbitrárias
  (u.a.) — equivalente em espírito a somar em vez de tirar média de
  `channelData[c]` sobre `HOPPING_CHANNELS`. O artigo reporta, para a banda
  Bluetooth (79 canais), energia acumulada média de ~40 u.a. com jammer
  desligado vs. ~605 u.a. com jammer ligado (jammer sweep contínuo,
  ~15× de separação) — ordem de grandeza útil como ponto de partida ao
  calibrar `LIMIAR_ALVO`/`LIMIAR_CONCENTRACAO`, embora os valores exatos não
  sejam diretamente portáveis (banda diferente: 16 canais do hopping vs. 79
  do Bluetooth; jammer sweep vs. `targeted-jammer` mirando só os 16 canais).
- **Latência de detecção referência**: com ~2 varreduras/s (analisador do
  IC, 126 canais, 30 amostras/canal), o artigo reporta detecção visual do
  início/fim do jamming com latência < 500 ms. O `analisador` do TCC varre
  mais rápido em `FAST_BT_SCAN` (79 canais, 10 amostras/canal) — útil como
  referência ao escolher `ATTACK_STREAK_N` em função do ciclo real medido
  em bancada, não como valor a copiar.
- **Distinção qualitativa validada empiricamente**: o artigo confirma
  visualmente (heatmap) que dispositivos legítimos (fone, mouse, microondas)
  produzem "bandas" estáveis/coerentes no tempo, enquanto o jammer produz
  pontos de saturação dispersos sem banda estável — mesma premissa por trás
  da condição `alvo_avg - fora_avg >= LIMIAR_CONCENTRACAO` já decidida aqui;
  não muda a decisão, apenas corrobora com dado real de outro hardware
  idêntico.
- **Resolução de amostragem**: IC usa `SCAN_SAMPLES=30` por canal (resolução
  de ocupação ~3.3%) contra `SCAN_SAMPLES=10` do `analisador` do TCC
  (resolução ~10%, trade-off deliberado por velocidade de ciclo). Não é uma
  lacuna a corrigir agora, mas vale registrar como parâmetro a revisitar se
  a calibração dos limiares (tasks 3.1-3.3) mostrar que a resolução de 10%
  está mascarando a distinção `alvo_avg`/`fora_avg` na prática.
- **Limitação compartilhada**: o RPD do NRF24L01 é um indicador binário de
  limiar (~-64 dBm), não uma leitura calibrada em dBm — limitação que o
  artigo do IC documenta explicitamente e que também vale para o
  `analisador` do TCC; relevante para a seção de limitações da monografia,
  não para o código.

Nenhuma dessas referências altera as decisões já tomadas neste `design.md`;
servem como material de apoio para a calibração empírica (tasks 3.1-3.3) e,
opcionalmente, como citação na monografia do TCC.

## Achado de calibração (2026-09-23): média sobre os 16 canais não separa as classes

Tentativa de fechar a task 3.2 recalibrando `LIMIAR_ALVO`/
`LIMIAR_CONCENTRACAO` (ou os parâmetros do acumulador de calor,
`HEAT_HIT_THRESHOLD`/`HEAT_DECAY_PER_CYCLE`) contra o piso de ruído real
desta bancada, usando `tools/validation/calibrate_thresholds.py` (novo) —
uma varredura de grade que simula `channelHeat[]` exatamente como
`analisador/src/node.h` sobre o CSV bruto de 7 execuções reais em
bancada (2026-09-18 e 2026-09-23, 5 delas em modo `targeted`).

**Resultado**: nenhuma das 20 combinações de `(HEAT_HIT_THRESHOLD,
HEAT_DECAY_PER_CYCLE)` testadas produz uma margem positiva entre "pior
concentração vista em baseline" e "pior concentração vista sob jamming
real" — a pior leitura de baseline sempre supera a pior leitura de
jamming (melhor caso encontrado: baseline chegou a concentração 13,0
contra apenas 0,7 do jamming real). Ou seja, **não existe nenhum valor
de `LIMIAR_CONCENTRACAO` que classifique corretamente nas duas
direções** com os dados coletados — isto deixou de ser um problema de
"escolher o número certo".

Uma segunda estatística foi testada como alternativa à média de 16
canais — contar quantos dos 16 canais de `HOPPING_CHANNELS` estão
simultaneamente "quentes" (`channelHeat[ch] >= 100`) num dado instante,
em vez de tirar a média entre eles. Mesmo resultado: baseline chega a 3
canais quentes simultâneos só por ruído ambiente, jamming real não
ultrapassa 3-4 — margem de separação de no máximo 1 canal, não confiável
como critério binário.

### Causa raiz (duas, compostas)

1. **O piso de ruído desta bancada recai justamente sobre metade da
   tabela de hopping.** Já documentado em 2026-09-18
   (`openspec/ROADMAP.md`): 8 dos 16 canais de `HOPPING_CHANNELS` ficam
   na faixa onde este bench mostra 40-60% de atividade mesmo com o
   jammer desligado (provável Wi-Fi/BT do ambiente). Isso eleva
   `alvo_avg`/a contagem de canais "quentes" já na baseline, sem nenhum
   jammer envolvido.
2. **Um jammer de rádio único ilumina poucos canais-alvo por vez, mesmo
   em modo targeted.** Comparando `channelData[ch]` canal-a-canal entre
   baseline e jamming nas 7 sessões (não só a média), o padrão é
   consistente: em qualquer janela de tempo, só 1-4 dos 16 canais de
   `HOPPING_CHANNELS` mostram elevação clara sobre a própria baseline —
   nunca "a maioria dos 16 ao mesmo tempo". Isso é o mesmo limite físico
   já registrado na correção de 2026-09-18 (um rádio só pode estar em um
   canal por vez) — aumentar o dwell por canal (`JAM_TARGETED_HOLD_MS`)
   ajudou a detecção pontual de canal individual, mas não muda quantos
   canais ficam elevados *ao mesmo tempo*, que é exatamente o que uma
   média/contagem sobre os 16 precisa para se destacar do ruído de
   fundo que já atinge metade deles.

Combinado, o efeito é que "ruído ambiente concentrado em 8 canais" e
"jammer real concentrado em 1-4 canais por vez" produzem sinais da
**mesma ordem de grandeza** quando resumidos numa média/contagem sobre
os 16 — não há como uma reescala linear (ajustar um limiar) separar
distribuições que se sobrepõem desse jeito.

### Dois caminhos daqui, nenhum aplicado ainda

**Opção A — aceitar como limitação documentada.** O artigo do IC
referenciado acima já documenta explicitamente que o RPD do NRF24L01 é
um indicador binário de limiar, não uma leitura calibrada em dBm — esta
descoberta é consistente com essa limitação de hardware, não um defeito
de implementação. Reportar isso com os dados coletados (as 7 sessões,
o resultado da varredura de grade) é conteúdo válido para a seção de
limitações da monografia. `dynamic-seed-blacklist`/`blacklist-decay`
continuariam sem o gatilho de `alert_on` funcional nesta bancada
específica, mas o mecanismo de mitigação em si (troca de seed,
blacklist por evidência própria de `g_channel_fail`) já foi validado
independentemente do alerta.

**Opção B — redesenhar a métrica de detecção.** A comparação "média/
contagem sobre os 16 canais, num único instante" é o que não separa as
classes; a informação que SEPARA (achado desta sessão) é que canais
legítimos de ruído tendem a ser os *mesmos* canais ao longo do tempo
(um AP de Wi-Fi não muda de canal), enquanto o jammer ilumina canais
*diferentes* em sequência (ver também a observação do artigo do IC:
"dispositivos legítimos produzem bandas estáveis... jammer produz
pontos de saturação dispersos sem banda estável", citada acima). Uma
métrica que aprendesse, por canal, um piso de ruído individual ao longo
de uma janela de observação (em vez de um limiar global fixo) e
disparasse com base em desvio *daquele canal específico* em relação à
própria história — não em relação à média dos outros 15 — teria chance
de separar as classes onde a média não separa. Isto seria um change
novo (baseline adaptativo por canal), não apenas uma recalibração de
constantes, e não foi implementado nem prototipado — fica registrado
aqui como a direção mais promissora se a Opção A não for aceitável para
a banca.

Nenhuma mudança de código foi aplicada a `LIMIAR_ALVO`/
`LIMIAR_CONCENTRACAO`/`HEAT_HIT_THRESHOLD`/`HEAT_DECAY_PER_CYCLE` a
partir deste achado — os valores atuais (herdados, nunca calibrados)
permanecem, porque nenhum valor testado é defensável como melhoria real
sobre o comportamento atual.
