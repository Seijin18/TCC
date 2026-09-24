## 1. Firmware — modo canal fixo e métricas de latência/throughput

- [x] 1.1 Adicionar comando serial de modo (`mode hopping|fixed`) a
      `mestre/src/main.cpp` e `escravo/src/main.cpp`, análogo ao já usado
      em `targeted-jammer`; default `hopping` (comportamento atual
      preservado).
- [x] 1.2 Em modo `fixed`, usar um canal constante (`TEST_FIXED_CHANNEL =
      HOPPING_CHANNELS[0]`, `shared/hopping.h`) em vez de
      `get_next_channel`/`get_next_active_channel` em ambos os nós — ver
      design.md "Implementado (2026-09-24)" pela decisão de constante fixa
      em vez de configurável.
- [x] 1.3 No mestre, medir e logar latência (ms) por fragmento confirmado
      (`t_ack_recebido - t_envio`, evento `latency` em
      `tlm_printf("mestre","latency","ch=..;frag=..;ms=..")`).
- [x] 1.4 No mestre, agregar throughput (fragmentos confirmados/s) por
      janela deslizante (`THROUGHPUT_WINDOW_HOPS=50`, mesmo padrão de
      `plr-metric`) e logar (evento `throughput`).
      **Confirmado 2026-09-24**: `pio run` limpo em todos os 10 envs de
      firmware, `pio test -e native_test` 16/16. Validação em bancada
      (confirmar os números batendo com o esperado) fica pra seção 4,
      junto da bateria completa — precisa de placa_b conectada.

## 2. Ferramenta Python — captura multi-porta

- [x] 2.1/2.2 **Já satisfeito por infraestrutura existente, não por
      `tools/spectrum/main.py` como o design original previa.**
      `tools/logger/main.py` (de `telemetry-logging`, implementado depois
      deste design) já lê as portas de placa_a E placa_b simultaneamente
      (uma thread por porta) e mescla tudo por ordem de recepção real num
      único `logs/session-<ts>.jsonl` — nenhum parser novo nem
      `SerialReaderThread` adicional foi necessário. Os eventos `latency`/
      `throughput` da seção 1 já chegam nesse mesmo JSONL, parseados
      genericamente (`k=v` -> dict) sem exigir mudança no logger. Ver
      design.md, nota no topo do "Context".

## 3. Orquestração dos cenários

- [ ] 3.1 Implementar a sequência dos 3 cenários (A/B/C) com duração
      configurável cada, decidindo GUI (`ScenarioTab`) vs. script CLI
      dedicado conforme praticidade de uso em bancada.
- [ ] 3.2 Para cada cenário, emitir os comandos necessários: modo
      hopping/fixed no mestre/escravo (via porta serial correspondente,
      requer também comando serial equivalente já suportado por
      mestre/escravo), modo/estado do jammer (via `mode` +
      `jammer on|off`, de `targeted-jammer`).
- [ ] 3.3 Rotular cada linha gravada com o nome do cenário ativo no
      momento e salvar um único CSV consolidado por execução completa.

## 4. Relatórios e validação final

- [ ] 4.1 Implementar os gráficos de throughput × tempo e latência ×
      tempo, com os 3 cenários distinguíveis, reaproveitando a
      infraestrutura de plot de `ViewTab`.
- [ ] 4.2 Rodar a bateria completa uma vez de ponta a ponta e revisar
      manualmente se os números batem com o esperado pelo guia (Cenário
      A: erro ~100%; B: erro médio/alto; C: erro baixo/recuperação
      rápida) antes de considerar os dados prontos para a monografia.
- [ ] 4.3 Exportar os gráficos finais para uso na monografia (reaproveitar
      `_save_image` já existente em `ViewTab`).
