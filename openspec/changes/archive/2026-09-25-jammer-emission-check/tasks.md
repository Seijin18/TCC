## 1. Preparação (sem firmware)

- [ ] 1.1 Confirmar que o logger está de pé (`pgrep -af "[t]ools/logger/main.py"`)
      e anotar a sessão corrente (`logs/session-*.jsonl` mais recente). Não
      reiniciar o logger.
- [ ] 1.2 Confirmar que o analisador varre a faixa alta: checar em
      `logs/spectrum.csv` que existem colunas não-nulas nos índices 94-125
      (se `FAST_BT_SCAN=1` estiver ativo o analisador não enxerga acima de
      80 — nesse caso registrar e parar; exige reflash com `FAST_BT_SCAN=0`).

## 2. Teste de isolamento — o jammer irradia? (sem firmware, decisivo)

> Método corrigido na revisão 2026-09-25 (ver `design.md`, "Correção do
> método original"): **NÃO desligar o enlace** — sem enlace o jammer nem
> emite (só liga com `ready`) e o mestre despareado polui a faixa alta
> sondando os `PAIRING_CANDIDATES`. Em vez disso, manter o enlace pareado e
> os dois lados em `mode fixed` (trava em ch 10, fora de 94-125). Automatizado
> em `tools/validation/emission_check.py`.

- [x] 2.1 Confirmar que o enlace está pareado (`state.json`: `msg_done`
      recente nos dois lados). Travar em canal fixo fora da faixa:
      `echo "placa_a mode fixed" >> logs/cmd.in`; `echo "placa_b mode fixed"
      >> logs/cmd.in`. Aguardar assentar e confirmar que o enlace continua
      vivo (novos `msg_done`).
- [x] 2.2 Garantir jammer OFF e anotar o epoch: `echo "placa_b jam off" >>
      logs/cmd.in`. Coletar **120 s** de `spectrum.csv` como fase OFF.
- [x] 2.3 `echo "placa_b mode targeted" >> logs/cmd.in`; `echo "placa_b power
      max" >> logs/cmd.in`; `echo "placa_b jam on" >> logs/cmd.in`. Confirmar
      `@jammer;...e=jam_state_on` no log. Coletar **120 s** como fase ON,
      **anotando qualquer `loss_of_sync`** no trecho (queda do escravo
      co-locado = evidência de emissão).
- [x] 2.4 Restaurar: `echo "placa_b jam off" >> logs/cmd.in`; `echo "placa_b
      power low" >> logs/cmd.in`; `echo "placa_a mode hopping" >> logs/cmd.in`;
      `echo "placa_b mode hopping" >> logs/cmd.in`. Confirmar que o enlace
      volta ao hopping normal.
- [x] 2.5 Analisar `spectrum.csv` (via `emission_check.py analyze`): tabela
      média + fração de sweeps por canal {94,100,105,110,115,120,125}, OFF vs
      ON; fração combinada nos 7; contagem de `loss_of_sync` por fase. Aplicar
      o critério do `design.md` ("Critério corrigido"). **Registrar a tabela e
      o veredito (irradia / não irradia)** no `ROADMAP.md` e no fim deste
      tasks.md.

## 3. Diagnóstico de variante no boot do jammer (firmware — barato, sempre útil)

- [x] 3.1 Em `jammer/src/node.h`, no boot (após o `isChipConnected()`),
      chamar `tlm_radio_regs("jammer", jammer_radio, "boot")` (e para
      `jammer_radio2` se `has_radio2`), espelhando o que
      `escravo/src/node.h:411` já faz. Incluir `shared/radio_diag.h` se ainda
      não estiver.
- [x] 3.2 Reflash da placa B (task 1.1 diz "não reiniciar o logger", mas o
      flash **exige** parar as seriais — é a exceção necessária; depois o
      enlace só precisa re-parear): parar o logger (`pgrep -f
      "[t]ools/logger/main.py" | xargs -r kill`), `.venv/bin/pio run -e
      placa_b -t upload`, subir o logger de novo. (Placa A não é reflashada
      aqui.) Fazer o teste 2 (sem firmware) **antes** deste reflash, para não
      perturbar a bancada entre a medição decisiva e sua análise.
- [x] 3.3 Ler a telemetria `@jammer;...e=regs;...` do boot e registrar
      `isPVariant` e `RF_SETUP` do(s) rádio(s) do jammer.

## 4. Emissão robusta para clone não-P (firmware — CONDICIONAL) — NÃO APLICÁVEL

> Só executar se 2.5 = "não irradia/fraco" **e** 3.3 reportar rádio não-P.
> Se 2.5 = "irradia", pular esta seção inteira e ir para a 5.
>
> **NÃO EXECUTADA (2026-09-25).** A condição não se cumpre: 2.5 deu "não
> irradia/fraco", MAS 3.3 reportou o rádio como **P-variant** (`p=1`). Num
> rádio P, `startConstCarrier` já executa o caminho que sustenta a portadora
> (payload dummy + `reUseTX`), então o fix de payload-flood desta seção não
> corrige nada. Ramo correto do `design.md`: "reporta P-variant → investigar
> antena/potência/dwell antes de mexer na emissão" (intervenção de hardware,
> não firmware). Ver seção de Resultado.

