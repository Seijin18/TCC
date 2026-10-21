## Context

`jammer/src/main.cpp` hoje não lê a serial em `loop()` — é um sweep
autônomo iniciado no `setup()` (CW contínuo, canal avançado em
`hopCarrier`/`startCarrierOn`, `jammer/src/main.cpp:104-139`). Já suporta 1
ou 2 rádios (VSPI/HSPI), dividindo a faixa ao meio quando há 2. O harness
`validacao/src/main.cpp` já tem um parser de comando serial simples
(`handleCommand`, com comandos `jammer on|off` e `status`) e já sorteia
canal dentro da banda BT a cada passo (`randomBtChannel`, `stepJammer`).
Nenhum dos dois hoje conhece `shared/hopping.h`.

## Goals / Non-Goals

**Goals:**
- Modo `targeted`: percorre round-robin (ou sorteia, no caso do
  `validacao/`) apenas os 16 canais de `HOPPING_CHANNELS`.
- Alternância de modo em runtime por comando serial, para permitir uma
  sessão de coleta rodar sweep e depois targeted sem novo flash.
- Reaproveitar ao máximo a lógica de dwell/dual-rádio já validada no
  sweep — só trocar a fonte da lista de canais.

**Non-Goals (revisto em 2026-09-24 -- ver "Implementado depois" abaixo):**
- ~~Não implementa perseguição reativa do canal atual do mestre~~ --
  implementado depois como modo `adaptive`, mas por SENSORIAMENTO de
  energia (RPD), não perseguição via seed/sincronização (que continua fora
  de escopo -- exigiria o jammer conhecer a seed do mestre, quebrando o
  modelo de "atacante sem informação privilegiada do protocolo").
- Não implementa os perfis de potência/health-check/stats que estavam
  descritos nos documentos removidos no commit `8188180` — não estavam
  implementados antes e não são exigência do guia; podem virar um change
  futuro separado se a banca pedir.

## Decisions

- **`jammer/src/main.cpp` ganha um parser de serial mínimo** no `loop()`
  (não bloqueante, leitura por linha como já existe em `validacao/`) só
  para o comando `mode sweep|targeted` — não é necessário replicar todos
  os comandos de `validacao/`, só o de modo.
- **Modo default = `sweep`** (comportamento atual preservado) para não
  quebrar testes/scripts existentes que já dependem do sweep como padrão
  ao ligar o jammer.
- **Lista de canais do modo targeted = `HOPPING_CHANNELS` direto**, sem
  cópia local — inclui `shared/hopping.h` e itera `TABLE_SIZE` (16)
  elementos em vez de `BT_CH_MIN..BT_CH_MAX` (79).
- **Dual-rádio no modo targeted**: divide os 16 canais ao meio (8 e 8) em
  vez de dividir a faixa BT ao meio — mesma estratégia de paralelismo, só
  aplicada à lista menor.
- **`validacao/src/main.cpp`**: adicionar `randomTargetedChannel()` que
  sorteia dentro de `HOPPING_CHANNELS` e um novo comando serial `mode
  sweep|targeted` que troca qual função `stepJammer`/`jammerStart` chamam
  para escolher canal.

## Risks / Trade-offs

- Jamming concentrado em só 16 canais é mais fácil de "escapar" caso o
  mestre implemente blacklist (`dynamic-seed-blacklist`) — isso é
  esperado e desejado, é justamente o cenário que a Fase 4/5 quer
  demonstrar (mitigação funcionando contra um ataque dirigido).
- Adicionar leitura de serial ao `jammer/src/main.cpp` introduz uma
  pequena chance de jitter no dwell se a leitura bloquear — usar leitura
  não bloqueante (`Serial.available()` checado uma vez por iteração do
  `loop()`, sem `while` bloqueante) para não degradar o sweep/targeted em
  si.

## Implementado depois (2026-09-24): modo `adaptive`

**Motivação**: validando `dynamic-seed-blacklist`/`blacklist-decay` em
bancada, ficou claro que `sweep` (banda inteira) pode ser forte demais
para testar o sistema adaptativo de forma útil — se o sweep interrompe
comunicação em TODOS os canais ao mesmo tempo (inclusive os de
pareamento), não sobra pra onde escapar, e o teste não diferencia "a
mitigação não funciona" de "não existe canal livre para fugir". `targeted`
(round-robin fixo pelos 16 `HOPPING_CHANNELS`) é mais realista, mas ainda
reparte tempo igual entre canais ociosos e o canal que o enlace
realmente está usando no momento — um adversário rádio-único mais
eficiente concentraria energia onde o alvo de fato está.

**O que muda**: novo `JAM_MODE_ADAPTIVE` (`mode adaptive`) — em vez de
round-robin cego, sensoria por energia (RPD, mesma técnica de
`measureChannel()` no analisador e `measure_and_pick_candidate()` no
mestre) cada um dos 16 `HOPPING_CHANNELS` procurando atividade real; ao
encontrar, trava nesse canal e concentra a emissão ali, revalidando a
cada `ADAPTIVE_RECHECK_MS` (solta e volta a escanear se o canal ficar
quieto — o alvo saltou pra outro canal, ou nunca era o certo).

**Por que sensoriamento e não perseguição via seed**: perseguir o canal
exato via `get_next_active_channel(seed, hop_counter, mask)` exigiria o
jammer conhecer a seed do mestre — quebra o modelo de "atacante externo
sem informação privilegiada do protocolo" que o resto deste change já
assume (nenhum outro modo depende de conhecer seed/estado interno).
Sensoriamento por energia é comportamento de atacante realista (um
jammer reativo por detecção de energia é uma classe de ataque padrão na
literatura de segurança de RF) e não exige essa suposição.

**Trade-off técnico**: `startConstCarrier()` (portadora de teste
dedicada do NRF24) não permite recepção simultânea — sensoriar exige
sair do modo de portadora, entrar em RX, medir, e voltar
(`senseChannel()`), uma interrupção breve e deliberada na emissão.
Intervalo de revalidação (`ADAPTIVE_RECHECK_MS=150`) escolhido gerando
poucas interrupções por segundo, não uma por dwell curto como
`JAM_TARGETED_HOLD_MS` faz no modo `targeted`.

**Único rádio** (`jammer_radio`), mesmo em `has_radio2`: suficiente para
o cenário real de teste (bancada de 2 placas, um rádio só em modo
combinado); sensoriar+emitir com 2 rádios simultâneos dobraria a
complexidade sem necessidade aqui.

**Ainda não validado em bancada em 2026-09-24**: implementado e compila
limpo (`pio run -e jammer -e placa_b`), mas a placa combinada só inicia a
portadora depois que `jammer_set_ready_source()` confirma que
mestre/escravo já concluíram o handshake inicial (`g_jam_enabled=false`
por padrão em modo combinado) — a sessão de validação encontrou
dificuldade recorrente de pareamento (ver `adaptive-rendezvous`
design.md/tasks.md, achado do mesmo dia) que impediu o modo `adaptive`
de chegar a emitir de fato. Confirmado até agora: comando `mode adaptive`
reconhecido e telemetria (`jam_mode_adaptive`) disparando corretamente.
Falta confirmar em bancada real: `adaptive_lock`/`adaptive_release`
disparando nos canais certos, e que o enlace consegue escapar dele via
`dynamic-seed-blacklist` como escapa do `targeted`.
