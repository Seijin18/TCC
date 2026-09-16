# Roadmap de implementação — TCC Anti-Jamming

Consolida o estado dos 5 changes ativos (`openspec/changes/`) num plano de
execução único. Não substitui `project.md` (que continua sendo a fonte de
verdade de contexto/dependências) — é a camada de "o que fazer agora, em que
ordem" derivada dele. Atualizar esta lista conforme tasks forem fechadas ou
changes forem arquivados.

## Estado validado (código já implementado)

- **`plr-metric`** (Fase 1) — seções 1 e 2 do `tasks.md` **concluídas**:
  contadores de janela/sessão no escravo, cálculo e log serial de PLR
  (`PLR;t_ms=...;window=...;session=...;recv=...;exp=...`), guard contra
  log antes do handshake e contra divisão por zero. Falta só a
  **validação manual em bancada** (3.1 sem jammer, 3.2 com jammer sweep) —
  requer hardware, não pode ser fechado sem os dois nós ligados.

Todos os outros 4 changes estão **100% com tasks pendentes** (nenhuma
marcada `[x]`): `targeted-jammer`, `spectrum-attack-classification`,
`dynamic-seed-blacklist`, `scenario-automation`.

## Ordem de execução (de `project.md`)

```
plr-metric (validação pendente) ─┐
                                  ├─→ spectrum-attack-classification ─→ dynamic-seed-blacklist ─→ scenario-automation
targeted-jammer ──────────────────┘
```

`plr-metric` e `targeted-jammer` não dependem uma da outra e podem avançar
em paralelo. As três seguintes são estritamente sequenciais.

## Plano por change

### 1. `plr-metric` — fechar (bloqueado em hardware)
- Código pronto. Próxima ação é **manual**: rodar mestre+escravo sem
  jammer (3.1) e com `jammer/src/main.cpp` sweep ativo (3.2), confirmar
  padrão de PLR no log serial, então arquivar o change
  (`opsx:archive` / `openspec-archive-change`).
- Não requer ação de código adicional deste assistente.

### 2. `targeted-jammer` — próximo item acionável por código
- Sem dependências, todo o trabalho de firmware é software puro:
  - `jammer/src/main.cpp`: modo `sweep`/`targeted` via comando serial,
    avanço de canal restrito aos 16 canais de `shared/hopping.h`, divisão
    entre `radio`/`radio2`.
  - `validacao/src/main.cpp`: mesmo modo no harness (`randomTargetedChannel`,
    comando `mode`, refletir no `status`).
  - Build (`pio run -e jammer -e validacao`) é verificável sem hardware.
- Validação manual (3.x: heatmap, PLR sob targeted) fica para bancada,
  igual ao `plr-metric`.
- **Candidato a começar agora.**

### 3. `spectrum-attack-classification` — depende de (2)
- Precisa do modo `targeted` do jammer já implementado para validar a
  heurística de classificação empiricamente (tasks 3.x), mas a
  implementação de código (seção 1: cálculo `alvo_avg`/`fora_avg`,
  `ATTACK_STREAK_N`; seção 2: GPIO de alerta analisador→mestre) pode ser
  escrita assim que (2) estiver com build limpo, mesmo antes da validação
  de bancada de (2) terminar.

### 4. `dynamic-seed-blacklist` — depende de (1) e (3)
- Precisa do sinal de PLR (métrica de recuperação) e do alerta do
  analisador (gatilho de mitigação) para fazer sentido. Implementação de
  código (máscara de canais, pacotes de controle, fila de prioridade)
  só deve começar depois que (3) tiver ao menos a seção 1/2 implementada
  (o pino de alerta precisa existir para o mestre reagir a ele).

### 5. `scenario-automation` — depende de todos
- Fecha o projeto: modo `hopping|fixed`, captura multi-porta, orquestração
  dos 3 cenários (A/B/C), gráficos finais para a monografia. Só faz
  sentido depois que blacklist dinâmica (4) estiver validada em bancada,
  já que o Cenário C mede a recuperação que ela proporciona.

## Próxima ação recomendada

Começar a implementação de código de **`targeted-jammer`** (seção 1 e 2 do
`tasks.md`) via skill `openspec-apply-change`, deixando a validação manual
(seção 3, e a de `plr-metric`) para quando houver acesso à bancada.
