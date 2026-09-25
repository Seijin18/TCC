## 1. Classificação no analisador

- [x] 1.1 Incluir `shared/hopping.h` em `analisador/src/main.cpp` para
      acessar `HOPPING_CHANNELS`/`TABLE_SIZE`.
- [x] 1.2 ~~Após `scanSpectrum()`, calcular `alvo_avg` (média em
      `HOPPING_CHANNELS`) e `fora_avg` (média nos demais canais
      varridos)~~ -- **substituído em 2026-09-23** por um baseline
      adaptativo POR CANAL (`updateAdaptiveBaseline()`), depois que a
      abordagem por média foi confirmada como estruturalmente incapaz de
      separar ruído de jamming real (ver design.md "Achado de calibração
      2026-09-23"). `classifyAttack()` agora conta canais
      individualmente comprometidos, não médias. Ver design.md "parte 3".
- [x] 1.3 ~~Definir constantes `LIMIAR_ALVO`, `LIMIAR_CONCENTRACAO`~~ --
      substituídas pelas constantes do baseline adaptativo
      (`ADAPTIVE_K`, `ADAPTIVE_MIN_ABS_DEV`, `ADAPTIVE_WINDOW`,
      `ADAPTIVE_VOTES`, `ATTACK_MIN_CHANNELS`), calibradas na seção 3.
      `ATTACK_STREAK_N` mantida (valor ajustado de 3 para 2).
- [x] 1.4 Implementar contador `g_attack_streak` e estado
      `g_attack_active`, atualizados a cada ciclo conforme o spec.

## 2. Sinalização e recepção

- [x] 2.1 Escolher e documentar (no `README.md`, seção de hardware) um
      GPIO livre no analisador para o pino de alerta e o pino
      correspondente no mestre, mais o fio de GND comum entre as duas
      placas.
- [x] 2.2 No analisador, configurar o pino de alerta como saída e
      atualizar seu nível (HIGH/LOW) nas transições de `g_attack_active`
      conforme o spec.
- [x] 2.3 No mestre, configurar o pino correspondente como entrada e, no
      loop principal, detectar transições LOW→HIGH e HIGH→LOW, logando
      via serial.

## 3. Calibração e validação manual

- [ ] 3.1 Rodar o analisador sozinho em ambiente com Wi-Fi/BT normal do
      laboratório (sem jammer) por alguns minutos e confirmar que nenhum
      "ataque ativo" é declarado (ajustar limiares se houver falso
      positivo). **Rodado em 2026-09-18** via
      `tools/validation/run_scenario.py` (fase baseline, jammer desligado)
      — nenhum falso positivo (`alert_on` nunca disparou, correto), mas o
      CSV bruto mostrou vários canais (inclusive fora de
      `HOPPING_CHANNELS`) com 40–60% de atividade mesmo sem jammer —
      piso de ruído desta bancada mais alto que o assumido pelos valores
      default de `LIMIAR_ALVO`/`LIMIAR_CONCENTRACAO`. Ver
      `openspec/ROADMAP.md` para os números medidos.
      **Repetido 2026-09-25 02:16-02:26 (10 min, jammer desligado,
      detector CFAR por canal)**: **não passa** — o analisador manteve
      `alert` ativo durante 95% do tempo, marcando ch 58 (`mask=0x0010`,
      vizinho do Wi-Fi real que ocupa ch ~26-45 nesta bancada). Ao longo
      do dia houve ~140 `alert_on` com o jammer desligado. Custo prático
      pequeno: o reseed é disparado por borda, então foi 1 troca de seed em
      10 min, sem nenhuma queda de enlace (soak de `hop-permutation` 2.1).
      E, ao contrário, **nenhum alerta** nas fases com jammer ligado
      (`hop-resync` 2.2, baterias A/B/C): com as placas na posição atual o
      analisador (placa A) não enxerga o jammer (placa B), só o Wi-Fi. A
      calibração (e a decisão de tratar "canal ocupado por Wi-Fi" como
      alerta ou não) depende de reposicionar as placas — mesma dependência
      de 3.3 e de `dynamic-seed-blacklist` 4.5.
- [x] 3.2 Rodar com o `targeted-jammer` ativo e confirmar que o alerta é
      declarado dentro de poucos ciclos e mantido enquanto o ataque
      persistir. **Resolvido em 2026-09-23** com o baseline adaptativo
      por canal (design.md "Achado de calibração 2026-09-23, parte 3"):
      `alert_on` disparou 2x em bancada real (150s de `jam mode targeted`
      + `power high`), com evidência por canal (`mask=0x0001`/canal 10,
      depois `mask=0x0002`/canal 22), disparando `dynamic-seed-blacklist`
      de ponta a ponta (rekey de seed, blacklist aplicada,
      `mask=0xFFFB`). "Dentro de poucos ciclos" agora significa até
      `ADAPTIVE_WINDOW=60` sweeps de latência (trade-off deliberado —
      ver design.md — necessário pra não ficar em silêncio como as
      versões anteriores). Histórico do que NÃO funcionou antes de
      chegar aqui, mantido abaixo por registro:

      **Rodado em 2026-09-18, não passava**: mesmo com
      `jam mode targeted` + `power high` por 75s (PLR do enlace medido
      subindo de ~0% para ~12%, confirmando jamming real), `alert_on`
      nunca disparou. Corrigido um bug real no caminho (`classifyAttack()`
      usava snapshot único por sweep — `channelData[]` — que um jammer de
      rádio único quase nunca preenchia nos 16 canais simultaneamente;
      trocado por acúmulo de calor entre sweeps, `channelHeat[]`), mas o
      piso de ruído de 3.1 ainda mantém `fora_avg` alto o bastante pra
      `alvo_avg - fora_avg` nunca cruzar `LIMIAR_CONCENTRACAO=30`.

      **Tentativa de recalibração em 2026-09-23** (`tools/validation/calibrate_thresholds.py`,
      novo -- simula `channelHeat[]` sobre o CSV bruto de 7 execuções
      reais, 5 delas em modo targeted): **não existe nenhum par
      (`HEAT_HIT_THRESHOLD`, `HEAT_DECAY_PER_CYCLE`) testado em que a
      pior concentração vista em baseline fique abaixo da pior
      concentração vista sob jamming real** -- a margem é negativa em
      todas as 20 combinações varridas (ex.: no melhor caso, baseline
      chegou a concentração 13,0 enquanto jamming real só chegou a 0,7).
      Ou seja, isto não é mais "escolher o número certo" -- nenhum número
      separa as duas classes com os dados coletados até agora. Causa raiz
      identificada: (a) o piso de ruído desta bancada já documentado em
      3.1 recai justamente sobre metade dos 16 canais de
      `HOPPING_CHANNELS` (achado de 2026-09-18, `ROADMAP.md`); (b) o
      jammer de rádio único, mesmo em targeted, só ilumina 1-4 dos 16
      canais-alvo por vez (confirmado comparando `channelData[ch]`
      canal-a-canal, baseline vs. jamming, nas 7 sessões) -- longe do "a
      maioria dos 16 elevados ao mesmo tempo" que uma média de 16 canais
      precisaria para se destacar do ruído de fundo. Tentado também
      "contar quantos dos 16 canais estão simultaneamente 'quentes'" como
      estatística alternativa à média — mesmo resultado (baseline chega a
      3 canais quentes por acaso, jamming real não ultrapassa 3-4).

      **Não aplicada nenhuma mudança de `LIMIAR_ALVO`/`LIMIAR_CONCENTRACAO`
      a partir desta tentativa** — um número escolhido arbitrariamente
      dentro da faixa sobreposta erraria mais para um lado (falso
      positivo) ou outro (falso negativo) sem nenhuma garantia real, o
      que seria pior que documentar a limitação. Ver `design.md`
      ("Achado de calibração 2026-09-23") para a análise completa e os
      dois caminhos possíveis daqui (aceitar a limitação vs. redesenhar a
      métrica de detecção).
- [ ] 3.3 Rodar com o jammer em modo `sweep` (banda inteira) e verificar o
      comportamento: com o detector antigo (`alvo_avg`/`fora_avg`), o
      esperado era "alerta não deve disparar" já que ambos sobem juntos.
      Com o baseline adaptativo por canal (2026-09-23), o raciocínio muda:
      cada canal-alvo é avaliado contra a PRÓPRIA baseline, então sweep
      cobrindo 2-80 MHz (8 dos 16 canais de `HOPPING_CHANNELS`) deveria,
      em princípio, também conseguir marcar esses 8 canais como
      comprometidos individualmente -- ainda não testado em bancada com
      o novo detector. Decidir e documentar o comportamento esperado
      (sweep também deveria contar como "ataque"? provavelmente sim,
      dado que também degrada o enlace) antes de fechar este item.
- [x] 3.4 Confirmar no log serial do mestre que início e fim de alerta são
      registrados corretamente durante um ciclo liga/desliga do jammer.
      **Confirmado em 2026-09-23**: `alert_on`/`alert_off` observados em
      transições corretas ao longo de múltiplas execuções (ex.: `alert_on`
      em t=38,6s, `alert_off` em t=124,5s quando o canal parou de ser
      sinalizado, novo `alert_on` em t=130s quando outro canal cruzou o
      limiar).
