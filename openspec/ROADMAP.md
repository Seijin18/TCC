# Roadmap de implementação — TCC Anti-Jamming

Consolida o estado dos changes ativos (`openspec/changes/`) num plano de
execução único. Não substitui `project.md` (que continua sendo a fonte de
verdade de contexto/dependências) — é a camada de "o que fazer agora, em que
ordem" derivada dele. Atualizar esta lista conforme tasks forem fechadas ou
changes forem arquivados.

## Estado em 2026-09-17: todo o código pronto, bloqueado em hardware

Todo o código planejado para os cinco changes de fase mais
`telemetry-logging`/`adaptive-rendezvous`/`two-board-topology` §7 está
implementado, com `pio run` (8 environments) e `pio test -e native_test`
(10/10) limpos, e as duas placas flasheadas com o firmware atual. **Nenhum
change tem trabalho de código pendente** — tudo que resta em todo change
listado abaixo é validação manual em bancada.

- **`telemetry-logging`** — seções 1–4 fechadas (código); só falta 4.4
  (confirmar visualmente o dashboard Tkinter, precisa de display) e a
  seção 5 (validação em sessão real).
- **`adaptive-rendezvous`** — seções 1–5 fechadas (código); seção 6
  (validação manual) bloqueada — ver abaixo.
- **`two-board-topology`** — seções 1–5 e 7 fechadas; seção 6 (validação
  manual) bloqueada — ver abaixo.
- **`plr-metric`**, **`targeted-jammer`**, **`spectrum-attack-classification`**
  — código já fechado antes desta sessão; suas seções de validação/
  calibração em bancada também bloqueadas pelo mesmo motivo.
- **`dynamic-seed-blacklist`** — seções 1–3 (código) fechadas nesta sessão,
  consumindo `shared/protocol.h` de `adaptive-rendezvous` como previsto;
  seção 4 (validação) bloqueada.
- **`scenario-automation`** — único change ainda sem código, porque depende
  de todos os anteriores estarem validados em bancada primeiro.

## Bloqueio atual: comunicação RF não fecha nesta bancada

Ao tentar validar o handshake mestre↔escravo pela primeira vez, dois bugs
reais de firmware foram encontrados e corrigidos (detalhe completo em
`openspec/changes/adaptive-rendezvous/design.md`, seção "Achado de
bancada"):

1. `RF24::write()` (lib RF24 1.6.2) tem um bug que trava para sempre quando
   TX_DS/TX_DF nunca setam — disparava toda vez que o mestre sondava um
   canal onde o escravo não estava naquele instante exato (agora comum,
   com o escravo varrendo em vez de escutar um canal fixo). Corrigido com
   `radio_send()` (`writeFast()+txStandBy()`, `shared/protocol.h`),
   aplicado em **todos** os `write()` do projeto — sem esse fix, qualquer
   perda de pacote real sob jamming (o próprio objetivo deste TCC)
   travaria um nó permanentemente no meio de uma sessão.
2. `PAIR_DWELL_MS` (250ms) menor que `LINK_RETRY_MS` (300ms) podia nunca
   convergir por pigeonhole, dependendo só da fase relativa fixa entre os
   dois relógios livres — corrigido subindo o dwell pra 400ms.

**Mesmo com os dois fixes, a sonda do mestre não chega ao escravo.**
Testado exaustivamente: mestre e escravo fixos no mesmo canal 76 o tempo
todo (sem varredura, sem retry, o cenário mais simples possível) e também
com o firmware standalone original (anterior a todo trabalho desta sessão,
canal fixo, sem nenhuma das mudanças de `adaptive-rendezvous`) — mesmo
resultado nos dois casos, na mesma bancada. O analisador (rádio HSPI,
Placa A) também não detecta nenhuma atividade em nenhum dos 126 canais
mesmo com o jammer da Placa B em `PA_MAX` ativo. Isso aponta para fora do
software: fiação, antena, alcance físico ou módulo com defeito — **nenhum
change abaixo pode fechar sua validação manual até isso ser resolvido**.

### Checklist de inspeção física sugerido (nesta ordem)

1. Antenas dos 4 módulos NRF24 firmes no conector e não invertidas.
2. Alimentação 3.3V dos módulos — `docs/pinout.md` já alertava que o
   regulador onboard do ESP32 pode não dar conta de módulos PA+LNA; um
   brownout durante a transmissão é indistinguível de "não transmitiu"
   nos registradores (`STATUS`/`FIFO_STATUS` continuam parecendo normais).
   Testar com fonte 3.3V externa regulada para os módulos, não o pino 3V3
   do ESP32.
3. Reduzir a distância entre as duas placas para poucos centímetros como
   teste de sanidade mínimo (elimina alcance/obstrução como variável).
4. Se nada disso resolver, testar cada módulo NRF24 isoladamente com um
   sketch de exemplo mínimo (fora deste projeto, ex. os exemplos da
   própria lib RF24) para descartar módulo com defeito — pelo menos um
   dos quatro módulos usados neste teste (mestre/escravo/jammer/
   analisador) pode ter vindo com defeito de fábrica, comum em módulos
   NRF24L01+ clone baratos.

## Ordem de execução após o hardware ser resolvido

```
plr-metric §3 ──┐
targeted-jammer §3 ──┤
spectrum-attack-classification §3 ──┤─→ dynamic-seed-blacklist §4 ─→ scenario-automation
two-board-topology §6 ──┤
adaptive-rendezvous §6 ──┘
```

Todas as seções de validação à esquerda podem rodar em paralelo assim que
o handshake básico funcionar — nenhuma depende de código adicional, só de
bancada operante. `dynamic-seed-blacklist` §4 precisa de PLR (plr-metric)
e alerta (spectrum-attack-classification) confirmados funcionando primeiro.
`scenario-automation` fecha o projeto por último.

## Próxima ação recomendada

Resolver o checklist de inspeção física acima. Assim que uma sonda
qualquer do mestre chegar ao escravo (confirmável por `pair_ok` em
`logs/events.log`), toda a bateria de testes graduais T1–T7 do plano de
bancada pode rodar em sequência sem mais trabalho de código esperado.
