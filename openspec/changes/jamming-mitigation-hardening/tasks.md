## 1. Instrumentação e reprodução do baseline do problema

- [ ] 1.1 Reproduzir o gap com o jammer confiável: A/B/C com `mode hold ch10`
      (scripts de `jammer-emission-hardware`, `scratchpad/mitigation2.py`).
      Confirmar C ~17% e a oscilação da máscara (`e=blacklist` chrono).
- [ ] 1.2 Instrumentar (se preciso) a distinção da causa de `reset_to_pairing`
      (jamming vs limpo) e a atribuição de falha por hop confirmado.

## 2. Persistir a blacklist através do re-pareamento (causa 1)

- [ ] 2.1 Em `mestre/src/node.h`, não zerar `g_channel_fail`/`g_channel_mask`/
      `g_channel_blacklisted_since` num `reset_to_pairing()` causado por perda
      sob jamming (ou sempre preservar e deixar o decaimento reincluir).
- [ ] 2.2 Preservar o mesmo estado no escravo ao voltar a escanear, e
      re-sincronizar a máscara no re-pareamento (mestre e escravo consistentes).
- [ ] 2.3 Coordenar com `blacklist-decay`: o cooldown continua reincluindo
      canais quando o jammer sai (ban não vira permanente).

## 3. Ban rápido + distinção jamming/dessincronismo (causas 2 e 3)

- [ ] 3.1 Ban imediato de um canal com K falhas dominantes, sem esperar alerta
      nem `LOSS_HOPS`; propagar num hop de canal limpo.
- [ ] 3.2 Um miss num canal já banido/perto do ban NÃO conta para
      `g_hops_since_ack`/`LOSS_HOPS` (não escala para re-pareamento).
- [ ] 3.3 Manter `hop_counter` alinhado sob um hop jammed (coordenar com
      `hop-resync`; não estacionar no canal jammed).
- [ ] 3.4 Só atribuir falha a um canal com evidência de que os dois lados
      estavam nele (usar `hop_counter` carregado no pacote) — evitar contaminar
      canais inocentes.

## 4. Escalonamento para reseed quando a blacklist satura

- [ ] 4.1 Se fração de canais banidos > limiar (ex. 1/4) e PLR alto, disparar
      reseed (rekey) além do ban. Reusar o caminho de rekey existente.
- [ ] 4.2 Garantir que o reseed não entra em loop com o ban (histerese).

## 5. Validação com o atacante confiável

- [ ] 5.1 `pio run` (envs de HW) e `pio test -e native_test` limpos. Testes
      nativos novos para a lógica pura alterada (atribuição/máscara/escalonamento).
- [ ] 5.2 A/B/C `mode hold ch10`: **critério** C ≥ 80% do baseline, máscara
      convergindo para `0xFFFE`, sem ciclos de `reset_to_pairing` em regime.
- [ ] 5.3 A/B/C `adaptive` (follower): documentar teto de recuperação e o ganho
      do reseed (4) vs só blacklist.
- [ ] 5.4 Soak sem jammer: 0 `loss_of_sync`, sem bans espúrios (não regredir a
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

### Resultado (preencher ao rodar)

- Baseline reproduzido (C ~17%, máscara oscila?):
- Persistência da blacklist no re-pareamento (efeito):
- Ban rápido + distinção jamming/dessinc (efeito na convergência da máscara):
- Reseed por saturação (efeito no follower):
- A/B/C final (hold): C = ___% do baseline; máscara convergiu para ___?
- A/B/C final (adaptive): teto de recuperação = ___%
- Soak sem jammer sem regressão? (s/n):
