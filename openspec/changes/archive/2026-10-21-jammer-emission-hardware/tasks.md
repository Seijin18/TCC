## 1. Baseline de emissão pelo caminho confiável (co-locado, sem Placa A)

- [x] 1.1 Sonda dedicada `Tests/JammerTx/` (env `jam_tx_probe`) em vez do
      `env:validacao`: TX = módulo do JAMMER (HSPI CE16), RX/RPD = módulo do
      ESCRAVO (VSPI CE22) na mesma Placa B. Isola o **transmissor** do jammer
      (o `validacao` original, por causa da pinagem, testava o escravo como TX).
- [x] 1.2 Resultado: com `cw on 125` a PA_MAX, o detector co-locado leu **100%**
      no canal de TX (0% com TX off; 93% em ch40). **O transmissor do jammer
      funciona.** "Não irradia" era falso.
- [x] 1.3 Telemetria `RF_SETUP` no `jam_on` adicionada (`tlm_radio_regs(...,
      "jam_on")` após `startCarrier`) — confirma config sob emissão, não só boot.

## 2. Modo de canal fixo (`hold`) + robustez de emissão (firmware)

- [x] 2.1 `JAM_MODE_HOLD` + `loopHold()` — portadora parada em `g_hold_channel`.
- [x] 2.2 `mode hold` / `mode hold <ch>` + telemetria `jam_mode_hold`. Interface
      preservada.
- [x] 2.3/2.4 **Achado central**: a portadora do clone DEGRADA ao longo da
      sessão (o mesmo `hold ch10` negava 100% e minutos depois parava, sem mudar
      comando). Causa: firmware não reinicializava o rádio antes de
      `startConstCarrier`. Correção: `startCarrierOn()` faz `stopConstCarrier →
      powerUp → reconfig → startConstCarrier` (mesmo padrão do `radio_reinit`
      do escravo), e o `loopHold` re-arma com reinit completo a cada 2 s.
      Emissão passou a ser **confiável e reprodutível** (3/3 ciclos de 100%).
- [x] 2.5 `pio run` 11/11 envs de HW OK; `pio test -e native_test` 58/58 OK.

## 3. Fixes de hardware (manual) — NÃO NECESSÁRIO

- [x] 3.x **Dispensado.** A causa raiz era firmware (reinit da portadora), não
      hardware. A sonda co-locada e o teste de eficácia confirmam emissão forte
      após o fix, sem tocar em alimentação/antena/módulo. Nota: distância já
      estava descartada (30-50 cm, co-locado). Se um dia a emissão precisar
      alcançar mais longe, a alimentação do PA+LNA (~115 mA) volta a ser a
      primeira hipótese — mas não é necessária para o estudo.

## 4. Validação de EFICÁCIA de jamming (negação de comunicação)

- [x] 4.2 **PROVADO**: enlace preso (`mode fixed` ch10) + jammer `hold ch10`
      PA_MAX → **0 msg_done, 100% de negação** (baseline ~18/s → 0/s), com
      `loss_of_sync` no escravo co-locado. Reprodutível 3/3.
- [x] 4.3 Inundação de payload não foi necessária — a CW `hold` já nega 100%.
      (O probe mostrou que RPD não detecta bem o flood, mas isso é artefato de
      detecção; a CW basta para negar.)

## 4b. Caracterização: quantos canais o jammer nega (pedido do usuário)

- [x] Sweep `hopset <n>` (mestre) forçando o enlace a N canais, jammer `hold`
      num canal do conjunto, mitigação OFF:

      | N canais | retenção do enlace | negação |
      |---------:|-------------------:|--------:|
      |       16 |                 1% |     99% |
      |        8 |                 5% |     95% |
      |        4 |                 5% |     95% |
      |        2 |                 2% |     98% |
      |        1 |                 0% |    100% |

      **Achado**: um jammer de 1 rádio parado em UM canal nega ~95-100% do
      enlace FHSS para QUALQUER N testado (não o esperado 1/N). Um canal
      persistentemente morto colapsa o enlace inteiro por cascata de
      `loss_of_sync` — hopping sozinho NÃO tolera um canal sempre-ruim.
- [x] Follower (`adaptive`, PA_MAX, N=16): reduz throughput a ~50%, sem colapso.

## 5. Mitigação sob ataque real — ACHADO (recuperação apenas parcial)

- [x] 5.1 A/B/C sob `hold ch10` (enlace em hopping, atacante confiável):
      - A baseline: ~9,7/s.
      - B (mitigação OFF): ~0,1/s (**1%** — enlace praticamente negado).
      - C (mitigação ON): ~1,65/s (**~17%** — recupera parcialmente, não total).
      **Achado**: a blacklist reativa NÃO isola o canal atacado de forma limpa.
      O jamming causa DESSINCRONISMO, e a atribuição de falha por-canal fica
      corrompida — a máscara **oscila** (0xFFFF→0x7AFD→0x3AD0→0x2AC0→…),
      culpando canais inocentes em vez de banir só o ch10. Resultado: ajuda
      ~14× (17% vs 1%) mas não restaura o enlace. Isto **corrige** a
      interpretação de "self-healing validado" (era com jammer fraco/degradado).
- [ ] 5.2 (Novo trabalho, fora desta change) Endurecer a mitigação contra
      jammer forte de canal fixo: atribuição de falha robusta a dessincronismo,
      e/ou reseed mais agressivo. Vira uma change própria.

## 6. Fechamento

- [x] 6.1 Registrar no `ROADMAP.md` e memória `project-rf-status`.
- [ ] 6.2 `openspec validate --strict` e decidir arquivamento com o usuário.

---

### Resultado (2026-09-25)

- **Transmissor do jammer**: FUNCIONA (probe co-locado 100% RPD a PA_MAX).
- **Causa raiz do "não funciona"**: portadora do clone degrada na sessão;
  firmware não reinicializava o rádio. CORRIGIDO (reinit em `startCarrierOn`).
- **Eficácia**: `hold` num canal → 100% de negação, reprodutível.
- **Capacidade vs N**: nega ~95-100% para N=1..16 (1 canal morto colapsa o
  FHSS); follower `adaptive` → ~50%.
- **Mitigação**: recupera só ~17% sob ataque forte de canal fixo (máscara
  oscila por atribuição de falha corrompida pelo dessincronismo) — gap real,
  vira change nova.
- **Hardware trocado?**: NÃO (não foi preciso). Alimentação/SDR não usados.
