## 1. Instrumentar e confirmar o lado surdo

- [ ] 1.1 Mestre: amostrar `testCarrier()` durante `LINK_ACK_WAIT_MS` em
      `try_establish_link()` e publicar `rx_rpd=` em `pair_stats`.
- [ ] 1.2 Escravo: contar sessões de pareamento seguidas sem
      `FragmentPacket` e emitir `e=half_open_suspect;sessions=N` (só
      telemetria nesta etapa, sem ação).
- [ ] 1.3 `pio test -e native_test` + `pio run` de todos os envs; gravar
      `placa_a`/`placa_b`.
- [ ] 1.4 Reproduzir o estado preso: laço de `reset` forçados + bateria
      `hold` ch10 `PA_MAX` até ocorrer (registrar quantas tentativas).
- [ ] 1.5 No estado preso: (a) `spectrum.csv` no canal de acampamento —
      há energia a cada ~100 ms? (b) `placa_a reset` sozinho vs.
      `placa_b reset` sozinho — qual destrava? Registrar no `design.md`
      (confirma/refuta a hipótese D1).

## 2. Recuperação

- [ ] 2.1 `shared/pairing.h`: função pura de decisão de meio-aberto
      (contadores + backoff) + testes em `test/test_pairing`.
- [ ] 2.2 Mestre: watchdog de acampamentos silenciosos
      (`HALF_OPEN_SILENT_CAMPS`, backoff até 60 s) → `radio_reinit()` +
      `e=regs;why=half_open`.
- [ ] 2.3 Escravo: `HALF_OPEN_SESSIONS` sessões sem fragmento →
      `e=half_open`, `radio_reinit()`, volta a varrer.
- [ ] 2.4 `radio_reinit()` dos dois lados: readback dos registradores
      críticos + fallback para reinicialização completa; conferir
      `flush_rx()` na reinit do mestre.
- [ ] 2.5 Logger: `half_open` e `rx_rpd` em `state.json`.

## 3. Validação em bancada

- [ ] 3.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [ ] 3.2 30 `reset` forçados seguidos: nenhum período sem tráfego > 15 s.
- [ ] 3.3 Bateria A/B/C `hold` ch10 `PA_MAX`, 3 vezes: o cenário C sempre
      pareia; nenhum período sem tráfego > 15 s fora da fase de jamming do
      cenário A.
- [ ] 3.4 Se qualificar, copiar sessão e baterias para `logs/working/`
      (`CLAUDE.md`).

## 4. Fechamento

- [ ] 4.1 Sincronizar spec delta com o que foi implementado (D1 pode mudar
      a causa registrada).
- [ ] 4.2 `openspec validate half-open-handshake-recovery --strict` e
      arquivar.
