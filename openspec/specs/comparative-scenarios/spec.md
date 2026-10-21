# comparative-scenarios Specification

## Purpose
TBD - created by archiving change scenario-automation. Update Purpose after archive.
## Requirements
### Requirement: Modo de operação sem hopping
Mestre e escravo DEVEM (MUST) suportar um modo de teste "canal fixo", selecionável
em runtime, no qual ambos permanecem em um único canal conhecido durante
toda a sessão, sem invocar o cálculo de próximo canal do FHSS.

#### Scenario: Sessão em modo canal fixo
- **WHEN** mestre e escravo são configurados para o modo "canal fixo"
- **THEN** todos os fragmentos e ACKs da sessão trafegam no mesmo canal,
  sem qualquer salto entre canais

### Requirement: Log de latência por fragmento confirmado
O mestre DEVE (MUST) registrar, para cada fragmento confirmado, o tempo decorrido
entre o envio e o recebimento do ACK correspondente, em uma linha de log
serial parseável.

#### Scenario: Fragmento confirmado no primeiro envio
- **WHEN** um fragmento é enviado e o ACK correspondente chega antes do
  timeout
- **THEN** uma linha de log com a latência medida (em ms) é emitida para
  aquele fragmento

### Requirement: Log de throughput por janela
O mestre DEVE (MUST) calcular e logar a taxa de fragmentos confirmados por
segundo, agregada por janela de tempo, no mesmo espírito de janela
deslizante usado por `packet-loss-metric`.

#### Scenario: Janela sem nenhum fragmento confirmado
- **WHEN** uma janela de agregação fecha sem nenhum fragmento confirmado
  nela
- **THEN** o throughput reportado para aquela janela é 0

### Requirement: Captura correlacionada de múltiplas portas seriais
A ferramenta de coleta DEVE (MUST) ler simultaneamente a porta serial do nó de
espectro/jammer (analisador ou `validacao`) e a porta serial do escravo
(PLR/latência/throughput), associando as amostras de ambas por timestamp
relativo de recepção no host.

#### Scenario: Gravação com as duas portas conectadas
- **WHEN** uma gravação é iniciada com ambas as portas seriais
  configuradas e conectadas
- **THEN** o CSV resultante contém, para cada linha de tempo, tanto dados
  de espectro/atividade de canal quanto PLR/latência/throughput do mesmo
  intervalo

#### Scenario: Uma das portas seriais indisponível
- **WHEN** a gravação é iniciada mas uma das duas portas seriais não está
  disponível/conectada
- **THEN** a ferramenta reporta erro claro identificando qual porta falhou,
  sem iniciar uma gravação parcialmente inconsistente

### Requirement: Execução sequencial dos 3 cenários nomeados
A ferramenta DEVE (MUST) oferecer um modo de orquestração que executa, em
sequência e por uma duração configurável cada, os 3 cenários do guia:
(A) canal fixo sem jammer; (B) hopping fixo (Fase 1) vs. jammer targeted;
(C) sistema completo (hopping adaptativo + mitigação) vs. jammer — cada
amostra gravada rotulada com o nome do cenário correspondente.

#### Scenario: Execução completa da bateria de 3 cenários
- **WHEN** o operador inicia a orquestração completa com durações
  configuradas para os 3 cenários
- **THEN** a ferramenta configura mestre/escravo/jammer no modo
  apropriado para cada cenário, grava por a duração configurada e avança
  automaticamente para o próximo, produzindo um único CSV consolidado com
  a coluna `scenario` preenchida corretamente para cada trecho

### Requirement: Gráficos comparativos de throughput e latência
A ferramenta DEVE (MUST) gerar, a partir do CSV consolidado de uma execução, um
gráfico de throughput por tempo e um gráfico de latência por tempo, com os
3 cenários visualmente distinguíveis (ex. cor ou faceta por `scenario`).

#### Scenario: Geração de gráfico após execução completa
- **WHEN** um CSV consolidado com os 3 cenários rotulados está disponível
- **THEN** a ferramenta produz um gráfico de throughput × tempo e um de
  latência × tempo, ambos permitindo distinguir visualmente em qual
  cenário cada trecho foi coletado

