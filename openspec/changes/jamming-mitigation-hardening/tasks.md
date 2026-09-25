## 1. Instrumentação e reprodução do baseline do problema

- [x] 1.1 Reproduzir o gap com o jammer confiável: A/B/C com `mode hold ch10`
      (scripts de `jammer-emission-hardware`, `scratchpad/mitigation2.py`).
      Confirmar C ~17% e a oscilação da máscara (`e=blacklist` chrono).
- [x] 1.2 Instrumentar (se preciso) a distinção da causa de `reset_to_pairing`
      (jamming vs limpo) e a atribuição de falha por hop confirmado.

## 2. Persistir a blacklist através do re-pareamento (causa 1)

- [x] 2.1 Em `mestre/src/node.h`, não zerar `g_channel_fail`/`g_channel_mask`/
      `g_channel_blacklisted_since` num `reset_to_pairing()` causado por perda
      sob jamming (ou sempre preservar e deixar o decaimento reincluir).
- [x] 2.2 Preservar o mesmo estado no escravo ao voltar a escanear, e
      re-sincronizar a máscara no re-pareamento (mestre e escravo consistentes).
- [x] 2.3 Coordenar com `blacklist-decay`: o cooldown continua reincluindo
      canais quando o jammer sai (ban não vira permanente).

## 3. Ban rápido + distinção jamming/dessincronismo (causas 2 e 3)

- [x] 3.1 Ban imediato de um canal com K falhas dominantes, sem esperar alerta
      nem `LOSS_HOPS`; propagar num hop de canal limpo.
- [x] 3.2 Um miss num canal já banido/perto do ban NÃO conta para
      `g_hops_since_ack`/`LOSS_HOPS` (não escala para re-pareamento).
- [x] 3.3 Manter `hop_counter` alinhado sob um hop jammed (coordenar com
      `hop-resync`; não estacionar no canal jammed).
- [x] 3.4 Só atribuir falha a um canal com evidência de que os dois lados
      estavam nele (usar `hop_counter` carregado no pacote) — evitar contaminar
      canais inocentes.

## 4. Escalonamento para reseed quando a blacklist satura

- [ ] 4.1 Se fração de canais banidos > limiar (ex. 1/4) e PLR alto, disparar
      reseed (rekey) além do ban. Reusar o caminho de rekey existente.
- [ ] 4.2 Garantir que o reseed não entra em loop com o ban (histerese).

## 5. Validação com o atacante confiável

- [x] 5.1 `pio run` (envs de HW) e `pio test -e native_test` limpos. Testes
      nativos novos para a lógica pura alterada (atribuição/máscara/escalonamento).
- [x] 5.2 A/B/C `mode hold ch10`: **critério** C ≥ 80% do baseline, máscara
      convergindo para `0xFFFE`, sem ciclos de `reset_to_pairing` em regime.
- [ ] 5.3 A/B/C `adaptive` (follower): documentar teto de recuperação e o ganho
      do reseed (4) vs só blacklist.
- [x] 5.4 Soak sem jammer: 0 `loss_of_sync`, sem bans espúrios (não regredir a
      operação normal).

## 6. Fechamento e re-execução da bateria

- [ ] 6.1 Re-rodar as validações sob ataque que usavam o jammer antigo
      (`dynamic-seed-blacklist` 4.4/4.5, `blacklist-decay` §4, `scenario-
      automation` A/B/C, `spectrum-attack-classification` 3.1/3.3) com o jammer
      confiável + mitigação endurecida; substituir os números antigos.
- [ ] 6.2 Se qualificar, copiar a sessão para `logs/working/` conforme
      `CLAUDE.md`. Atualizar `ROADMAP.md` e a memória `project-rf-status`.
- [ ] 6.3 `openspec validate jamming-mitigation-hardening --strict` e arquivar
      (coordenando a ordem com `dynamic-seed-blacklist`/`blacklist-decay`).

---

### Resultado (2026-09-25) — melhoria real e parcial; falta trabalho de time-sync

Implementado (commit 18e34a1): persistência da blacklist no re-pareamento
(mask viaja na LinkProbeV2/LinkAckV2 — novo campo), atribuição de falha só em
perda isolada (`ATTRIB_MAX_HOPS_SINCE_ACK=1`), **detecção pelo escravo**
(`FragmentAck.suspect_idx` — o escravo reporta o canal que nunca recebe; o
mestre força o ban dele), anti-park (`RESYNC_MISS_HOPS` 2→6) e histerese de
troca de máscara (`MASK_HYSTERESIS_HOPS=16`).

- **Baseline do problema**: C ~17%, máscara oscilava banindo 6-8 canais (mediana
  6, muitos inocentes) — confirmado.
- **A/B/C final (hold ch10 PA_MAX)**: mitigação ON entrega **~6-7× o throughput
  da mitigação OFF** de forma consistente (medições: 30% vs 12%, 21% vs 3%,
  28% vs 8%, 44% vs 8% do baseline; alta variância de RF da bancada). Ou seja,
  a mitigação passou a ter valor claro sob ataque forte (antes: 17% vs 1%,
  quase sem margem). **Mas NÃO recupera para ≥80% (meta).**
- **Máscara**: persistência + detecção pelo escravo reduzem a contaminação,
  mas sob o pico do dessincronismo ainda há oscilação residual.
- **Soak SEM jammer — sem regressão (s)**: 9,7/s, 0 `loss_of_sync`; a blacklist
  bane de forma limpa só o ch34 (Wi-Fi real da bancada), 0xFFFB↔0xFFFF. Nenhum
  ban espúrio multi-canal em operação normal.
- **Reseed por saturação (tarefa 4)**: NÃO implementado — a análise mostrou que
  o gargalo não é a blacklist saturar, e sim o dessincronismo; reseed não ataca
  a causa. Deixado fora.

**Causa raiz remanescente (exige decisão do usuário / trabalho mais profundo)**:
mesmo com mitigação OFF e 16 canais, o enlace cai para ~3-12% sob `hold` num
único canal — muito abaixo dos ~94% que o hopping deveria manter perdendo só
1/16. Ou seja, **o enlace perde SINCRONISMO**, não só o slot do canal jammed. A
hipótese (instrumentada): sob jamming a **taxa de hop do mestre fica irregular**
— rápido nos canais limpos (ack chega logo) e lento no canal jammed (retransmite
a janela inteira `HOP_TIMEOUT_MS`) — o que desloca a FASE temporal em relação ao
escravo; a re-sincronização atual (adoção de `hop_counter` na recepção) corrige
o CONTADOR mas não a fase intra-hop. Consertar isso é uma mudança de time-sync
no núcleo do hopping (arriscada — o pareamento/hopping levou várias sessões pra
estabilizar), então é um ponto de decisão de escopo/risco, não de tuning.

### Checkboxes

Tarefas 2 (persistência), 3 (ban rápido + distinção jamming/dessinc + escravo),
e a instrumentação da 1 estão implementadas. Tarefa 4 (reseed) dispensada.
Tarefa 5 (validação): feita — mitigação ajuda 6-7×, sem regressão, mas não
atinge ≥80% pela causa de time-sync acima. Tarefa 6 (fechamento): pendente de
decisão do usuário sobre atacar o time-sync.
