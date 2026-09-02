## 1. Firmware — modo canal fixo e métricas de latência/throughput

- [ ] 1.1 Adicionar comando serial de modo (`mode hopping|fixed`) a
      `mestre/src/main.cpp` e `escravo/src/main.cpp`, análogo ao já usado
      em `targeted-jammer`; default `hopping` (comportamento atual
      preservado).
- [ ] 1.2 Em modo `fixed`, usar um canal constante (`PAIRING_CHANNEL` ou
      configurável) em vez de `get_next_channel`/`get_next_active_channel`
      em ambos os nós.
- [ ] 1.3 No mestre, medir e logar latência (ms) por fragmento confirmado
      (`t_ack_recebido - t_envio`).
- [ ] 1.4 No mestre, agregar throughput (fragmentos confirmados/s) por
      janela deslizante e logar, reaproveitando o padrão de janela de
      `plr-metric`.

## 2. Ferramenta Python — captura multi-porta

- [ ] 2.1 Adicionar uma segunda instância de leitura serial (porta do
      escravo) reaproveitando `SerialReaderThread`, com um parser de linha
      para o formato de PLR/latência/throughput definido em `plr-metric` e
      na seção 1 deste change.
- [ ] 2.2 Mesclar as duas fontes por timestamp relativo de recepção no
      host, tratando o caso de uma das portas não estar conectada com erro
      explícito (não gravação parcial silenciosa).

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
