## Escopo enxuto (decisão 2026-10-21)

O gatilho conhecido do travamento (falha de rekey sob mitigação pela deriva)
foi eliminado por `slot-phase-servo` e o travamento não reproduz mais (bateria
+ 15 ciclos de estresse com mitigação ligada: 0 travas, 0 `rekey_failed`).
Decisão do autor: implementar **só** o watchdog de auto-recuperação do
escravo como rede de segurança; descartar o escopo original de diagnóstico de
lado surdo, instrumentação de RPD e watchdog do mestre (tornados
desnecessários pela correção do #2).

## 1. Lógica pura

- [x] 1.1 `shared/pairing.h`: função pura
      `half_open_should_recover(sessions_without_data, threshold)` (+ qualquer
      estado de contagem que faça sentido isolar) e testes em
      `test/test_pairing`.

## 2. Escravo

- [x] 2.1 Contar sessões provisórias consecutivas com nonce novo que nunca
      confirmam `FragmentPacket`; zerar ao confirmar (CONFIRMED) e em
      `reset_to_scanning` por comando.
- [x] 2.2 Ao atingir `HALF_OPEN_SESSIONS` (constante em `shared/protocol.h`):
      emitir `e=half_open`, `radio_reinit()` completo, voltar a varrer a
      partir de um índice de candidato / fase de dwell **aleatorizados**
      (quebra ressonância de fase com o acampamento do mestre, imitando o
      `reset` manual que recuperava o estado preso).
- [x] 2.3 Logger: registrar `half_open` em `state.json`/`events.log`.

## 3. Validação em bancada

- [x] 3.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [x] 3.2 Não-regressão: bateria `hold` ch10 `PA_MAX` + 10 re-pareamentos
      normais — o watchdog NÃO dispara em operação normal (0 `half_open`), e
      o pareamento continua subindo em ~3-4 s.
- [~] 3.3 **Validação parcial (2026-10-21)**: o estado preso original não
      reproduz mais (gatilho eliminado pelo #2), então o disparo não pôde ser
      forçado de forma representativa. Evidência obtida: (a) ANTES do fix do
      reset completo, o cenário A disparou `half_open` 5x SEM recuperar
      (`battery-20261021-144452`); (b) DEPOIS, cenário A x5 recuperou sozinho
      em todas (`session-20261021-115813`), com o watchdog disparando 1x
      (sessions=4) e recuperando ~1,2 s após `jam off`; (c) a ação de
      recuperação é agora código IDÊNTICO ao `reset` manual comprovadamente
      recuperador. Cobertura empírica do caminho disparo→recuperação é de 1
      amostra (o estado raramente chega a 4 sessões sem confirmar). Opção de
      reforço: baixar HALF_OPEN_SESSIONS temporariamente p/ forçar mais
      disparos — não feito (custo/benefício baixo dado o argumento mecânico).
- [ ] 3.4 Se qualificar, copiar sessão para `logs/working/`.

## 4. Fechamento

- [ ] 4.1 Sincronizar o spec delta com o implementado.
- [ ] 4.2 `openspec validate half-open-handshake-recovery --strict` e arquivar.
