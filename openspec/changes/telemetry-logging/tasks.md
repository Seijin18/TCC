## 1. `shared/telemetry.h`

- [x] 1.1 Definir `tlm_printf(const char *src, const char *event, const char *fmt, ...)`
      que monta a linha `@<src>;t=<millis>;e=<event>;<fmt...>` e escreve
      via `Serial` sob lock.
- [x] 1.2 Lock: mutex FreeRTOS real quando `ARDUINO_ARCH_ESP32` e mais de
      uma task pode chamar (placas combinadas); no-op em builds standalone
      de um nó por placa.
- [x] 1.3 Prefixo `@csv;` reservado para a saída de `sendCSV()` do
      analisador (não usa `tlm_printf`, mas passa pelo mesmo lock).

## 2. Instrumentar os nós (aditivo, sem remover log humano)

- [x] 2.1 `mestre/src/node.h`: `boot`, `pair_scan`, `pair_ok`, `seed`, `hop`,
      `ack`, `miss`, `msg_done`, `alert` (transição do alerta recebido).
- [x] 2.2 `escravo/src/node.h`: `boot`, `pair_scan`, `pair_ok`, `seed`,
      `plr` (mesmo dado do `log_plr_window()` existente, formato `@`).
- [x] 2.3 `jammer/src/node.h`: `boot`, `jam_mode`, `jam_state`.
- [x] 2.4 `analisador/src/node.h`: `boot`, `alert_on`/`alert_off`. Campo
      `chmap` (máscara por canal) descartado -- `dynamic-seed-blacklist`
      acabou não precisando dele: o mestre já deriva a blacklist da própria
      evidência de ACK por canal (`g_channel_fail`), o analisador só
      precisa do booleano de alerta para disparar o reseed.

## 3. `tools/logger/main.py`

- [x] 3.1 Abrir as duas portas por caminho `/dev/serial/by-id/...`
      (`dtr=False, rts=False`), uma thread de leitura por porta.
- [x] 3.2 Parsear `@...;` em campos; linhas sem `@` viram evento `raw`;
      linhas CSV (`@csv;` ou heurística de vírgulas) viram
      `logs/spectrum.csv`.
- [x] 3.3 Gravar `logs/session-<timestamp>.jsonl` com `flush()` por linha;
      manter `logs/latest.jsonl` (symlink) apontando pra sessão corrente.
- [x] 3.4 Derivar e reescrever `logs/state.json` (~1 Hz): link up/down,
      canal de pareamento, seed, máscara, PLR janela/sessão, alerta,
      modo/potência do jammer, timestamp do último evento por placa.
- [x] 3.5 Escrever `logs/events.log` (texto humano, um evento relevante por
      linha, sem os `hop` de rotina).
- [x] 3.6 Ler `logs/cmd.in` (offset persistido) e reenviar cada linha nova
      para a porta indicada.
- [x] 3.7 Reconexão automática por porta se o dispositivo cair/resetar.
- [x] 3.8 Rotação simples do JSONL por tamanho de arquivo.
- [x] 3.9 `logs/` no `.gitignore` (manter `logs/.gitkeep`).

## 4. `tools/dashboard/main.py` — migrar para leitura de arquivo

- [x] 4.1 Substituir `SerialReaderThread` por um tail de `logs/latest.jsonl`
      (posição persistida entre re-renders, não re-lendo o arquivo inteiro
      a cada tick).
- [x] 4.2 `classify_line`/regex existentes viram fallback para eventos
      `raw`; eventos estruturados alimentam os mesmos campos de estado
      (`RE_HOP`, `RE_PLR`, etc. mapeados para `hop`, `plr`, ...).
- [x] 4.3 Botão "Enviar" escreve em `logs/cmd.in` em vez de
      `send_line()` direto na porta.
- [ ] 4.4 Confirmar visualmente que o painel (spectrum, enlace, jammer, PLR,
      log) continua populando igual ao comportamento anterior, com o logger
      rodando em paralelo -- requer display (Tkinter), não verificável no
      ambiente onde o código foi escrito.

## 5. Validação manual

- [x] 5.1 Rodar `tools/logger/main.py` sozinho (sem dashboard) com uma
      placa ligada, confirmar `logs/session-*.jsonl` crescendo e
      `logs/state.json` atualizando.
      Usado assim (headless, as duas placas) em toda a validação de
      2026-09-24/25: dezenas de sessões, ~950 MB de JSONL, `state.json` e
      `events.log` lidos o tempo todo durante os testes.
- [ ] 5.2 Abrir e fechar o dashboard várias vezes durante uma sessão do
      logger, confirmar que o histórico em `logs/events.log` não se perde.
- [x] 5.3 Confirmar, numa placa combinada (two-board-topology), que os
      eventos das duas tasks (ex. mestre + analisador na Placa A) saem sem
      entrelaçamento de linha no JSONL.
      **Resultado (sessões 2026-09-24 20:10 → 2026-09-25 01:0x, ~1,6 M
      eventos)**: 0 linhas com marcador `@<src>;` no meio da linha e 0
      linhas começando com `@` que o logger não conseguiu parsear. Única
      exceção encontrada (e corrigida no logger em
      `pairing-handshake-hardening` 5.6): o heartbeat `'.'` do jammer, sem
      newline, grudava no início da próxima linha do escravo na placa B.
