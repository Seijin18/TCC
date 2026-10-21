## Why

Hoje nada que os quatro nós emitem pela serial é persistido. `tools/dashboard/main.py`
guarda o log num `deque(maxlen=500)` em memória (`tools/dashboard/main.py:510`) —
fechar a GUI ou passar de 500 linhas apaga o histórico, e sem a GUI aberta não
existe captura alguma. Isso bloqueia qualquer validação de bancada que dependa
de reconstruir depois "o que aconteceu": os testes graduais do rendezvous
adaptativo (`adaptive-rendezvous`) e da troca dinâmica de seed/blacklist
(`dynamic-seed-blacklist`) precisam de evidência auditável (qual canal, qual
seed, em que instante) para confirmar que a esquiva funcionou, e a validação
pendente de `plr-metric` (seção 3) e `spectrum-attack-classification` (seção 3)
também depende de sessões longas que ninguém consegue acompanhar olhando uma
janela Tkinter o tempo todo.

Além disso, dois nós às vezes escrevem na mesma UART (nas placas combinadas de
`two-board-topology`, mestre+analisador ou escravo+jammer rodam em tasks
FreeRTOS de núcleos diferentes compartilhando o `Serial` global) — hoje isso
já causa entrelaçamento de linha visível (o dashboard tem um workaround
ad-hoc para o `.` do jammer colar na linha seguinte, `tools/dashboard/main.py:165`),
e vai piorar conforme mais eventos estruturados forem adicionados pelos changes
futuros.

## What Changes

- Formato de telemetria canônico e serializado por linha (`shared/telemetry.h`),
  emitido por todos os quatro nós lado a lado com os `Serial.printf` humanos
  já existentes (não os substitui) — resolve o entrelaçamento entre tasks na
  mesma UART com um lock, e dá um formato único (`@<src>;t=..;e=..;chave=valor;...`)
  para eventos que os changes seguintes (rendezvous, rekey, blacklist) vão
  precisar logar de forma parseável.
- Um daemon Python headless novo (`tools/logger/main.py`), dono exclusivo das
  duas portas seriais da bancada, que grava cada sessão em
  `logs/session-<timestamp>.jsonl` (append-only, `flush()` por linha),
  mantém `logs/latest.jsonl` apontando para a sessão corrente, deriva
  `logs/state.json` (snapshot do estado do sistema) e `logs/events.log`
  (visão humana só dos eventos relevantes), e aceita comandos via
  `logs/cmd.in` para reenviar às placas.
- `tools/dashboard/main.py` deixa de abrir a porta serial diretamente — passa
  a seguir (`tail -f`) `logs/latest.jsonl`, podendo abrir/fechar sem perder
  histórico e sem disputar a porta com o logger. O campo de envio de texto
  passa a escrever em `logs/cmd.in`.
- **BREAKING** (só para quem usava o dashboard sozinho, sem o logger): a
  captura deixa de existir enquanto `tools/logger/main.py` não estiver
  rodando — o dashboard não abre mais a porta serial por conta própria.

## Capabilities

### New Capabilities
- `structured-telemetry`: formato de linha de evento serializado, emitido por
  todos os nós, mais o daemon headless que persiste, deriva estado e
  encaminha comandos.

### Modified Capabilities
(nenhuma — não há capability publicada para o dashboard hoje)

## Impact

- Novo: `shared/telemetry.h`, `tools/logger/main.py`, diretório `logs/`
  (`.gitignore`).
- `mestre/src/node.h`, `escravo/src/node.h`, `jammer/src/node.h`,
  `analisador/src/node.h`: cada um ganha chamadas a `tlm_printf()` nos pontos
  de evento relevantes, sem remover nenhum `Serial.printf` humano existente.
- `tools/dashboard/main.py`: `SerialReaderThread` (`:329`) substituído por um
  leitor de arquivo; `classify_line` (`:159`) passa a consumir campos
  estruturados quando disponíveis, mantendo o parsing por regex como
  fallback para linhas `raw`.
- Nenhuma mudança de hardware.
