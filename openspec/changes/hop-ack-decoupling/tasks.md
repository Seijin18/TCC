## 1. Preparação: nova cadência e re-derivação de constantes (sem mudar lógica)

- [x] 1.1 Introduzir `ATTEMPT_TIMEOUT_MS` (~30 ms) em `shared/protocol.h` e
      passar a usá-lo como janela de escuta por hop do escravo e de espera de
      ack do mestre (hoje `HOP_TIMEOUT_MS`=220 / `attempt_wait_ms`=25).
      `HOP_TIMEOUT_MS` removido (só sobravam referências em comentário, todas
      atualizadas).
- [x] 1.2 Re-derivar as constantes de CONTAGEM de hops para preservar os mesmos
      orçamentos de TEMPO na nova cadência (~30 ms/hop): `LOSS_HOPS` 25→180,
      `RESYNC_MISS_HOPS` 6→45, `RESYNC_PARK_MAX_MS` agora
      `(TABLE_SIZE+2)*ATTEMPT_TIMEOUT_MS`=540, `CONTROL_RETRY_HOPS` 6→45,
      `CONTROL_APPLY_MARGIN` 2→15, `RECOVERY_COOLDOWN_HOPS` 200→1500,
      `THROUGHPUT_WINDOW_HOPS` 50→350, `g_window_size` 50→350,
      `RESYNC_QUALITY_DECAY_HOPS` 64→450 (contador `g_resync_quality_hops`
      alargado p/ uint16_t), `MASK_HYSTERESIS_HOPS` 16→112. `CHANNEL_FAIL_THRESHOLD`
      MANTIDO em 3 (conta VISITAS, não tempo). `static_assert` reexpresso em
      `ATTEMPT_TIMEOUT_MS`.
- [~] 1.3 `pio run` (7 envs) + `pio test -e native_test` (58 casos) VERDES,
      incluindo o `static_assert` do orçamento. **Soak sem jammer ≥ 10 min
      pendente (hardware — precisa das duas placas).**

## 2. Mestre: uma tentativa por canal, salto por tentativa (o núcleo)

- [x] 2.1 Reescrever o laço de fragmento em `mestre/src/node.h`: enviar o
      fragmento `g_current_fragment` UMA vez em `channel(g_hop_counter)`, esperar
      `ATTEMPT_TIMEOUT_MS` pelo ack; avançar `g_hop_counter` SEMPRE; avançar
      `g_current_fragment` só no ack (o não-ackado é reenviado no próximo canal).
      O `while` de reenvio no mesmo canal (janela de 220 ms) foi removido.
- [x] 2.2 Preservado: attribution gate (`ATTRIB_MAX_HOPS_SINCE_ACK`=1),
      `suspect_idx`, throughput/latência e a máquina de controle
      (blacklist/reseed) intactos — só muda QUANDO o hop avança, não a semântica
      de miss/ack. `g_current_fragment++` só no ack / `g_hop_counter++` sempre já
      eram assim; o que mudou foi tirar a janela de reenvio.
- [~] 2.3 Build verde. **Soak sem jammer + medição sob `hold ch10` pendentes
      (hardware).**

## 3. Escravo: janela curta, lockstep, park como último recurso

- [x] 3.1 Escuta por hop = `ATTEMPT_TIMEOUT_MS` (avança em lockstep com o mestre);
      adoção de `hop_counter` no RX e dedup por `received_bitmap` mantidos.
- [x] 3.2 `park` como último recurso: `RESYNC_MISS_HOPS` elevado 6→45 (na nova
      escala). O `g_resync_miss_count` já zera em QUALQUER RX válido (frag ou
      controle), então 45 timeouts CONSECUTIVOS sem nada = desalinhamento
      genuíno e sustentado, não um canal jammed pontual.
- [~] 3.3 Build verde. **Soak sem jammer + medição sob `hold`/`adaptive`
      pendentes (hardware).**

## 4. Validação comparativa

- [ ] 4.1 `run_battery.py --jammer-mode hold --jammer-power max` e comparar C vs
      os 25% atuais (mesmos scripts/gráficos). Alvo: subir claramente rumo a
      ~94%; PLR sob jamming cair; A continua colapsando (sanidade).
- [ ] 4.2 Bateria `adaptive` (follower) para o segundo perfil de atacante.
- [ ] 4.3 Se qualificar, copiar a sessão para `logs/working/` (`CLAUDE.md`).

## 5. Fechamento

