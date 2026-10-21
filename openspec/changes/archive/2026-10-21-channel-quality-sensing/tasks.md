## 1. Calibração (offline, antes do firmware)

- [x] 1.1 Script em `tools/validation/` cruzando ocupação por canal
      (`spectrum.csv`) com miss% do mestre por canal, por sessão.
- [ ] 1.2 Coleta dedicada: Wi-Fi ambiente sem jammer (10 min) + jammer
      `hold` em 3 canais conhecidos da tabela, enlace ativo.
- [x] 1.3 Escolher `W`, `T_on`, `T_off`; medir a ocupação causada pelo
      próprio mestre (D3). Registrar no `design.md`.

## 2. Lógica pura

- [x] 2.1 Ocupação por janela + histerese por canal, como lógica pura em
      `shared/` + testes `native_test`.
- [x] 2.2 Fusão (falha do enlace OR degradado do analisador) sobre
      `update_channel_mask()` mantendo as guardas + testes.
- [x] 2.3 Gatilho de reseed em `shared/control_policy.h` + testes.

## 3. Firmware

- [x] 3.1 Analisador: ocupação + histerese; emitir `e=chq;mask=..;top=..`;
      entregar a máscara pelo callback da placa combinada (booleano no fio
      de GPIO da topologia de 4 placas).
- [x] 3.2 Analisador: tirar o classificador CFAR/votos/streak do caminho de
      decisão (manter como telemetria até a validação).
- [x] 3.3 Mestre: receber a máscara e fundir com `g_channel_fail`; aplicar
      a política de reseed nova.
- [x] 3.4 Atualizar `run_scenario.py`/`run_battery.py` e o logger para o
      evento `chq`.

## 4. Validação em bancada

- [x] 4.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [x] 4.2 Wi-Fi ambiente, sem jammer, 10 min: canais acima de `T_on`
      marcados de forma estável (sem oscilar), enlace sem queda.
- [x] 4.3 `hold` ch10 `PA_MAX` com mitigação: ch10 marcado em poucos
      segundos, blacklist aplicada **nos dois lados**, miss do enlace volta
      ao patamar sem jammer.
- [x] 4.4 `jam off`: ch10 volta à máscara depois do cooldown.
- [ ] 4.5 Se qualificar, copiar sessões para `logs/working/`.

## 5. Fechamento

- [ ] 5.1 Marcar `spectrum-attack-classification` 3.1/3.3 como superadas
      por esta change.
- [ ] 5.2 `openspec validate channel-quality-sensing --strict` e arquivar.

## Resultado da validação de bancada 2026-10-21 (session-20261021-140517)

Cadeia FUNCIONA e sem regressão: analisador sinaliza o canal travado
(ch10 +2,9 s; ch110 +2,6 s — detecção acima de RF 80 confirmada), limpa ~8 s
após `jam off`; mestre funde e aplica a blacklist nos DOIS lados; enlace nunca
caiu; 0 falso positivo de Wi-Fi no baseline (ch34 ocupação máx 26 < 30, máscara
0x0000); 0 `half_open` (#1 ok), err de fase ±1 ms (#2 ok), 0 reseed espúrio.

ACHADO HONESTO (proativo NÃO venceu o reativo): nos dois ataques a blacklist
por falha de ack do mestre excluiu o canal ~1 s ANTES do flag de ocupação
(ch10 +1,9 vs +2,9; ch110 +1,2 vs +2,6). Como o enlace salta pelos 16 canais a
cada ~0,5 s, ele descobre o jammer forte reativamente quase na hora — mais
rápido que a janela de ocupação de 9 s. Logo a premissa de "detecção proativa
mais rápida" não se sustenta para ataque forte de canal único.

Valor REAL entregue pelo #3: (1) conserta o alerta antes travado pelo Wi-Fi
(agora alerta por canal, agnóstico à causa, 0 falso positivo a T_on=30);
(2) observabilidade (`chq`); (3) confirma a fusão pelos dois lados — no
baseline o reativo baniu o ch34 (Wi-Fi) que a ocupação NÃO pega, e a ocupação
pega o jammer forte limpo; fontes complementares, como a calibração previu.

NÃO EXERCITADO: os dois braços de reseed (≥6 canais degradados; miss alto
sustentado pós-blacklist) nunca dispararam — os ataques disponíveis não ocupam
6 canais ao mesmo tempo. Fica como limitação, não como validado.
