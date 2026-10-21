## Escopo enxuto (decisão 2026-10-21)

O gatilho conhecido do travamento (falha de rekey sob mitigação pela deriva)
foi eliminado por `slot-phase-servo` e o travamento não reproduz mais (bateria
+ 15 ciclos de estresse com mitigação ligada: 0 travas, 0 `rekey_failed`).
Decisão do autor: implementar **só** o watchdog de auto-recuperação do
escravo como rede de segurança; descartar o escopo original de diagnóstico de
lado surdo, instrumentação de RPD e watchdog do mestre (tornados
desnecessários pela correção do #2).

## 1. Lógica pura

- [ ] 1.1 `shared/pairing.h`: função pura
      `half_open_should_recover(sessions_without_data, threshold)` (+ qualquer
      estado de contagem que faça sentido isolar) e testes em
      `test/test_pairing`.

## 2. Escravo

- [ ] 2.1 Contar sessões provisórias consecutivas com nonce novo que nunca
      confirmam `FragmentPacket`; zerar ao confirmar (CONFIRMED) e em
      `reset_to_scanning` por comando.
- [ ] 2.2 Ao atingir `HALF_OPEN_SESSIONS` (constante em `shared/protocol.h`):
      emitir `e=half_open`, `radio_reinit()` completo, voltar a varrer a
      partir de um índice de candidato / fase de dwell **aleatorizados**
      (quebra ressonância de fase com o acampamento do mestre, imitando o
      `reset` manual que recuperava o estado preso).
- [ ] 2.3 Logger: registrar `half_open` em `state.json`/`events.log`.

## 3. Validação em bancada

- [ ] 3.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [ ] 3.2 Não-regressão: bateria `hold` ch10 `PA_MAX` + 10 re-pareamentos
      normais — o watchdog NÃO dispara em operação normal (0 `half_open`), e
      o pareamento continua subindo em ~3-4 s.
- [ ] 3.3 (Se possível) injetar o estado meio-aberto artificialmente (ex.:
      um comando de teste que force o escravo a ignorar o próximo
      `FragmentPacket` por N sessões) e confirmar `half_open` + recuperação
      ≤ 15 s. Se não for viável sem mudar firmware de teste, documentar que a
      validação do disparo fica dependente de uma reprodução futura.
- [ ] 3.4 Se qualificar, copiar sessão para `logs/working/`.

## 4. Fechamento

- [ ] 4.1 Sincronizar o spec delta com o implementado.
- [ ] 4.2 `openspec validate half-open-handshake-recovery --strict` e arquivar.
