## 1. `shared/telemetry.h`

- [ ] 1.1 Definir `tlm_printf(const char *src, const char *event, const char *fmt, ...)`
      que monta a linha `@<src>;t=<millis>;e=<event>;<fmt...>` e escreve
      via `Serial` sob lock.
- [ ] 1.2 Lock: mutex FreeRTOS real quando `ARDUINO_ARCH_ESP32` e mais de
      uma task pode chamar (placas combinadas); no-op em builds standalone
      de um nó por placa.
- [ ] 1.3 Prefixo `@csv;` reservado para a saída de `sendCSV()` do
      analisador (não usa `tlm_printf`, mas passa pelo mesmo lock).

## 2. Instrumentar os nós (aditivo, sem remover log humano)

- [ ] 2.1 `mestre/src/node.h`: `boot`, `pair_scan`, `pair_ok`, `seed`, `hop`,
      `ack`, `miss`, `msg_done`, `alert` (transição do alerta recebido).
- [ ] 2.2 `escravo/src/node.h`: `boot`, `pair_scan`, `pair_ok`, `seed`,
      `plr` (mesmo dado do `log_plr_window()` existente, formato `@`).
- [ ] 2.3 `jammer/src/node.h`: `boot`, `jam_mode`, `jam_state`.
- [ ] 2.4 `analisador/src/node.h`: `boot`, `alert`, `chmap` (preparar o
      campo, mesmo antes de `spectrum-attack-classification` popular a
      máscara de verdade).

## 3. `tools/logger/main.py`

- [ ] 3.1 Abrir as duas portas por caminho `/dev/serial/by-id/...`
      (`dtr=False, rts=False`), uma thread de leitura por porta.
- [ ] 3.2 Parsear `@...;` em campos; linhas sem `@` viram evento `raw`;
      linhas CSV (`@csv;` ou heurística de vírgulas) viram
      `logs/spectrum.csv`.
- [ ] 3.3 Gravar `logs/session-<timestamp>.jsonl` com `flush()` por linha;
      manter `logs/latest.jsonl` (symlink) apontando pra sessão corrente.
- [ ] 3.4 Derivar e reescrever `logs/state.json` (~1 Hz): link up/down,
      canal de pareamento, seed, máscara, PLR janela/sessão, alerta,
      modo/potência do jammer, timestamp do último evento por placa.
- [ ] 3.5 Escrever `logs/events.log` (texto humano, um evento relevante por
      linha, sem os `hop` de rotina).
- [ ] 3.6 Ler `logs/cmd.in` (offset persistido) e reenviar cada linha nova
      para a porta indicada.
- [ ] 3.7 Reconexão automática por porta se o dispositivo cair/resetar.
- [ ] 3.8 Rotação simples do JSONL por tamanho de arquivo.
- [ ] 3.9 `logs/` no `.gitignore` (manter `logs/.gitkeep`).

## 4. `tools/dashboard/main.py` — migrar para leitura de arquivo

- [ ] 4.1 Substituir `SerialReaderThread` por um tail de `logs/latest.jsonl`
      (posição persistida entre re-renders, não re-lendo o arquivo inteiro
      a cada tick).
- [ ] 4.2 `classify_line`/regex existentes viram fallback para eventos
      `raw`; eventos estruturados alimentam os mesmos campos de estado
      (`RE_HOP`, `RE_PLR`, etc. mapeados para `hop`, `plr`, ...).
- [ ] 4.3 Botão "Enviar" escreve em `logs/cmd.in` em vez de
      `send_line()` direto na porta.
- [ ] 4.4 Confirmar visualmente que o painel (spectrum, enlace, jammer, PLR,
      log) continua populando igual ao comportamento anterior, com o logger
      rodando em paralelo.

## 5. Validação manual

- [ ] 5.1 Rodar `tools/logger/main.py` sozinho (sem dashboard) com uma
      placa ligada, confirmar `logs/session-*.jsonl` crescendo e
      `logs/state.json` atualizando.
- [ ] 5.2 Abrir e fechar o dashboard várias vezes durante uma sessão do
      logger, confirmar que o histórico em `logs/events.log` não se perde.
- [ ] 5.3 Confirmar, numa placa combinada (two-board-topology), que os
      eventos das duas tasks (ex. mestre + analisador na Placa A) saem sem
      entrelaçamento de linha no JSONL.
