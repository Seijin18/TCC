# Logs de referência — sistema funcionando corretamente

Cópias (os originais continuam em `logs/`) das sessões de bancada em que o
comportamento esperado foi verificado nos dados. Critério e evidência de
cada uma abaixo; ver a regra em `CLAUDE.md` (raiz do repositório).

Critério "enlace OK": pareamento + seed nos dois lados, ≥ 60 s de enlace
contínuo com tráfego de dados real (`msg_done`/`ack`), sem `loss_of_sync`
no trecho (ou recuperação automática confirmada).

| Sessão | Enlace contínuo | `msg_done` | Observação |
|---|---|---|---|
| `session-20260918-021916.jsonl` | 106 s | 1.846 | primeiro enlace estável pós hopping-por-evento |
| `session-20260918-031422.jsonl` | 725 s | 12.742 | enlace mais longo sem nenhuma perda; contém 3 cenários de jammer (abaixo) |
| `session-20260918-032703.jsonl` | 262 s | 4.218 | contém `scenario-20260918-062714` |
| `session-20260918-033231.jsonl` | 246 s | 3.476 | contém `scenario-20260918-063242` |
| `session-20260923-003036.jsonl` | 128 s | 629 | contém `scenario-20260923-033046` |
| `session-20260923-003328.jsonl` | 143 s | 734 | contém `scenario-20260923-033339` |
| `session-20260923-011357.jsonl` | 341 s | 310 | contém `scenario-20260923-041413` |
| `session-20260923-012211.jsonl` | 1.262 s | 376 | **re-pareamento automático após `loss_of_sync` funcionou** (16 s); contém `scenario-20260923-042225` |
| `session-20260924-152517.jsonl` | trechos de ~10 s entre perdas forçadas | ~890 (escravo) | **validação do handshake v2** (`pairing-handshake-hardening`): 20/20 re-pareamentos forçados (`repair-20260924-182542.json`, mediana 7,9 s, p95 9,0 s), re-ack após ack descartado (15:30), 5/5 reboots assimétricos (15:31), pareamento sob sweep (15:32-15:34 — enlace depois cai pelo jamming, ver tasks.md 5.4) |

Cenários (`scenario-*.json`/`.png`, fases baseline → jamming → recovery →
end; horários do nome em UTC, das sessões em horário local UTC-3):

- `scenario-20260918-061433`, `-061627`, `-062209` → sessão `20260918-031422`
- `scenario-20260918-062714` → `20260918-032703`
- `scenario-20260918-063242` → `20260918-033231`
- `scenario-20260923-033046` → `20260923-003036`
- `scenario-20260923-033339` → `20260923-003328`
- `scenario-20260923-041413` → `20260923-011357`
- `scenario-20260923-042225` → `20260923-012211`

| `session-20260925-004847.jsonl` | **601 s, um único pareamento, 0 `loss_of_sync`** | 2.612 | **soak com `hop-resync` + correção da enxurrada de controle** (jammer desligado, Wi-Fi real ativo em ch 28-45): 49/49 estacionamentos recuperados (`resync_ok`, mediana 2,2 s), 77 trocas de máscara aplicadas nos dois lados. Ainda com os experimentos 1.6/1.7 no firmware (removidos depois, sem efeito no resultado) |

| `session-20260925-010250.jsonl` | 4 fases de 3 min sob o mesmo pareamento (quedas só em adaptive/sweep) | ~1.300 | **enlace sob ataque** (PA_LOW): off 0 quedas/9,5% miss; **targeted 0 quedas**, 43,6% miss, 69/70 resync, 86 máscaras aplicadas nos dois lados; adaptive 3 quedas; sweep 6 quedas (dessensibilização fora da faixa). Fases em `hop-resync/tasks.md` 2.2 |