- [ ] 5.1 Atualizar `jamming-mitigation-hardening` (o teto foi destravado?) e o
      `ROADMAP.md`/memória `project-rf-status` com os novos números.
- [ ] 5.2 `openspec validate hop-ack-decoupling --strict` e arquivar.

---

### Resultado de bancada (2026-09-26) — parcial, com achados que reorientam

Hardware: placa_a (mestre+analisador), placa_b (escravo+jammer). Wi-Fi real
ativo. Jammer `hold ch10` PA_MAX.

**Achado 1 — o casamento de fase, não o counter, é o gargalo sem relógio.**
Janelas casadas de 30 ms nos dois lados (o desenho original) deram **65% de
miss uniforme em TODOS os canais, em rajadas**, sem jammer. Causa: o escravo
avança por EVENTO no RX (reancora a fase), mas o mestre só avança depois do ack
+ ~4 telemetrias que disputam o mutex com o analisador → lead de ~15-20 ms de
jitter; numa janela de 30 ms isso estoura e o escravo avança na frente →
rajada de miss. **Multi-send DENTRO da janela piorou** (perda contínua): cada
reenvio é mais uma chance de o escravo avançar cedo num envio cujo ack se
perde, enquanto o mestre ainda está no canal velho → dessincroniza.

**Correção que passou o gate sem jammer:** janela de escuta do escravo MAIOR
que a do mestre (`ESCRAVO_LISTEN_MS`=60 vs `ATTEMPT_TIMEOUT_MS`=30). O caminho
de sucesso continua por evento (rápido); a janela larga só absorve o lead do
mestre quando o escravo ESPERA. Resultado (soak `session-20260925-230940`,
~100 s): **0 `loss_of_sync`** (inclui o startup, que as versões casadas não
passavam), miss 4-49% (varia, margem melhor mas não perfeita), `msg_done`
4-8,6/s (par com o baseline antigo ~4,4-4,9/s), latência p50=14 ms. GATE
SEM-JAMMER: **PASSOU** (0 loss, throughput mantido).

**Achado 2 — sob `hold` PA_MAX a retenção é limitada por DESSENSIBILIZAÇÃO de
banda larga, e a nova cadência REGREDIU a retenção.** Bateria
`battery-20260926-021206` vs a antiga `battery-20260925-165233` (mesmo modo
hold/PA_MAX), retenção = `msg_done/s` jamming / baseline:

| Cenário | ANTES (pré-change) | DEPOIS (v3) |
|---|---|---|
| A (sem hopping) | 0% | 0% |
| B (FHSS, sem mitigação) | 8% | 4% |
| C (mitigação completa) | **16%** | **7%** |

Detalhe do C sob jamming: a portadora PA_MAX no ch10 (2410 MHz)
**dessensibiliza o front-end do NRF24 em banda larga** — canais PRÓXIMOS em
frequência (15, 22, 27, 46, 100, 110) dão 100% de miss, canais LONGE (125,
105, 94, 82) ainda passam parcialmente. O ch10 é banido cedo e some. A
blacklist então **super-bane** (máscara cai para 2-4 canais ativos de 16),
porque muitos canais realmente falham (desense), não só o atacado. Bane o
certo E os vizinhos mortos → sobram poucos canais → throughput baixo. Bate com
a memória `project_rf_status` ("hold 1 canal nega ~95-100% de FHSS; blacklist
recupera só ~17%").

**Hipótese da regressão 16%→7%:** a cadência ~7x mais rápida faz a atribuição/
ban dispararem ~7x mais rápido (mais visitas/s aos canais que parecem ruins sob
desense), super-banindo mais agressivo que o sistema antigo mais lento — apesar
de `MASK_HYSTERESIS_HOPS` ter sido re-derivado. As constantes de blacklist
foram re-derivadas por EQUIVALÊNCIA DE TEMPO, mas sob desense a dinâmica é por
VISITA, e visitas/s subiram ~7x. Re-tuning da blacklist para a nova cadência
(ou distinguir desense de jamming pontual) é o próximo passo — território de
`jamming-mitigation-hardening`, que este change deveria destravar.

**Pendente / decisão do usuário:** (a) confirmar se 16%→7% é regressão real ou
variância de desense entre runs (re-rodar); (b) re-tunar blacklist p/ a cadência
rápida (anti-super-ban); (c) testar em PA menor, onde a desense é localizada e a
blacklist pode de fato ajudar; (d) bateria `adaptive`. Não arquivar até resolver.
