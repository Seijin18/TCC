## 1. Classificação no analisador

- [x] 1.1 Incluir `shared/hopping.h` em `analisador/src/main.cpp` para
      acessar `HOPPING_CHANNELS`/`TABLE_SIZE`.
- [x] 1.2 Após `scanSpectrum()`, calcular `alvo_avg` (média em
      `HOPPING_CHANNELS`) e `fora_avg` (média nos demais canais varridos
      no modo ativo, respeitando `FAST_BT_SCAN`).
- [x] 1.3 Definir constantes `LIMIAR_ALVO`, `LIMIAR_CONCENTRACAO`,
      `ATTACK_STREAK_N` no topo do arquivo (valores iniciais a calibrar na
      seção 3).
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
- [ ] 3.2 Rodar com o `targeted-jammer` ativo e confirmar que o alerta é
      declarado dentro de poucos ciclos e mantido enquanto o ataque
      persistir. **Rodado em 2026-09-18, ainda não passa**: mesmo com
      `jam mode targeted` + `power high` por 75s (PLR do enlace medido
      subindo de ~0% para ~12%, confirmando jamming real), `alert_on`
      nunca disparou. Corrigido um bug real no caminho (`classifyAttack()`
      usava snapshot único por sweep — `channelData[]` — que um jammer de
      rádio único quase nunca preenchia nos 16 canais simultaneamente;
      trocado por acúmulo de calor entre sweeps, `channelHeat[]`), mas o
      piso de ruído de 3.1 ainda mantém `fora_avg` alto o bastante pra
      `alvo_avg - fora_avg` nunca cruzar `LIMIAR_CONCENTRACAO=30`. Falta:
      recalibrar `LIMIAR_ALVO`/`LIMIAR_CONCENTRACAO` (ou o próprio esquema
      de calor -- `HEAT_HIT_THRESHOLD`/decaimento) contra o piso de ruído
      real medido em 3.1, não os valores default herdados sem calibração.
- [ ] 3.3 Rodar com o jammer em modo `sweep` (banda inteira) e verificar o
      comportamento: se `fora_avg` sobe junto com `alvo_avg`, o alerta não
      deve disparar (ou deve disparar com uma classificação diferenciada,
      se a banca considerar sweep também como "ataque" — decidir e
      documentar o comportamento esperado antes de fechar este item).
- [ ] 3.4 Confirmar no log serial do mestre que início e fim de alerta são
      registrados corretamente durante um ciclo liga/desliga do jammer.