| `session-20260925-013616.jsonl` | 600 s de soak sem nenhuma queda + bateria A/B/C | 2.913 no soak | **soak com `hop-permutation`** (02:16-02:26): 0 `loss_of_sync`, 41/41 resync; depois bateria `targeted` `battery-20260925-044627` (não reprodutível — ver `targeted-jammer` 3.2) |
| `session-20260925-015833.jsonl` | 2 baterias A/B/C seguidas | — | **baterias reprodutíveis** após o retune do jammer (`battery-20260925-045843`/`-050631`): B e C sem nenhuma queda sob `targeted`, C com 8 máscaras aplicadas por rodada |
| `session-20260926-001430.jsonl` | **soak ~145s (0 loss, 0 slip) + bateria A/B/C** | 2.900+ no soak | **`time-slotted-hopping` (commit 9329d4f)**: salto por TEMPO. Soak sem jammer: 0 `loss_of_sync`, 0 reanchor, PLR do escravo 0,3–1,7% por janela (0,9% acumulado) e miss do mestre 1,0% nos 145 s (recontado do jsonl em 2026-10-27; o "miss 2-3%" anterior vinha de `time-slotted-hopping/tasks.md` e não reproduz), 30ms/slot exato, ~19 msg/s. Bateria `battery-20260926-031714` (hold ch10 PA_MAX): retenção **C=83%, B=49%**, A=0% (vs 16/8/0 pré-change, 7/4/0 por-evento); fase C jamming 60s com 0 `loss_of_sync`/0 slip, máscara converge sem super-ban. ~17% residual = desense de banda larga do PA_MAX (hardware) |
| `session-20260926-014218.jsonl` (time-slotted) + `session-20260926-015031.jsonl` (event-driven) | **comparação head-to-head A/B/C** | — | **event-driven vs time-slotted, teste IDÊNTICO** (hold ch10 PA_MAX, baseline 40s/jam 60s): `battery-20260926-044321` (time-slotted, `variants`=main) vs `battery-20260926-045108` (event-driven, `variants/event-driven/`=commit 26423a6). Retenção sob jamming: A 0%/0%, **B 45% vs 9%**, **C 78% vs 1%**; baseline ~20/s vs ~8/s. Gráfico: `compare-ev-vs-td.png` (`tools/validation/plot_compare.py`) |
| `session-20261021-143707.jsonl` | **bateria A/B/C com os 3 bugs corrigidos** | — | **sistema final (slot-phase-servo + half-open-handshake-recovery + channel-quality-sensing)** sob jammer hold ch10 PA_MAX (baseline 30s/jam 90s/recovery 30s): retenção de ACKs sob jamming **A=0%, B=94%, C=99%**; C com blacklist/rekey aplicado nos dois lados (ch10 banido), 0 `loss_of_sync` em B e C, 0 `half_open`, A re-pareia e recupera sozinho. Gráfico `battery-20261021-174727.png` (`battery-20261021-174727.json`) |

Baterias A/B/C (`battery-*.json`, fases com `scenario`; `.png` quando
gerado): `battery-20260925-041711` (1ª, `targeted`; é a que mostra o
self-healing de `dynamic-seed-blacklist` 4.4 — dados na sessão
`20260925-010250`), `-042708` (`adaptive`), `-045843` e `-050631`
(`targeted` após retune, reprodutíveis). Interpretação em
`openspec/changes/scenario-automation/tasks.md` 4.2.

> ⚠️ **RESSALVA (2026-09-25):** todas as baterias acima foram rodadas ANTES
> do achado de `jammer-emission-hardware` — o jammer emitia de forma
> intermitente/degradada (a portadora do clone caía ao longo da sessão sem
> reinit do rádio). Por isso B e C "sem quedas" e o "self-healing" delas
> refletem um atacante fraco, NÃO a defesa vencendo um ataque forte. Os
> números devem ser lidos com essa ressalva. A bateria abaixo é a primeira
> com o jammer confiável.

### Bateria A/B/C com o jammer CONFIÁVEL (`jammer-emission-hardware` + `jamming-mitigation-hardening`)

- `session-20260925-135209.jsonl` + `battery-20260925-165233.json`/`.png`
  (mode=`hold` ch10, power=`max` — atacante forte de canal fixo, portadora
  parada e reprodutível). Resultado por cenário (throughput na fase jamming /
  baseline do mesmo cenário; PLR médio sob jamming):

  | Cenário | retenção sob jamming | PLR jam | leitura |
  |---|---|---|---|
  | A (sem hopping) | **0%** (colapso total) | 59% | canal fixo negado por completo |
  | B (FHSS, sem mitigação) | **12%** | 40% | hopping sobrevive parcialmente, mas dessincroniza |
  | C (FHSS + mitigação) | **25%** | 25% | ~2× o B; menor PLR; recupera ao desligar o jammer |

  Demonstra a ordem esperada C > B > A com um ataque real, e o valor da
  mitigação endurecida (persistência de blacklist + detecção pelo escravo +
  anti-park). NÃO é recuperação total: resta o desalinhamento de fase temporal
  mestre/escravo sob jamming (ver `openspec/changes/jamming-mitigation-hardening/tasks.md`).
  Gráfico: 3 painéis (throughput×tempo, latência×tempo, throughput por fase com
  % de retenção).

## Referência, mas NÃO "funcionando corretamente" (ficou só em `logs/`)

- `session-20260923-015103.jsonl` (+ `scenario-20260923-045128`): pipeline
  de mitigação disparou de ponta a ponta (alerta → blacklist → 14
  `rekey_tx` / 18 `rekey_ack`), mas o enlace morreu logo depois (6
  `rekey_failed`, `loss_of_sync`) — é o gráfico "validação end to end" com
  a metade final em branco.
- `session-20260924-*`: re-pareamento pós-perda quebrado — ver
  `openspec/changes/archive/2026-09-24-pairing-handshake-hardening/design.md`.
