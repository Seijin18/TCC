# Roadmap de implementação — TCC Anti-Jamming

Consolida o estado dos changes ativos (`openspec/changes/`) num plano de
execução único. Não substitui `project.md` (que continua sendo a fonte de
verdade de contexto/dependências) — é a camada de "o que fazer agora, em que
ordem" derivada dele. Atualizar esta lista conforme tasks forem fechadas ou
changes forem arquivados.

## Achado adicional (2026-09-18): analisador não dispara alerta contra o jammer real desta bancada — piso de ruído alto + bug de snapshot único (parcialmente corrigido)

Construindo `tools/validation/run_scenario.py` (cenário automatizado
baseline→jamming→recovery com plot, ver commit correspondente) e rodando
pela primeira vez jammer+analisador juntos de forma medida, dois problemas
foram encontrados em `spectrum-attack-classification`:

1. **Sweep nunca poderia disparar o alerta, estruturalmente**: cobre só
   2–80 MHz, faixa com apenas 8 dos 16 canais de `HOPPING_CHANNELS` — o
   teto de `alvo_avg` (média sobre os 16) é ~50%, sempre abaixo de
   `LIMIAR_ALVO=70`. Não é bug, é limite de cobertura — usar `jam mode
   targeted` para exercitar o alerta.
2. **Mesmo em targeted, o alerta não disparava**: `classifyAttack()`
   usava um snapshot único por sweep (`channelData[]`, resetado a cada
   ciclo) — um jammer de rádio único visita 1 canal por vez, e seu ciclo
   pelos 16 alvos raramente coincidia no tempo com o scanSpectrum() do
   analisador (~860ms para 126 canais), então um hit isolado num canal
   não sobrevivia até o próximo canal-alvo ser atingido. **Corrigido**:
   `channelHeat[]` acumula calor entre sweeps em vez de resetar
   (`analisador/src/node.h`); jammer targeted ganhou dwell mais longo por
   canal (`JAM_TARGETED_HOLD_MS=60ms`, `jammer/src/node.h`).
3. **Ainda não dispara, por um terceiro motivo — piso de ruído desta
   bancada**: mesmo com o fix acima e jamming real confirmado (PLR do
   enlace subindo de ~0% pra ~12% durante 75s de `jam mode targeted` +
   `power high`), `alert_on` não disparou. O CSV bruto do analisador
   mostra vários canais com 40–60% de atividade **mesmo com o jammer
   desligado** — ruído ambiente (Wi-Fi/BT do ambiente) mantém `fora_avg`
   alto o bastante pra `alvo_avg - fora_avg` nunca cruzar
   `LIMIAR_CONCENTRACAO=30`. Essa é exatamente a tarefa de calibração já
   registrada e nunca executada
   (`openspec/changes/spectrum-attack-classification/tasks.md` seção 3) —
   agora com dados reais medidos pra calibrar contra, em vez dos valores
   default herdados sem calibração.

**Consequência para `dynamic-seed-blacklist`**: como `maybe_enqueue_control()`
(`mestre/src/node.h`) só enfileira rekey de seed/blacklist quando
`g_alert_active`/`g_reseed_needed` estão ligados pelo analisador, a
mitigação inteira fica bloqueada até os limiares serem recalibrados
contra o piso de ruído real. `tools/validation/plot_scenario.py` já está
pronto pra mostrar `rekey_tx`/`rekey_apply`/`blacklist` assim que o
alerta passar a disparar — não precisa de mais nenhum código, só a
recalibração.

## Estado em 2026-09-18: hardware validado, pareamento funciona, hopping ativo resolvido (trocado por evento) — falta soak longo e sob jammer

