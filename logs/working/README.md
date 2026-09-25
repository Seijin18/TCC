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

## Referência, mas NÃO "funcionando corretamente" (ficou só em `logs/`)

- `session-20260923-015103.jsonl` (+ `scenario-20260923-045128`): pipeline
  de mitigação disparou de ponta a ponta (alerta → blacklist → 14
  `rekey_tx` / 18 `rekey_ack`), mas o enlace morreu logo depois (6
  `rekey_failed`, `loss_of_sync`) — é o gráfico "validação end to end" com
  a metade final em branco.
- `session-20260924-*`: re-pareamento pós-perda quebrado — ver
  `openspec/changes/archive/2026-09-24-pairing-handshake-hardening/design.md`.
