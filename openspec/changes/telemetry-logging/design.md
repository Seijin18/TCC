## Context

Quatro nós (`mestre`, `escravo`, `jammer`, `analisador`) já logam texto livre
pela serial via `Serial.printf`/`Serial.println`, consumido hoje só por
`tools/dashboard/main.py` (regex ad-hoc por prefixo, `:132-141`) e
`tools/spectrum/main.py` (CSV de espectro). Nada é persistido em disco. Nas
placas combinadas de `two-board-topology`, dois nós compartilham a mesma
`Serial` a partir de tasks FreeRTOS de núcleos diferentes.

## Goals / Non-Goals

**Goals:**
- Formato de evento único, fácil de parsear, emitido por todos os nós.
- Persistência contínua e sobrevivente a reinícios de sessão (arquivo em
  disco, não memória de processo).
- Funcionar sem GUI (headless), para soak tests e coleta de cenários.
- Não quebrar o parsing humano existente (dashboard, olho humano no monitor
  serial do PlatformIO) durante a transição.
- Serializar escritas de UART compartilhada entre duas tasks (placas
  combinadas) para não entrelaçar linhas.

**Non-Goals:**
- Não implementa nenhuma lógica de negócio nova (rendezvous, rekey,
  blacklist) — só a infraestrutura de log que os outros changes vão usar.
- Não versiona `logs/` no git (dados de bancada, não código).
- Não muda o formato do CSV de espectro consumido por `tools/spectrum/` —
  esse fluxo continua igual, só ganha um destino em arquivo via o logger.

## Decisions

### Formato de linha: `@<src>;t=<millis>;e=<evento>;k=v;...`
Prefixo `@` para diferenciar de log humano sem prefixo (permite os dois
coexistirem na mesma UART sem ambiguidade). `src` identifica o nó
(`mestre`, `escravo`, `jammer`, `analisador`) mesmo quando duas fontes
compartilham a mesma porta física (placas combinadas) — o logger não precisa
adivinhar por conteúdo como o dashboard faz hoje.

### `tlm_printf()` como ponto único de escrita, com lock
Implementado em `shared/telemetry.h`, usado por todos os nós. Em builds
combinadas (duas tasks no mesmo processo), a escrita é protegida por um
mutex FreeRTOS (`portMUX_TYPE` ou `SemaphoreHandle_t`) para a linha inteira
sair atômica — sem isso, a mesma classe de entrelaçamento que hoje afeta o
`.` do jammer (`tools/dashboard/main.py:165`) afetaria qualquer evento novo.
Em builds standalone (`native_test`, um nó por placa) o lock é um no-op.

### Logger como processo Python separado, dono exclusivo das portas
Alternativa considerada: manter o dashboard como único consumidor e só
adicionar `open()`/`csv.writer` nele. Rejeitada porque (a) não funciona
headless — sessões de soak/coleta de cenário não têm por que depender de
Tkinter/X, e (b) a GUI trava/loga como efeito colateral de estar aberta,
inviável de rodar em background numa sessão longa (`scenario-automation`).
Um daemon dedicado desacopla "captura" de "visualização".

### JSONL append-only + `state.json` derivado + `events.log` humano
Três saídas com propósitos diferentes: `session-*.jsonl` é a fonte bruta e
completa (uma linha por linha serial recebida, inclusive não reconhecidas
como `@...`, marcadas `"e":"raw"` — nada é descartado); `state.json` é
reescrito periodicamente com o estado atual derivado (link, canal, seed,
máscara, PLR, alerta) para consulta rápida sem reprocessar o JSONL inteiro;
`events.log` é a versão legível (sem o ruído de `hop` por hop) usada para
acompanhamento manual via `tail -f`.

### Comandos via arquivo (`logs/cmd.in`), não socket/porta extra
Mantém a interface simples (append de uma linha) e dá à GUI, a mim (Claude)
e a qualquer script futuro o mesmo canal para comandar a bancada, sem abrir
uma segunda forma de acesso à porta serial (que continua exclusiva do
logger).

## Risks / Trade-offs

- **Overhead de `tlm_printf()` no loop de hop (220 ms/hop)**: mitigado
  mantendo o formato compacto (chaves curtas) e reservando eventos de alta
  frequência (`hop`) para quando realmente necessário na validação —
  eventos como `pair_scan` podem ser mais verbosos sem impacto de timing.
- **Lock em builds combinadas** pode introduzir uma pequena janela de espera
  entre as duas tasks; como cada evento é emitido só nos pontos de transição
  (não em polling apertado), o impacto esperado é desprezível frente ao
  `HOP_INTERVAL_MS` de 220 ms.
- **`logs/` crescendo sem limite** num soak longo: o logger aplica rotação
  simples por tamanho de arquivo.

## Migration Plan

1. `shared/telemetry.h` implementado e incluído por todos os `node.h`
   (aditivo, `Serial.printf` humanos preservados).
2. `tools/logger/main.py` implementado e testado com pelo menos uma placa.
3. `tools/dashboard/main.py` migrado para ler arquivo; validado que a GUI
   continua mostrando o mesmo painel de antes com o logger rodando.
4. Nenhum passo remove capacidade existente — o dashboard antigo (leitura
   direta de porta) só é desligado depois que o novo caminho for confirmado
   funcional em bancada.