A causa raiz da falha da fase de hopping (seção abaixo, "hardware
validado... hopping ainda bloqueado") foi encontrada: hopping era
disparado por **relógio periódico** (avançava o canal a cada 220ms
incondicionalmente, sucesso ou falha) desde o commit inicial do projeto
— mantendo mestre e escravo em relógios livres independentes,
sincronizados só por sorte residual de fase. Reescrito para hopping **por
evento com fallback de timeout** (`mestre_loop()`/`escravo_loop()`, ver
`openspec/changes/adaptive-rendezvous/design.md` "Correção 2"): o canal
só avança quando o hop atual é confirmado (ack/fragmento recebido) ou
quando um timeout por tentativa esgota (fallback, evita travar num canal
jamado pra sempre).

**Resultado medido logo após o reflash**: PLR de sessão caiu de
~90–100% pra ~0,6% (jammer desligado, teste curto de ~1-2 min);
`hop_counter` avançou a ~41 hops/s (vs. ~4,5 hops/s do relógio fixo
antigo), confirmando que hops de sucesso agora terminam quase
instantaneamente. Falta: soak de vários minutos (tasks.md 6.4) e
validação com jammer ativo (6.2) — ver "Próximo passo" abaixo.

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

## Atualização (2026-09-17, mesma sessão): hardware validado, bug de software encontrado — pareamento funciona; hopping ainda não

A seção abaixo ("Bloqueio atual") documentava uma conclusão que **se
provou errada**: uma bateria completa e final de `Tests/AntennaCross`,
testando os 4 módulos físicos (após o usuário trocá-los fisicamente
várias vezes entre VSPI/HSPI e Placa A/B ao longo do diagnóstico)
confirmou **todos os 4 módulos funcionando em ambos os sentidos,
~100% de entrega** — não existe módulo defeituoso. O padrão "só VSPI da
Placa A falha como transmissor" era um artefato de qual módulo físico
estava conectado em qual barramento naquele instante da sessão, não uma
propriedade fixa da placa/barramento.

Com o hardware descartado, a causa real era software:
`enableAckPayload()` (presente desde antes desta sessão em
`mestre/src/node.h`/`escravo/src/node.h`, nunca de fato usado —
`writeAckPayload()` nunca é chamado no protocolo) faz estes módulos
clone (base SI24R1) descartarem silenciosamente pacotes NO_ACK
(`radio_send()`/`writeFast(..., multicast=true)`, o único caminho de
envio do projeto). Isolado e confirmado 100% repetível com
`Tests/AntennaCross`; removido dos dois lados. **Resultado: pareamento e
troca de seed funcionam de forma confiável pela primeira vez em toda a
sessão.**

**O que ainda não funciona**: com o pareamento ok, a fase de hopping
ativo (fragmentos, troca de canal a cada 220ms) ainda falha na grande
maioria dos hops (~0–10% de entrega). PA, DPL, assentamento de PLL,
contenção de mutex de telemetria, rajada de relógio na largada e mesmo
um teste decisivo de offset de fase (dwell 2000ms + ~76 reenvios/hop,
ainda só 2/14 hops) já foram descartados como causa única — ver detalhe
completo em `openspec/changes/adaptive-rendezvous/design.md`, seção
"Correção". Causa raiz ainda desconhecida.

A seção "Bloqueio atual" e "Diagnóstico em duas rodadas" abaixo ficam só
como registro histórico do processo de diagnóstico (útil pra não repetir
os mesmos passos) — a conclusão final delas (trocar o módulo VSPI da
Placa A) **não deve ser seguida**.

## Bloqueio atual (histórico — ver correção acima): comunicação RF não fecha nesta bancada

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

### Diagnóstico em duas rodadas (2026-09-17) — conclusão: rádio VSPI da Placa A, só como transmissor

**Rodada 1** — `env:validacao` (jammer VSPI + analisador HSPI no mesmo
ESP32, já existente antes desta sessão, usado por `tools/spectrum/`):
Placa B passa (analisador detecta o próprio jammer da mesma placa), Placa
A falha (analisador HSPI lê zero em todos os 126 canais, qualquer
condição, inclusive ruído ambiente que a Placa B capta normalmente).
Apontava inicialmente pro HSPI da Placa A — mas essa conclusão estava
incompleta, porque `testCarrier()`/RPD (o que o analisador usa) é um
caminho de detecção diferente da recepção normal de pacote, e não testa
transmissão nenhuma.

**Rodada 2** — `Tests/AntennaCross/` (par TX/RX standalone mínimo, sem
FHSS/pareamento/protocolo do projeto — só manda e conta pacotes por
`writeFast()+txStandBy()`, nunca trava mesmo sem resposta). Testa cada
rádio de cada placa como transmissor e como receptor, isoladamente, em
canais fixos configuráveis (`Tests/AntennaCross/tx/main.cpp`, defines
`CH_VSPI`/`CH_HSPI`). Rodado nas duas direções e em dois canais
diferentes pro VSPI (76 e 100, pra descartar interferência de canal
específico):

| Direção | VSPI | HSPI |
|---|---|---|
| Placa A → Placa B | **falha** (0–1 de ~30+ enviados, nos dois canais) | 100% |
| Placa B → Placa A | 100% | 100% |

**Conclusão final**: única combinação que falha é **o rádio VSPI da
Placa A especificamente como transmissor**. O receptor desse mesmo
rádio funciona (Placa A recebendo, na segunda direção); o rádio HSPI da
mesma placa transmite normalmente; tudo na Placa B funciona nos dois
sentidos. Isso não é um problema de alimentação, alcance ou canal
(ambos descartados pelos testes acima) — é um componente específico:
provavelmente o estágio de saída/amplificador de potência do módulo
VSPI da Placa A. Essa é exatamente a posição do **mestre** no firmware
principal, o que explica o pareamento nunca ter funcionado em nenhuma
das duas fases desta sessão.

### Próximo passo (atualizado 2026-09-18): soak longo + validação sob jammer

~~Trocar o módulo VSPI da Placa A~~ — descartado, ver correção acima: os
4 módulos funcionam. ~~Investigar causa da falha de hopping~~ —
encontrada e corrigida: hopping era por relógio periódico, trocado por
evento com fallback de timeout (`openspec/changes/adaptive-rendezvous/design.md`
"Correção 2"). Pareamento/seed/hopping já funcionam de forma confiável
num teste curto.

O que falta antes de fechar `adaptive-rendezvous` seção 6 por completo:

1. **Soak de vários minutos** (tasks.md 6.4) com jammer desligado —
   confirmar que o PLR de ~0,6% observado num teste de ~1-2 min se
   mantém estável (não degrada) ao longo do tempo.
2. **Jammer sweep ativo** (tasks.md 6.2) — confirmar que o novo hopping
   por evento continua convergindo e escapando de canais ruidosos sob
   ataque real, não só em bancada limpa.
3. **Perda de sincronismo forçada** (tasks.md 6.3) e **resync de
   `hop_counter` após reboot assimétrico** (tasks.md 6.5) — ainda não
   re-testados com o novo mecanismo de hop.

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

`pair_ok`/`seed`/hopping já funcionam de forma confiável num teste curto
(confirmado em 2026-09-18 após trocar hopping por relógio periódico por
hopping por evento, ver `openspec/changes/adaptive-rendezvous/design.md`
"Correção 2"). O que falta pra destravar a bateria T1–T7 do plano de
bancada não é mais nenhum bug conhecido — é rodar as validações mais
longas/sob ataque que só um teste curto de bancada não cobre: soak de
vários minutos, jammer sweep ativo, perda de sincronismo forçada (ver
"Próximo passo" acima).
