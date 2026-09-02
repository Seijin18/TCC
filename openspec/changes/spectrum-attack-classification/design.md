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
