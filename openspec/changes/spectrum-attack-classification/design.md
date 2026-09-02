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
