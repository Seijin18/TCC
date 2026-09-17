# Roadmap de implementação — TCC Anti-Jamming

Consolida o estado dos changes ativos (`openspec/changes/`) num plano de
execução único. Não substitui `project.md` (que continua sendo a fonte de
verdade de contexto/dependências) — é a camada de "o que fazer agora, em que
ordem" derivada dele. Atualizar esta lista conforme tasks forem fechadas ou
changes forem arquivados.

## Estado validado (código já implementado)

- **`plr-metric`** (Fase 1) — seções 1 e 2 **concluídas**; falta só
  validação manual em bancada (3.1/3.2).
- **`targeted-jammer`** (Fase 2) — seções 1 e 2 **concluídas** (modo
  `sweep`/`targeted` no jammer e no harness `validacao`); falta validação
  visual/PLR em bancada (3.2/3.3).
- **`spectrum-attack-classification`** (Fase 3) — seções 1 e 2
  **concluídas** (heurística `alvo_avg`/`fora_avg` e sinalização
  analisador→mestre); falta calibração em bancada (seção 3).

Todos com código pronto, bloqueados apenas em hardware — **e o hardware, por
sua vez, está bloqueado** pelos itens abaixo (a bancada de 2 placas não roda
como está: ver `two-board-topology` seção 7).

## Estado 100% pendente

- **`dynamic-seed-blacklist`** (Fase 4) — design completo, nenhuma linha de
  código. Depende de `adaptive-rendezvous` entregar `shared/protocol.h`
  antes de começar a seção 2 (pacotes de controle).
- **`scenario-automation`** (Fase 5) — depende de todos os anteriores.
- **`telemetry-logging`** (infra, novo 2026-09-17) — sem dependências,
  pré-requisito de qualquer validação de bancada auditável.
- **`adaptive-rendezvous`** (novo 2026-09-17) — sem dependências,
  pré-requisito de `dynamic-seed-blacklist` (protocol.h) e de qualquer teste
  de pareamento sob ataque.

## Bloqueio imediato: bancada de 2 placas não compila corretamente

`two-board-topology` seções 1–5 estão `[x]`, mas a seção 7 (adicionada
2026-09-17 ao tentar rodar de verdade) documenta que os `node.h` de
`analisador`/`jammer` nunca foram atualizados para usar HSPI — como estão,
colocariam o nó secundário de cada placa combinada no mesmo barramento
físico do primário (mesmo pino CE/CSN do mestre/escravo). `env:placa_b`
nunca foi sequer compilado. **Nada do resto pode ser validado em bancada até
a seção 7 fechar.**

## Ordem de execução recomendada

```
telemetry-logging ──────────────┐
                                 ├─→ two-board-topology §7 (pin fix) ─→ bancada liberada
adaptive-rendezvous ─(protocol.h)┘         │
        │                                   ▼
        │                    plr-metric §3, targeted-jammer §3,
        │                    spectrum-attack-classification §3
        ▼                                   │
dynamic-seed-blacklist ←──────────────────┘
        │
        ▼
scenario-automation
```

1. **`telemetry-logging`** — infraestrutura de log persistente
   (`shared/telemetry.h`, `tools/logger/main.py`, dashboard lendo arquivo).
   Sem isso, nenhuma validação de bancada abaixo produz evidência
   reaproveitável entre sessões.
2. **`adaptive-rendezvous`** — `shared/protocol.h`, pareamento em canal
   limpo, tráfego contínuo, seed não-bloqueante, resync de `hop_counter`.
   Pode avançar em paralelo com (1).
3. **`two-board-topology` §7** — corrigir pinagem SPI do nó secundário em
   `analisador/src/node.h` e `jammer/src/node.h` (mover para HSPI), guardas
   de `pinMode` no pino de alerta (GPIO 4 = HSPI SCK) e no LED (GPIO 2 =
   HSPI MISO). **Bloqueia toda validação de bancada seguinte.**
4. **Validações de bancada já desbloqueáveis**: `plr-metric` §3,
   `targeted-jammer` §3, `spectrum-attack-classification` §3,
   `two-board-topology` §6, `adaptive-rendezvous` §6 — todas dependem só de
   (2) e (3).
5. **`dynamic-seed-blacklist`** — implementação de código (seções 1–3) só
   começa depois que (2) entregar `shared/protocol.h`; validação (seção 4)
   depois que (4) confirmar PLR e alerta funcionando em bancada.
6. **`scenario-automation`** — fecha o projeto, depende de todos os
   anteriores validados.

## Próxima ação recomendada

Implementar `telemetry-logging` e `adaptive-rendezvous` (paralelos, sem
dependência entre si), seguido imediatamente por `two-board-topology` §7 —
essa sequência é o que desbloqueia toda validação manual pendente nos outros
três changes já com código pronto.
