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

### Diagnóstico feito em 2026-09-17: `env:validacao` isola o problema na Placa A

`validacao/src/main.cpp` (jammer VSPI + analisador HSPI no mesmo ESP32,
já existente antes desta sessão, usado por `tools/spectrum/`) é o teste de
antena mais simples possível — não depende de nenhuma lógica de
pareamento, só de "o jammer de um rádio é visto pelo analisador do
outro". Rodado em cada placa, comandando via
`echo "<placa_a|placa_b> jammer on" >> logs/cmd.in` com `tools/logger/main.py`
de pé:

- **Placa B: passa.** Antes do `jammer on`, o analisador já lia ruído
  ambiente típico de WiFi/BT (valores esparsos 10–100). Ao ligar o
  jammer (mesma placa, alcance de poucos centímetros), a atividade sobe
  visivelmente e de forma sustentada em quase todos os canais — o par
  VSPI+HSPI desta placa funciona.
- **Placa A: falha.** O analisador (HSPI) lê **zero em todos os 126
  canais**, em todo cenário testado: jammer da própria Placa A ligado a
  centímetros de distância, jammer da Placa B (já confirmado funcional)
  ligado, e mesmo ruído ambiente de WiFi — que a Placa B claramente
  capta e a Placa A nunca captou nenhuma vez. Isso não é uma questão de
  calibração de limiar (`LIMIAR_ALVO` etc.) — é zero absoluto em toda
  amostra.
- **Placa A, rádio VSPI (jammer/mestre) — inconclusivo.** Ligar o jammer
  da Placa A e observar pela Placa B (o receptor comprovadamente
  funcional) não mostrou nenhum salto correlacionado nas médias de
  atividade — mas isso pode ser falta de alcance entre as duas placas
  (não testado a poucos centímetros como o caso da Placa B) tanto quanto
  um problema real neste módulo.

**Conclusão**: o bloqueio de RF não está espalhado pelos 4 módulos —
está concentrado na Placa A, com o rádio HSPI (posição do analisador)
comprovadamente não recebendo nada em nenhuma condição. Como os dois
módulos de uma mesma placa compartilham fonte de alimentação, um problema
de alimentação (ver `docs/pinout.md`) na Placa A explicaria os dois
sintomas ao mesmo tempo (HSPI morto + VSPI sem alcance).

### Checklist de inspeção física sugerido (foco na Placa A)

1. Antena do módulo HSPI (posição do analisador) da Placa A — confirmar
   que está fisicamente conectada e não invertida; se for módulo com fio
   solto em vez de PCB antenna, confirmar que o fio não se soltou.
2. Alimentação 3.3V dos dois módulos da Placa A especificamente —
   `docs/pinout.md` já alertava que o regulador onboard do ESP32 pode não
   dar conta de dois módulos PA+LNA simultâneos; testar com fonte 3.3V
   externa regulada só para os módulos (não o pino 3V3 do ESP32) antes de
   qualquer outra hipótese.
3. Reexecutar `env:validacao` na Placa A depois do fix acima — se o
   analisador voltar a ler ruído ambiente não-zero, o hardware está bom;
   repetir o teste `jammer on` da Placa A observado pela Placa B para
   confirmar o alcance do rádio VSPI também.
4. Se a alimentação não for o problema, trocar o módulo HSPI da Placa A
   por um dos módulos sobressalentes (se houver) para descartar módulo
   com defeito de fábrica — comum em NRF24L01+ clone baratos.
5. Reduzir a distância entre as duas placas para poucos centímetros como
   teste adicional de alcance do rádio VSPI da Placa A, já que esse
   resultado ficou inconclusivo.

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
