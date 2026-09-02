## Context

`tools/spectrum/main.py` já sabe: ler uma porta serial em thread separada
(`SerialReaderThread`), enviar comandos (`send_command`/
`_send_jammer_command`), ciclar jammer on/off por tempo
(`_desired_jammer_on`, `_apply_auto_jammer`), gravar amostras em
`pandas.DataFrame` e salvar CSV (`_save_data`), e plotar heatmap a partir
de um CSV salvo (`ViewTab._plot_heatmap`). O que falta é: uma segunda fonte
serial (escravo, com PLR de `plr-metric`), um modo de firmware sem hopping,
e a orquestração/rotulagem dos 3 cenários nomeados pelo guia.

## Goals / Non-Goals

**Goals:**
- Reaproveitar ao máximo `SerialReaderThread`/`RecordTab` já existentes —
  instanciar uma segunda thread de leitura para a porta do escravo, em vez
  de reescrever a camada serial.
- Cenário A precisa de um modo de firmware genuinamente sem hopping
  (canal fixo), não apenas "hopping com jammer desligado".
- Orquestração determinística: cada cenário roda por N segundos fixos,
  nesta ordem: A → B → C, com o jammer no modo certo para cada um (off /
  sweep ou targeted / off-então-alerta-dispara-mitigação).
- Saída: um CSV consolidado por execução completa (3 cenários) com coluna
  `scenario`, mais os gráficos de throughput/latência pedidos pelo guia.

**Non-Goals:**
- Não reimplementa a GUI do zero — extensão de `tools/spectrum/main.py`
  (ou um script CLI companheiro para rodar sem interação, se for mais
  prático para automação repetida em bancada).
- Não inventa uma métrica de "throughput" nova no firmware além de
  fragmentos confirmados por segundo — é a unidade que o protocolo atual
  já produz de forma natural (não há um payload de aplicação maior para
  medir throughput "de dados reais").
- Não tenta rodar os 3 cenários em paralelo — são sequenciais, o guia
  também descreve como comparação sequencial.

## Decisions

- **Modo canal fixo no mestre/escravo**: reutilizar `PAIRING_CHANNEL` (ou
  um canal fixo configurável dentre `HOPPING_CHANNELS`) como único canal
  de operação quando o modo "sem hopping" estiver ativo — mestre não
  chama `get_next_channel`/`get_next_active_channel` nesse modo, escravo
  idem; selecionável por comando serial análogo ao `mode` do
  `targeted-jammer`, para não exigir flashes diferentes por cenário.
- **Latência = round-trip de `wait_fragment_ack`**: medir
  `millis()` antes do envio do fragmento e no recebimento do ACK
  correspondente, logar por fragmento confirmado (reaproveita o mesmo
  ponto de código que já loga sucesso/timeout hoje).
- **Throughput = fragmentos confirmados / segundo**, calculado por janela
  (mesmo padrão de `plr-metric`, reaproveitando a ideia de janela
  deslizante para consistência entre métricas).
- **Segunda porta serial na ferramenta Python**: nova
  `SerialReaderThread` apontando para a porta do escravo, com um parser
  de linha diferente do `parse_spectrum_line` (formato de
  `plr-metric`/latência), mesclado por timestamp relativo de gravação (não
  por timestamp de firmware, que não está sincronizado entre placas).
- **Rótulo de cenário como coluna, não arquivo separado por cenário**:
  simplifica a correlação temporal entre PLR/latência/throughput e
  espectro dentro de uma execução — um único CSV por execução completa,
  filtrável por `scenario` na hora de plotar.
- **Script/aba de orquestração dedicada** (`ScenarioTab` na GUI existente,
  ou script CLI novo em `tools/spectrum/`) que sequencia: configurar modo
  do mestre/escravo (fixo/hopping) → configurar modo do jammer (off /
  sweep / targeted) → gravar por N segundos → avançar para o próximo
  cenário — decisão de GUI vs. CLI fica para a implementação, com CLI
  preferível para rodar em bancada sem depender de clique manual em cada
  fase.

## Risks / Trade-offs

- Sincronização entre 3 placas físicas (mestre, escravo, validacao) e o PC
  via 2 portas seriais tem jitter inerente (USB, SO) — timestamps são
  relativos ao momento de recepção no PC, não aos relógios dos ESP32;
  aceitável para comparação de tendência (guia pede gráficos comparativos,
  não medição de latência de laboratório de precisão).
- Cenário C exige que `dynamic-seed-blacklist` já esteja calibrado
  (thresholds de `spectrum-attack-classification` e do blacklist) — se
  esse change ainda estiver com limiares não calibrados, o Cenário C não
  vai mostrar "self-healing" de forma convincente; a ordem de dependência
  do `project.md` já reflete isso (`scenario-automation` por último).

## Referências externas (auditoria do projeto IC, 2026-09-02)

O artigo do projeto de IC que originou o TCC (`Artigo/conference_101719.tex`
em `github.com/Seijin18/IC`) descreve um protocolo de medição relevante
para a orquestração deste change, sem contradizer as decisões já tomadas:

- **Captura contínua com múltiplos ciclos liga/desliga do jammer dentro da
  mesma sessão**, em vez de uma captura por condição: o experimento de
  jamming do IC ligou/desligou o jammer 3 vezes dentro de uma única
  gravação de 60 s, permitindo comparar diretamente jammer-on vs.
  jammer-off sem variação de ambiente entre gravações separadas. Vale
  considerar o mesmo padrão dentro do Cenário C (`dynamic-seed-blacklist`
  em ação): ao invés de um único ciclo ataque→mitigação, orquestrar 2-3
  ciclos de ataque dentro da mesma janela de captura do Cenário C reforça
  estatisticamente a evidência de "self-healing" (task 4.2 deste change) e
  segue o mesmo protocolo já usado para validar `dynamic-seed-blacklist`
  (task 4.4 lá). Não é uma mudança de escopo, é um refinamento de como a
  task 3.1 (orquestração) implementa a janela do Cenário C — decisão final
  fica para a implementação.
- **Protocolo de baseline antes de cada condição**: o IC roda 60 s de
  baseline sem nenhum dispositivo/ataque ativo antes de cada captura por
  dispositivo, para caracterizar o piso de ruído daquela sessão. Sugestão
  para a task 3.1: gravar um trecho curto de baseline (jammer off, hopping
  normal) no início de cada execução completa dos 3 cenários, não só
  confiar no Cenário A como baseline (o Cenário A já tem jammer ativo desde
  o início, não serve como piso de ruído do ambiente).

Nenhuma dessas referências exige mudança nas `Decisions` já registradas
acima; são sugestões de refinamento para a task 3.1 (orquestração) a
considerar na implementação, não pré-requisitos.
