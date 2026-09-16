# Pinout — módulos NRF24L01+ por nó

Este documento descreve a pinagem SPI usada para conectar os módulos
NRF24L01+ a cada ESP32, tanto no cenário completo (4 placas) descrito no
[README](../README.md) quanto num cenário alternativo para quem só dispõe de
2 placas.

Os pinos abaixo são os já usados no firmware (`#define` em cada
`src/main.cpp>`) — este documento apenas os consolida numa tabela, não
introduz nenhum valor novo.

## Barramento único (VSPI) — 1 módulo por placa

Usado por `mestre/`, `escravo/` e `analisador/` no cenário completo, e por
`jammer/` quando rodando com um único rádio (`has_radio2 = false`).

| Sinal | GPIO |
|-------|------|
| CE    | 22   |
| CSN   | 21   |
| SCK   | 18   |
| MISO  | 19   |
| MOSI  | 23   |

## Dois barramentos (VSPI + HSPI) — 2 módulos na mesma placa

Usado por `jammer/` (dual-rádio, sweep em duas frequências simultâneas) e por
`validacao/` (harness que combina jammer + analisador numa única placa).

| Sinal | VSPI (rádio 1) | HSPI (rádio 2) |
|-------|----------------|----------------|
| CE    | 22             | 16             |
| CSN   | 21             | 15             |
| SCK   | 18             | 4              |
| MISO  | 19             | 2              |
| MOSI  | 23             | 17             |

## Alerta analisador → mestre (spectrum-attack-classification)

Fio de sinal (GPIO) + GND comum entre a placa do analisador e a do mestre,
usado pelo analisador para sinalizar "ataque direcionado ativo" (nível
HIGH mantido enquanto o ataque persistir, LOW caso contrário — ver
`openspec/changes/spectrum-attack-classification/design.md`). Não conflita
com os pinos SPI de nenhum dos dois nós (ambos usam só VSPI).

| Sinal        | Analisador (saída) | Mestre (entrada) |
|--------------|---------------------|-------------------|
| ALERT        | GPIO 4              | GPIO 4            |
| GND (comum)  | GND                 | GND               |

No cenário de 2 placas (mestre + analisador na mesma placa, ver seção
abaixo), este fio deixa de ser necessário — vira leitura direta de uma
flag compartilhada entre as duas tasks, conforme nota no `design.md`.

## Cenário alternativo: 2 placas, 4 módulos (sem hardware para o cenário completo)

O cenário completo descrito no README pressupõe 4 placas ESP32, uma por nó
(mestre, escravo, jammer, analisador). **Esta seção documenta uma variante
para quem só dispõe de 2 placas** — cada placa roda dois nós diferentes,
cada um com seu próprio módulo NRF24 num barramento SPI dedicado (mesmo
esquema de pinos VSPI/HSPI da seção anterior). Não é o cenário de referência
do projeto, é uma adaptação para hardware limitado.

O agrupamento não é arbitrário — segue a relação funcional entre os nós:

- **Placa A: mestre (VSPI) + analisador (HSPI).** O analisador existe para
  alertar o mestre sobre um ataque em andamento
  (`openspec/changes/spectrum-attack-classification/design.md`), e só o
  mestre consome esse alerta (o escravo não age sobre ele). Colocando os
  dois na mesma placa, o alerta deixa de precisar do fio de GPIO entre
  placas previsto no design — vira uma leitura direta entre duas tasks
  FreeRTOS (uma por núcleo do ESP32) na mesma placa. **Isso é um desvio do
  design documentado (que assume duas placas separadas ligadas por GPIO) —
  ver nota nesse `design.md`.**
- **Placa B: escravo (VSPI) + jammer (HSPI).** O escravo só segue os saltos
  do mestre e recebe — não coordena nada, então tolera bem dividir a placa.
  O jammer colocado fisicamente perto do receptor é, na prática, o cenário
  mais exigente para o link (atacante próximo da vítima), não um
  compromisso de realismo.

Cada nó combinado precisa rodar em sua própria task FreeRTOS (uma por
núcleo), já que cada `main.cpp` atual assume que é o único ocupante do
`loop()` — isto ainda não está implementado, é o firmware combinado a ser
escrito para usar este pinout.

### Placa A — mestre + analisador

| Sinal | Mestre (VSPI) | Analisador (HSPI) |
|-------|---------------|--------------------|
| CE    | 22            | 16                 |
| CSN   | 21            | 15                 |
| SCK   | 18            | 4                  |
| MISO  | 19            | 2                  |
| MOSI  | 23            | 17                 |

### Placa B — escravo + jammer

| Sinal | Escravo (VSPI) | Jammer (HSPI) |
|-------|----------------|----------------|
| CE    | 22             | 16             |
| CSN   | 21             | 15             |
| SCK   | 18             | 4              |
| MISO  | 19             | 2              |
| MOSI  | 23             | 17             |

### Implicações de usar 2 placas em vez de 4

Este arranjo não é "gratuito" — ele troca placas por outros riscos que o
cenário de referência (4 placas) não tem. Vale considerar cada um antes de
tirar conclusões dos dados coletados.

