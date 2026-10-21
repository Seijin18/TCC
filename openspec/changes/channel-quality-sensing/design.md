# Design — qualidade de canal agnóstica à causa

## Decisões

### D1 — Medir ocupação, não classificar causa
`ocupacao[ch]` = fração das últimas `W` varreduras em que `testCarrier()`
acendeu no canal. Estado por canal com histerese:

```
utilizável --(ocupacao >= T_on)--> degradado --(ocupacao <= T_off)--> utilizável
```

Um jammer, Wi-Fi ou outro enlace no canal produzem o mesmo efeito no
receptor do escravo, e o sistema reage do mesmo jeito.

### D2 — Ponto de vista diferente: o analisador é fonte proativa, não juiz
O analisador está na placa A, ao lado do mestre; a qualidade que importa é
a da recepção dos dois lados do enlace. Por isso ele **não decide sozinho**:
a máscara dele é fundida com a evidência do próprio enlace (falhas de ack
por canal no mestre), que continua sendo a verdade sobre o canal. O ganho
do analisador é antecipação — marcar um canal antes de o enlace perder
pacotes nele.

### D3 — A própria transmissão do enlace
Na placa A o analisador também enxerga o mestre transmitindo. A ocupação
causada pelo próprio enlace por canal é aproximadamente
`duty_do_mestre × 1/16` (cada canal da tabela é visitado 1/16 do tempo),
muito abaixo de um `T_on` razoável. Medir na calibração; se ficar perto do
limiar, o analisador descarta as amostras do canal em que o mestre está no
slot corrente (informação disponível na mesma placa).

### D4 — Fusão e guardas
`mascara_ativa = update_channel_mask(...)` com entrada de falha =
`falha_do_enlace[i] OR degradado_analisador[i]`. Mantém: nunca menos de 4
canais ativos (os de melhor histórico), histerese
(`MASK_HYSTERESIS_HOPS`), cooldown de reinclusão. Quando o analisador
volta o canal para utilizável, a contribuição dele some; a reinclusão
continua respeitando o cooldown.

### D5 — Reseed
Blacklist resolve canal ruim. Seed nova serve para quando o atacante segue
o padrão de salto ou a banda está tomada em muitos pontos. Gatilhos
propostos (calibrar): `>= K` canais degradados simultâneos, ou miss do
enlace acima de X% por N janelas depois de a blacklist estar aplicada.

### D6 — Calibração
Script em `tools/validation/` que, para cada sessão, cruza a ocupação por
canal no `spectrum.csv` com o miss% do mestre no mesmo canal e período.
Escolher `T_on` tal que canais acima dele tenham miss ≥ X% (X ≈ 20%) e
`T_off` com folga para não oscilar. Dados de referência que já existem:
check de acoplamento de 2026-10-21 (jammer a 100% no canal travado), baseline
de Wi-Fi (ch30-36 até ~16% de ocupação média), miss do ch34 entre 7% e 68%.

## Riscos
- Banir canal que o escravo ainda recebe bem: custa só capacidade; a guarda
  de 4 canais limita o dano.
- Wi-Fi + jammer encolherem demais a banda: guarda de 4 canais; o reseed
  cobre o caso extremo.
- Mudança de semântica do alerta (booleano → máscara) nos testes
  `run_scenario`/`run_battery`: atualizar os scripts que procuram
  `alert_on`.

## Resultado da calibração 2026-10-21 (tarefa 1, decide os limiares)

Análise offline sobre os dados em disco (`tools/validation/calibrate_chq.py`;
12 baterias `hold`, 8 cenários sweep/targeted/adaptive de hoje, 3,7 h de soak
sem jammer). Métrica de ocupação = média da célula do `spectrum.csv` (0-100,
% de 10 amostras de `testCarrier()` por varredura) sobre `W` varreduras
(~0,9 s cada).

**Limiares escolhidos: `W=10` (~9 s), `T_on=30`, `T_off=15`.**

Base da escolha:
- **Separação perfeita para ataque forte (hold / qualquer modo que trave um
  canal em 90-100):** canais limpos nunca passam de 15 (p99,9 = 11) em 17.649
  pontos; o jammer hold fica em 90-100. Zero sobreposição. Latência mediana de
  detecção ~2,7 s (máx 6,3 s); volta ao normal ~8 s após `jam off`. Dos pontos
  sinalizados, 92% com miss ≥ 20%.
- **T_off=15 (=T_on/2):** absorve as quedas pontuais de 1 varredura do jammer
  (ex. 100,100,0,100) sem perder o flag; o flicker do ch34 vem do cruzamento
  de T_on, quase não depende de T_off.

**Achado que corrige o framing da proposta (a ocupação NÃO separa tudo):**
- O Wi-Fi (ch34) em ocupação moderada (~11) custa 15-17% de miss em algumas
  sessões e ~4% em outras — correlação ocupação×miss ≈ 0 no ch34 nessas
  janelas. Não existe limiar na faixa 10-30 que separe "Wi-Fi que dói" de
  "Wi-Fi inofensivo": baixar T_on para ~10-12 sinalizaria o ch34 45-78% do
  tempo (flicker alto) e o miss do sinalizado (12-13%) é igual ao do
  não-sinalizado (11,4%).
- Consequência de design: a ocupação é confiável só para **ocupação forte**
  (ataque que realmente toma o canal, e Wi-Fi quando de fato dispara alto). A
  faixa moderada fica para a **evidência do próprio enlace** (falha de ack por
  canal, D2). Por isso a **fusão (D4) é essencial, não opcional**: o analisador
  é sensor proativo de alta precisão para ataque forte; o enlace é o backstop
  confiável do resto.
- D3 confirmado: auto-ocupação do mestre ≈ 0,2 (< 1/50 de T_on) — não precisa
  descartar o canal do slot corrente.

Constantes de firmware: `CHQ_WINDOW_SWEEPS=10`, `CHQ_T_ON=30`, `CHQ_T_OFF=15`.

Ressalvas: dados de uma única bancada e uma única condição de Wi-Fi ambiente;
os dados de jammer em hopping têm poucas tentativas no canal atacado (o enlace
colapsa). A validação em bancada (tarefa 4) confirma o comportamento vivo.
