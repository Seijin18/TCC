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

**Non-Goals:**
- Não implementa perseguição reativa do canal atual do mestre (precisaria
  de rádio adicional em modo escuta + sincronização com a seed —
  fora do escopo do guia para a Fase 2, que descreve apenas "atacar os
  canais da tabela conhecida").
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
