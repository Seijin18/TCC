# Gráficos para o relatório final

Figuras de bancada selecionadas para o relatório. Cada uma é válida **dentro
da sua própria campanha de medição**. Atenção a uma ressalva importante de
comparabilidade (abaixo). Dados brutos (`.jsonl`, `battery-*.json`) ficam em
`logs/` (não versionado); cópias qualificadas em `logs/working/`. Scripts:
`tools/validation/plot_battery.py`, `plot_compare.py`, `plot_compare_metrics.py`.

Cenários em todas as baterias: **A** = sem hopping (canal fixo, nenhuma
adaptação); **B** = FHSS sem mitigação; **C** = sistema completo (FHSS +
detecção + rekey/blacklist). Taxa de sobrevivência = throughput na fase
jamming ÷ baseline do mesmo cenário (maior é melhor). Atacante: jammer de um
rádio em `hold ch10 PA_MAX` (o atacante confiável desta bancada).

## Figuras principais

| Arquivo | O que demonstra |
|---|---|
| `bateria-abc-sistema-final.png` | **Resultado final do sistema** (2026-10-21, firmware com as 3 correções: servo de fase, watchdog de pareamento, qualidade por canal). A/B/C sob `hold ch10 PA_MAX`. Sobrevivência sob jamming: **A=0%, B=94%, C=99%**; C aplica blacklist nos dois lados, 0 `loss_of_sync` em B/C, A cai e re-pareia sozinho. |
| `comparacao-survival-event-vs-time.png` | **Impacto do redesenho do transporte** (event-driven → time-slotted, 2026-09-26, mesmo teste, mesma sessão). Sobrevivência: B **9%→45%**, C **1%→78%**; o event-driven dessincroniza sob jamming e o C colapsa, o time-slotted se mantém. É a justificativa da escolha de arquitetura. |
| `estabilidade-deriva-antes-depois.png` | **Confiabilidade do enlace sem jammer** — correção da deriva de clock (`slot-phase-servo`). Antes: throughput decai com o tempo (serrote, re-pareamento mascarava); depois: estável ~100 fragmentos/s por 15 min. |

## Figuras de apoio

| Arquivo | O que demonstra |
|---|---|
| `comparacao-metricas-event-vs-time.png` | Detalhe por cenário/fase (throughput, acks/s, miss/s, latência, `loss_of_sync`) da comparação event-vs-time. |
| `bateria-time-slotted.png` / `bateria-event-driven.png` | As duas baterias brutas (4 painéis cada) por trás da comparação acima. |

## Ressalvas (ler antes de citar números no texto)

- **Não comparar números absolutos entre campanhas de medição diferentes.** O
  C=78% da comparação de 2026-09-26 e o C=99% da bateria final de 2026-10-21
  foram medidos com **acoplamento físico diferente** (as placas foram
  desconectadas/reconectadas entre as duas datas, o que muda o acoplamento de
  campo próximo entre jammer e escravo). Cada figura é válida internamente: a
  de 09-26 isola o efeito do redesenho de transporte (mesma sessão, mesmo
  acoplamento); a de 10-21 mostra o sistema final endurecido. Não apresentar
  78% e 99% como progressão direta.
- **A vantagem de C sobre B é modesta contra um atacante de um rádio.** Um
  jammer que trava um único canal custa ~6% ao hopping puro (B), porque o
  enlace só passa ~1/16 do tempo no canal atacado; a mitigação (C) fecha a
  maior parte dessa folga. A diferença C≫B apareceria contra um atacante
  multi-canal ou que persegue a sequência de salto — fora do alcance desta
  bancada de um rádio.
- **Quem banе o canal é a evidência de ack do próprio mestre (reativa).** A
  detecção por ocupação do analisador (`channel-quality-sensing`) é um sinal
  complementar: ela conserta o alerta antes travado pelo Wi-Fi e dá
  observabilidade por canal, mas, para jammer forte, a blacklist reativa do
  mestre já exclui o canal ~1 s antes do flag de ocupação. Não atribuir a C a
  um ganho de detecção proativa que os dados não mostram.
