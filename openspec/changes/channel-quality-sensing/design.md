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