- [ ] 4.1 Em `jammer/src/node.h`, adicionar caminho de emissão por inundação
      de payload (ver `design.md`, "Detalhe do fix"): `setAutoAck(false)`,
      `setRetries(0,0)`, escrita contínua de payload fixo de 32 B sem esperar
      ACK no canal alvo, mantendo o rádio em TX. Selecionar CW (P) vs
      payload-flood (não-P) por `isPVariant()`.
- [ ] 4.2 Preservar intactos: comandos seriais (`mode`/`jam`/`power`) e
      eventos de telemetria (`jam_state_on/off`, `jam_mode_*`) — não quebrar
      `run_scenario.py`/`run_battery.py`.
- [ ] 4.3 `.venv/bin/pio run -e jammer -e placa_b` limpo.
- [ ] 4.4 Repetir a Tarefa 2 (teste de isolamento) com o novo firmware e
      confirmar que agora a faixa 94-125 sobe claramente com o jammer ON.

## 5. Fechamento

- [x] 5.1 Registrar o veredito final e os números no `openspec/ROADMAP.md`
      (substituir a pendência "aproximar placa A / jammer fraco" pela
      conclusão real) e atualizar a memória `project-rf-status`.
- [ ] 5.2 Se o jammer irradia (com ou sem o fix da Tarefa 4), a bateria A/B/C
      volta a ter sentido — atualizar `scenario-automation` 4.2/4.3 e
      `targeted-jammer` 3.2 com o achado. Se qualificar, copiar a sessão para
      `logs/working/` conforme `CLAUDE.md`.
- [ ] 5.3 `openspec validate jammer-emission-check --strict` e arquivar a
      change (`openspec archive jammer-emission-check`).

---

### Resultado (2026-09-25)

- **Sessão / fases**: teste sem firmware em `session-20260925-025055`, janelas
  em `logs/emission-check-20260925-062037-session-20260925-025055.json`.
  Enlace pareado + `mode fixed` (ch 10) nos dois lados; jammer `targeted` +
  `power max`. Fases de 120 s (133 varreduras cada).
- **Tabela média + fração por canal (banda de teste), OFF vs ON**:

  | canal | OFF média | ON média | OFF frac>0 | ON frac>0 |
  |------:|----------:|---------:|-----------:|----------:|
  |    94 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   100 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   105 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   110 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   115 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   120 |      0.00 |     0.00 |      0.000 |     0.000 |
  |   125 |      0.08 |     0.00 |      0.008 |     0.000 |

  Combinado (qualquer dos 7 ativo): **OFF=0.008, ON=0.000**.
  `loss_of_sync`: **0 no ON, 0 no OFF** (escravo co-locado imperturbado a
  `power max`).
- **Veredito**: **NÃO IRRADIA / fraco demais.** ON ≤ OFF na banda limpa e o
  receptor co-locado a centímetros do jammer não sofreu nenhuma perda de
  sincronismo — dois sinais independentes concordam (o co-locado contorna o
  confundidor conhecido "analisador não enxerga o jammer").
- **isPVariant do jammer**: **P-variant (`p=1`), 1 rádio** (`radios=1`).
  Boot: `@jammer;e=regs;why=boot;config=76;en_aa=00;en_rxaddr=03;rf_ch=4C;`
  `rf_setup=0B;...` e `@jammer;e=variant;p=1;p2=-1;radios=1`
  (`session-20260925-032610`).
- **Fix da Tarefa 4 aplicado?**: **NÃO** — não aplicável. O rádio é P, então
  `startConstCarrier` já usa o caminho de portadora sustentada; o fix de
  não-P não corrigiria a fraqueza observada. Causa remanescente é de
  hardware (antena/potência real do clone, ou clone que reporta P mas não
  implementa CONT_WAVE de fato) — exige SDR/analisador aproximado, não
  firmware.
