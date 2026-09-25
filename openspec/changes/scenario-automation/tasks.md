## 1. Firmware — modo canal fixo e métricas de latência/throughput

- [x] 1.1 Adicionar comando serial de modo (`mode hopping|fixed`) a
      `mestre/src/main.cpp` e `escravo/src/main.cpp`, análogo ao já usado
      em `targeted-jammer`; default `hopping` (comportamento atual
      preservado).
- [x] 1.2 Em modo `fixed`, usar um canal constante (`TEST_FIXED_CHANNEL =
      HOPPING_CHANNELS[0]`, `shared/hopping.h`) em vez de
      `get_next_channel`/`get_next_active_channel` em ambos os nós — ver
      design.md "Implementado (2026-09-24)" pela decisão de constante fixa
      em vez de configurável.
- [x] 1.3 No mestre, medir e logar latência (ms) por fragmento confirmado
      (`t_ack_recebido - t_envio`, evento `latency` em
      `tlm_printf("mestre","latency","ch=..;frag=..;ms=..")`).
- [x] 1.4 No mestre, agregar throughput (fragmentos confirmados/s) por
      janela deslizante (`THROUGHPUT_WINDOW_HOPS=50`, mesmo padrão de
      `plr-metric`) e logar (evento `throughput`).
      **Confirmado 2026-09-24**: `pio run` limpo em todos os 10 envs de
      firmware, `pio test -e native_test` 16/16. Validação em bancada
      (confirmar os números batendo com o esperado) fica pra seção 4,
      junto da bateria completa — precisa de placa_b conectada.

## 2. Ferramenta Python — captura multi-porta

- [x] 2.1/2.2 **Já satisfeito por infraestrutura existente, não por
      `tools/spectrum/main.py` como o design original previa.**
      `tools/logger/main.py` (de `telemetry-logging`, implementado depois
      deste design) já lê as portas de placa_a E placa_b simultaneamente
      (uma thread por porta) e mescla tudo por ordem de recepção real num
      único `logs/session-<ts>.jsonl` — nenhum parser novo nem
      `SerialReaderThread` adicional foi necessário. Os eventos `latency`/
      `throughput` da seção 1 já chegam nesse mesmo JSONL, parseados
      genericamente (`k=v` -> dict) sem exigir mudança no logger. Ver
      design.md, nota no topo do "Context".

## 3. Orquestração dos cenários

- [x] 3.1 Implementado como script CLI dedicado,
      `tools/validation/run_battery.py` (decisão: CLI, não `ScenarioTab` —
      mesmo caminho já usado por `run_scenario.py`, sem depender de GUI
      pra bancada não assistida). Sequencia A→B→C com duração
      baseline/jam/recovery configurável (`--baseline`/`--jam`/
      `--recovery`), mesmo modo/potência de jammer nos 3 (ver design.md,
      decisão de comparabilidade).
- [x] 3.2 Cada cenário envia `reset` (mestre e escravo, fronteira limpa) +
      `mode fixed|hopping` (+ `mitigation on|off` no mestre, pra distinguir
      B de C) via `logs/cmd.in`, e os comandos de jammer já existentes
      (`mode`/`power`/`jam on|off`) — tudo reaproveitando o mecanismo de
      `send_cmd()` de `run_scenario.py` (importado, não duplicado).
- [x] 3.3 Cada fase de cada cenário é registrada com o campo `"scenario":
      "A"/"B"/"C"` em `logs/battery-<ts>.json` — não um CSV separado por
      linha (não há CSV consolidado nesta arquitetura, ver seção 2: o
      rótulo vive nos metadados de fase, e `plot_battery.py` recorta o
      `session-<ts>.jsonl` por essas janelas de tempo).

## 4. Relatórios e validação final

- [x] 4.1 `tools/validation/plot_battery.py`: gráficos de throughput ×
      tempo e latência × tempo com os 3 cenários sobrepostos (cores
      distintas, eixo relativo ao início da baseline de cada cenário),
      mais um resumo textual (PLR/throughput/latência médios sob jamming
      por cenário). Testado com dados sintéticos (sem hardware) — ver
      commit; um bug real de fronteira encontrado e corrigido nesse teste
      (janela de tempo por cenário era inclusiva nos dois limites,
      deixando a primeira amostra do próximo cenário vazar pro anterior
      quando não há gap real entre eles).
- [ ] 4.2 Rodar a bateria completa uma vez de ponta a ponta em bancada real
      e revisar manualmente se os números batem com o esperado pelo guia
      (throughput/PLR/latência degradando de C -> B -> A sob o mesmo
      jamming) antes de considerar os dados prontos para a monografia.
      **Bloqueado**: precisa de placa_b (escravo+jammer) conectada — não
      disponível nesta sessão (só placa_a aparece em
      `/dev/serial/by-id`).
      **1ª rodada completa (2026-09-25 01:37-01:46,
      `logs/battery-20260925-041711.{json,png}`, jammer `targeted` PA_LOW,
      30/90/30 s)** — fase de jamming:

      | Cenário | miss % | frag/s | quedas |
      |---|---|---|---|
      | A (canal fixo 10) | 0,0 | 46,5 | 0 |
      | B (hopping, sem mitigação) | 16,7 | 14,2 | 1 |
      | C (hopping + mitigação) | 4,6 | 30,9 | 0 |

      C > B como esperado (mitigação reduz a perda de 16,7% para 4,6% e
      dobra o throughput; já no baseline a blacklist tira os canais do
      Wi-Fi: 4,7% vs 10,4%). **Mas A > C, o contrário do guia** — e o
      motivo é do *jammer*, não do sistema: o modo `targeted` percorre
      `HOPPING_CHANNELS` na ordem da tabela (60 ms por canal), e a
      sequência de salto também percorre a tabela na mesma ordem
      (`get_next_active_channel` = `(seed + counter) % ativos` — a seed só
      desloca a fase). Um jammer round-robin colide sistematicamente com um
      salto round-robin, enquanto o canal fixo só é atingido 1/16 do tempo
      e a retransmissão a cada 25 ms do mestre dentro da janela do hop
      absorve isso. Conclusão: `targeted` não é um atacante justo para o
      cenário A; repetir com o jammer reativo `adaptive` (ataca o canal
      que o enlace estiver usando, fixo ou não). **Achado de projeto
      registrado para a monografia**: a ordem de salto é previsível (só a
      fase depende da seed), então um jammer que siga a tabela na mesma
      ordem acompanha o enlace.
- [ ] 4.3 Exportar os gráficos finais (`logs/battery-<ts>.png`, já gerado
      por `plot_battery.py`) para uso na monografia. Depende de 4.2 rodar
      com dados reais primeiro.
