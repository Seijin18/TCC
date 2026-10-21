## 1. Calibração (offline, antes do firmware)

- [x] 1.1 Script em `tools/validation/` cruzando ocupação por canal
      (`spectrum.csv`) com miss% do mestre por canal, por sessão.
- [ ] 1.2 Coleta dedicada: Wi-Fi ambiente sem jammer (10 min) + jammer
      `hold` em 3 canais conhecidos da tabela, enlace ativo.
- [x] 1.3 Escolher `W`, `T_on`, `T_off`; medir a ocupação causada pelo
      próprio mestre (D3). Registrar no `design.md`.

## 2. Lógica pura

- [ ] 2.1 Ocupação por janela + histerese por canal, como lógica pura em
      `shared/` + testes `native_test`.
- [ ] 2.2 Fusão (falha do enlace OR degradado do analisador) sobre
      `update_channel_mask()` mantendo as guardas + testes.
- [ ] 2.3 Gatilho de reseed em `shared/control_policy.h` + testes.

## 3. Firmware

- [ ] 3.1 Analisador: ocupação + histerese; emitir `e=chq;mask=..;top=..`;
      entregar a máscara pelo callback da placa combinada (booleano no fio
      de GPIO da topologia de 4 placas).
- [ ] 3.2 Analisador: tirar o classificador CFAR/votos/streak do caminho de
      decisão (manter como telemetria até a validação).
- [ ] 3.3 Mestre: receber a máscara e fundir com `g_channel_fail`; aplicar
      a política de reseed nova.
- [ ] 3.4 Atualizar `run_scenario.py`/`run_battery.py` e o logger para o
      evento `chq`.

## 4. Validação em bancada

- [ ] 4.1 `pio test -e native_test` + `pio run` todos os envs; gravar.
- [ ] 4.2 Wi-Fi ambiente, sem jammer, 10 min: canais acima de `T_on`
      marcados de forma estável (sem oscilar), enlace sem queda.
- [ ] 4.3 `hold` ch10 `PA_MAX` com mitigação: ch10 marcado em poucos
      segundos, blacklist aplicada **nos dois lados**, miss do enlace volta
      ao patamar sem jammer.
- [ ] 4.4 `jam off`: ch10 volta à máscara depois do cooldown.
- [ ] 4.5 Se qualificar, copiar sessões para `logs/working/`.

## 5. Fechamento

- [ ] 5.1 Marcar `spectrum-attack-classification` 3.1/3.3 como superadas
      por esta change.
- [ ] 5.2 `openspec validate channel-quality-sensing --strict` e arquivar.
