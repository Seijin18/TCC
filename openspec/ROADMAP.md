# Roadmap de implementação — TCC Anti-Jamming

Consolida o estado dos changes ativos (`openspec/changes/`) num plano de
execução único. Não substitui `project.md` (que continua sendo a fonte de
verdade de contexto/dependências) — é a camada de "o que fazer agora, em que
ordem" derivada dele. Atualizar esta lista conforme tasks forem fechadas ou
changes forem arquivados.

## Estado em 2026-09-17: hardware validado, pareamento funciona, hopping ainda bloqueado (bug de software não resolvido)

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

### Próximo passo (atualizado): investigar a falha específica da fase de hopping

~~Trocar o módulo VSPI da Placa A~~ — descartado, ver correção acima: os
4 módulos funcionam. Pareamento/seed já funcionam de forma confiável.

O que falta é achar por que o envio de fragmentos falha especificamente
durante troca ativa de canal (~0–10% de entrega), quando o mesmo
`radio_send()` no canal fixo do pareamento funciona quase 100%. Hipóteses
ainda não testadas, por ordem de suspeita:

1. `flush_rx()` explícito logo após `setChannel()` em ambos os lados —
   verificar se o RX FIFO carrega lixo de antes da troca de canal.
2. Reproduzir `Tests/AntennaCross` com troca de canal ativa a cada
   ~220ms (hoje ele só testa canal fixo) — se a falha aparecer isolada
   ali, descarta de vez qualquer interação com o resto do protocolo
   (pareamento, `hop_counter`, `FragmentPacket`) e confirma que é
   específico da troca de canal em si.
3. Comparar captura de telemetria (`tlm_printf`) dos dois lados hop a
   hop lado a lado (timestamp de `setChannel()`, de envio, de
   `startListening()`) pra checar se a janela de escuta do escravo e a
   de envio do mestre realmente se sobrepõem no relógio real, não só no
   `HOP_INTERVAL_MS` nominal.

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

`pair_ok`/`seed` já acontecem de forma confiável (confirmado em
2026-09-17 após remover `enableAckPayload()`) — a bateria T1–T7 do plano
de bancada segue bloqueada não mais por hardware, mas pela fase de
hopping ativo (envio de fragmentos) ainda falhar na maioria dos hops.
Resolver isso é o item que destrava toda a validação restante — ver
"Próximo passo" acima e `openspec/changes/adaptive-rendezvous/design.md`
("Correção") para o que já foi descartado como causa.
