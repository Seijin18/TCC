# Design — investigação de emissão de hardware do jammer

## Contexto herdado de `jammer-emission-check` (arquivada)

- **Veredito**: jammer NÃO irradia observavelmente. Banda de teste 94-125,
  fases de 120 s, `targeted`+`power max`: combinado ON=0.000 vs OFF=0.008;
  zero `loss_of_sync` no escravo co-locado (a centímetros do jammer).
- **Rádio do jammer**: P-variant (`p=1`, `radios=1`), `rf_setup=0B` no boot.
  Logo `startConstCarrier` já usa payload dummy + `reUseTX` (caminho correto).
- **Descartado**: firmware não-P (o rádio é P), distância (30-50 cm, LOS
  limpa; co-locado também não sente nada).
- **Ferramentas prontas**: `tools/validation/emission_check.py` (`run`
  orquestra OFF/ON + análise; `analyze` reprocessa janelas). Boot do jammer
  já emite `e=regs;why=boot` e `e=variant`.

## Por que um modo de canal fixo ("hold") primeiro

Antes de condenar o hardware, remover o último confundidor de software. Os
três modos atuais **saltam**:

| Modo | Comportamento | Problema para detecção |
|---|---|---|
| `targeted` | ~60 ms/canal, 16 canais | cada canal só ~6% das varreduras |
| `sweep` | percorre 2-80 | não cobre 94-125 e reparte tempo |
| `adaptive` | sensoria e trava, mas só em canal com atividade | não trava numa banda limpa vazia |

Uma **portadora parada** num canal alto fixo (ex. 125) por ≥ 120 s dá:
- ao analisador: ~100% de chance de ver, em vez de ~6% (varre aquele canal
  ~1x/varredura, e a portadora está sempre lá);
- ao escravo co-locado: se o link for posto em `mode fixed` no **mesmo** canal
  do hold, a portadora bate direto no canal do enlace — o teste mais duro
  possível de dessensibilização.

Se **mesmo parado** a 125 por 120 s o analisador não vê e o co-locado no
mesmo canal não cai, a conclusão "hardware não emite" fica à prova de
confundidor de software.

Bônus: esse modo é exatamente o "atacante de canal fixo" que falta para
definir o Cenário A da bateria (`scenario-automation` 4.2/4.3).

## Detalhe de implementação do modo hold

- Reusar `startCarrier`/`startCarrierOn` já existentes (portadora constante
  via `startConstCarrier`, que no rádio P sustenta). No `hold`, **não** chamar
  `hopCarrier` no loop — deixar a portadora parada no canal escolhido.
- Comando serial: `mode hold` (usa canal default alto, ex. 125) e,
  opcionalmente, `mode hold <ch>` para escolher o canal. Adicionar ao parser
  `handleCommand` do jammer, junto de `mode sweep|targeted|adaptive`.
- Novo enum `JAM_MODE_HOLD`; `loopHold()` que só mantém a portadora (e o
  heartbeat), sem trocar canal. Telemetria `jam_mode_hold` (espelhando
  `jam_mode_targeted` etc.), preservando `jam_state_on/off`.
- Não quebrar `run_scenario.py`/`run_battery.py`: só acrescenta um modo.

## Ramo de decisão

```
mode hold @125, power max, 120s (via emission_check.py --mode hold --ch 125)
   │
   ├─ analisador vê 125 subir claramente OU co-locado (link fixo em 125) cai
   │     ──► o jammer EMITE; o problema anterior era só dwell/dispersão.
   │         Conclusão de TCC: CW estreito é escapável por hopping rápido;
   │         hold serve de atacante do Cenário A. Retomar bateria A/B/C.
   │
   └─ nada muda mesmo parado a 125 por 120s
         ──► hardware. Checklist manual:
             1. antena/conexão do módulo do jammer (solда, cabo, u.FL);
             2. medir 2400+125 MHz = 2525 MHz com SDR/analisador externo
                com o hold ligado — confirma emissão real ou ausência;
             3. comparar PA real / RF_SETUP; testar outro canal (ex. 2);
             4. trocar o módulo nRF24 do jammer por um bom e repetir.
```

## O que NÃO fazer nesta change

- Não reintroduzir o fix de clone não-P (rádio é P — não se aplica).
- Não mexer em `mode targeted` (quais canais ataca — isso é `targeted-jammer`).
- Não rodar bateria A/B/C aqui — esta change desbloqueia a metodologia dela,
  mas a bateria em si é de `scenario-automation`.
