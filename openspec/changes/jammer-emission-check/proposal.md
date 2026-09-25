## Why

Antes de gastar outra bateria A/B/C ou de reposicionar placas, precisamos
confirmar um fato mais básico: **o jammer está de fato irradiando?** As
evidências acumuladas apontam que talvez não, ou muito pouco:

- Bateria `targeted` a `power high` **e** `power max`, com mitigação
  desligada (sessão `session-20260925-015833`, cenários
  `scenario-20260925-053601`/`-053837`): a taxa de `miss` do mestre **não
  subiu** durante o jamming em relação à baseline (6,0% → 6,0% em high; 8,4%
  → 5,1% em max). Excluindo o canal 34 (impairment fixo do Wi-Fi real, ~50-65%
  de miss em **todas** as fases, inclusive baseline e recovery), os demais 15
  canais ficam em 0-9% e não se mexem quando o jammer liga.
- O **escravo está na mesma placa que o jammer** (placa B, a centímetros do
  rádio do jammer). Se uma portadora contínua a `PA_MAX` estivesse mesmo no
  ar, o escravo co-locado seria a primeira coisa a dessensibilizar — e não é.
- O analisador não levantou **nenhum** `alert_on` durante os dois episódios
  de jamming; o `spectrum.csv` mostra só um leve aumento nos 16 canais da
  tabela durante o jamming, abaixo do nível ambiente do resto da banda.
- O mesmo modo `targeted` já tinha produzido resultados muito diferentes no
  mesmo dia sem mudança de código (motivo do retune determinístico em
  `19501d4`), o que é sintoma de emissão intermitente/instável.

Hipótese de causa raiz (a confirmar): `RF24::startConstCarrier`
(`.pio/libdeps/jammer/RF24/RF24.cpp:2103`) só executa o caminho que
**sustenta** a portadora — carregar um payload dummy no TX FIFO e chamar
`reUseTX()` para manter CE transmitindo — quando `isPVariant()` é verdadeiro.
Num clone **não-P**, ele apenas seta `CONT_WAVE`+`PLL_LOCK` e sobe CE com o
TX FIFO vazio, o que em muitos clones produz portadora fraca, intermitente ou
ausente. O firmware nunca registrou qual variante o rádio do jammer reporta.

Sem responder "o jammer irradia?" primeiro, qualquer resultado de bateria é
ambíguo: "mitigação venceu o ataque" e "não havia ataque" são
indistinguíveis nos dados.

## What Changes

- **Teste de isolamento decisivo (sem firmware)**: com o enlace desligado
  (mestre sem transmitir), jammer em `mode targeted` + `power max`, comparar
  o `spectrum.csv` do analisador na **faixa alta limpa (canais 94-125)** com
  o jammer OFF vs ON. Essa faixa não tem Wi-Fi (que vive em ~26-45) nem
  tráfego do enlace, então qualquer elevação ali é atribuível **só** ao
  jammer. Resposta binária: irradia / não irradia.
- **Diagnóstico de variante no boot do jammer (firmware, condicional)**:
  o jammer passa a emitir sua telemetria de registradores no boot
  (`isPVariant`, `RF_SETUP`), espelhando o `tlm_radio_regs` que mestre e
  escravo já têm. Explica um resultado negativo do teste de isolamento.
- **Emissão robusta em clone não-P (firmware, condicional)**: se o rádio for
  não-P e o teste confirmar portadora ausente/fraca, trocar a emissão do modo
  targeted de portadora constante (`startConstCarrier`/`hopCarrier`) por um
  método que funciona em clones — transmissão contínua de payloads fixos no
  canal alvo (auto-ack desligado, sem espera de ACK) — mantendo a mesma
  interface de comando serial (`mode`/`jam`/`power`).

Os dois itens de firmware são **condicionais ao resultado** do teste de
isolamento: se o jammer já irradia bem, só o diagnóstico de boot é
implementado (barato, ajuda a documentar) e a conclusão vira "jammer OK, CW
estreito é fácil de escapar por hopping rápido" — que é um resultado de TCC
válido por si só.

## Capabilities

### New Capabilities
- `jammer-emission`: garantia verificável de que o jammer produz energia de
  RF observável quando habilitado, com telemetria de variante do rádio no
  boot e um método de emissão que funciona também em clones não-P.

### Modified Capabilities
(nenhuma — `targeted-jamming` continua definindo *quais* canais o modo
targeted ataca; esta change cobre *se a emissão de fato acontece*.)

## Impact

- **Nenhum arquivo** no caminho "só teste de isolamento" (usa comandos
  seriais e ferramentas já existentes: `logs/cmd.in`, `tools/logger`,
  `logs/spectrum.csv`).
- `jammer/src/node.h` (condicional): chamada de `tlm_radio_regs("jammer",
  ...)` no boot; e, se necessário, novo caminho de emissão para clone não-P.
- `shared/radio_diag.h`: reutilizado como está (já expõe `tlm_radio_regs`).
- Destrava `targeted-jammer` 3.2 (verificação visual de emissão),
  `scenario-automation` 4.2/4.3 (a decisão de metodologia A/B/C depende de
  saber se o jammer atual serve como atacante) e a interpretação de toda a
  série de baterias já coletadas.
