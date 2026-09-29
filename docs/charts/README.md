# Gráficos — comparação event-driven vs time-slotted

Gráficos de bancada da comparação entre as duas variantes de hopping (mesmo
teste: bateria A/B/C, jammer `hold ch10 PA_MAX`, baseline 40s / jamming 60s).
`main` = time-slotted; `variants/event-driven/` = event-driven (commit 26423a6).
Dados brutos (sessões `.jsonl`, `battery-*.json`) ficam em `logs/` (não
versionado) e as cópias qualificadas em `logs/working/`. Scripts que geram
estes PNGs: `tools/validation/plot_compare.py`, `plot_compare_metrics.py`,
`plot_battery.py`.

Cenários: **A** = sem hopping (canal fixo, nenhuma adaptação); **B** = FHSS
sem mitigação; **C** = sistema completo (FHSS + detecção + rekey/blacklist).
Taxa de sobrevivência = throughput na fase jamming ÷ baseline do mesmo cenário
(maior é melhor).

| Arquivo | Conteúdo |
|---|---|
| `comparacao-survival-event-vs-time.png` | Resumo: taxa de sobrevivência por cenário, throughput absoluto sob jamming, e throughput×tempo do Cenário C. Event-driven vs time-slotted. |
| `comparacao-metricas-event-vs-time.png` | Detalhado por cenário e fase: throughput, acks/s, miss/s, latência de ACK, loss_of_sync. |
| `bateria-time-slotted.png` | 4 painéis (throughput×t, acks/s×t, latência×t, acks/s por fase) da variante **time-slotted** (`battery-20260926-044321`). |
| `bateria-event-driven.png` | Mesmos 4 painéis da variante **event-driven** (`battery-20260926-045108`). |

Resultado (sobrevivência sob jamming): A 0%/0%, B **45% (time)** vs 9% (event),
C **78% (time)** vs 1% (event); baseline ~20 msg/s (time) vs ~8 (event). O
salto por tempo não dessincroniza sob jamming, então a detecção/blacklist
converge e o enlace não cai (0 `loss_of_sync` no C, contra 3 no event-driven).