**Interferência de RF (proximidade física dos dois módulos)**

- Os dois módulos de uma mesma placa ficam a poucos centímetros um do
  outro. Em RF isso é muito perto: mesmo em canais diferentes, energia
  vazada de um módulo pode elevar o piso de ruído lido pelo outro
  (especialmente com módulos PA+LNA, que o README recomenda justamente pelo
  alcance maior — o mesmo ganho que ajuda o alcance também aumenta o
  acoplamento entre antenas vizinhas).
- Na Placa A, isso é uma faca de dois gumes: o analisador pode captar a
  própria transmissão do mestre como parte do espectro varrido. Isso não é
  necessariamente ruim — o mestre transmite nos 16 canais da tabela de
  hopping, então o analisador *precisa* diferenciar "energia do próprio
  mestre" de "energia do jammer" nesses canais de qualquer forma (é o
  propósito da heurística de classificação). Mas a proximidade extrema pode
  saturar o receptor do analisador nesses canais (energia muito acima do
  esperado a distância normal), distorcendo a calibração de limiar feita
  em `spectrum-attack-classification/design.md`.
- Na Placa B, o jammer emite portadora de alta potência (`RF24_PA_MAX`) a
  centímetros da antena do escravo. Isso é o pior caso de acoplamento
  possível — bem mais agressivo que um jammer a alguns metros de distância
  como o cenário de 4 placas permitiria. Os números de PLR/recuperação
  medidos nesse arranjo tendem a representar um ataque **mais severo** que
  o cenário de referência, não um substituto equivalente dele.
- Recomendação prática: isolar fisicamente os dois módulos de cada placa o
  máximo possível (blindagem, distância nos limites do protoboard,
  orientação das antenas) e tratar os resultados desse arranjo como um
  cenário à parte na análise, não como equivalente ao de 4 placas — vale
  registrar isso na metodologia do TCC caso os dois arranjos sejam
  comparados.

**Execução de código (duas tasks no mesmo MCU)**

- Cada `main.cpp` atual foi escrito assumindo que é o único ocupante do
  `loop()`/CPU da placa. Combinar dois nós exige portar cada um para uma
  task FreeRTOS presa a um núcleo (`xTaskCreatePinnedToCore`), tipicamente
  core 0 para uma e core 1 (onde o Arduino já roda `loop()` por padrão) para
  a outra — isso ainda precisa ser escrito, não é so re-flashear.
- **Mestre + analisador (Placa A)**: o mestre é sensível a timing (janelas
  de handshake, cadência de hop, espera por ACK). Se a task do analisador
  não ceder CPU com frequência suficiente (ex. bloqueando em
  `radio.read()`/varredura de canal sem `vTaskDelay`), pode atrasar o hop
  do mestre e gerar perda de pacote que **não é causada pelo jammer**,
  poluindo a métrica de PLR. Vale medir PLR com o analisador
  ligado-e-parado vs. ligado-e-rodando antes de atribuir qualquer queda ao
  ataque.
- **Escravo + jammer (Placa B)**: o jammer tende a ser mais tolerante a
  jitter (não depende de sincronismo fino), mas ainda compete por CPU/tempo
  de barramento com o escravo. Um jammer "agressivo" (`JAMMER_SPECTRUM_TEST
  = 0`, dwell de 130 µs) monopolizando o núcleo pode atrasar o
  processamento de pacotes recebidos pelo escravo por motivo de
  escalonamento, não de fato por colisão de RF — mesma armadilha de
  atribuição de causa do item anterior.
- Nos dois casos, um watchdog reset ou brownout num núcleo pode reiniciar a
  placa inteira (afeta os dois nós juntos), diferente do cenário de 4
  placas onde uma falha isolada em um nó não derruba o resto do
  experimento.

**Alimentação elétrica**

- Dois módulos NRF24L01+ com PA+LNA no pico de transmissão (`RF24_PA_MAX`)
  podem somar picos de corrente que o regulador 3.3V onboard do ESP32
  (ou de um adaptador de tensão genérico) não foi dimensionado para
  fornecer para dois módulos simultâneos — o README já observa que o NRF24
  é sensível a ruído na linha de 3.3V mesmo com 1 módulo por placa. Quedas
  de tensão sob carga podem causar resets espúrios ou perda de pacote
  indistinguível de jamming real. Alimentar os módulos por uma fonte 3.3V
  externa regulada (não o pino 3V3 do ESP32) é mais importante aqui do que
  no cenário de 1 módulo por placa.

**Validade dos dados coletados**

- Qualquer comparação entre "cenário de 2 placas" e "cenário de 4 placas"
  (caso ambos sejam usados no TCC) deve ser tratada como dois setups
  experimentais distintos, não como repetições do mesmo experimento — a
  proximidade RF e a contenção de CPU introduzem variáveis de confusão que
  o cenário de referência não tem. Se só o setup de 2 placas for usado,
  vale documentar essa limitação na metodologia (banca pode perguntar sobre
  isso).
