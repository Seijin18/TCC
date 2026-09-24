# TCC — instruções do projeto

## Logs de bancada que mostram o sistema funcionando

Sessões de bancada (`logs/session-*.jsonl`, gravadas por `tools/logger`) em
que o comportamento esperado foi **verificado nos dados** são copiadas para
`logs/working/` e indexadas em `logs/working/README.md` (sessão, duração do
enlace contínuo, `msg_done`, cenário associado, o que ela demonstra).

- Ao terminar uma sessão de bancada, verifique os números antes de
  classificar (pareamento + seed nos dois lados, ≥ 60 s de enlace contínuo
  com tráfego real, sem `loss_of_sync` no trecho ou com recuperação
  automática confirmada; para mitigação: alerta → blacklist/rekey aplicado
  nos **dois** lados **e** o enlace sobrevivendo/recuperando).
- Se qualificar, **copie** (não mova — o original fica em `logs/`) a sessão
  e os `scenario-*`/`repair-*`/`battery-*` associados para `logs/working/`
  e acrescente a linha no `README.md` de lá.
- Sessões interessantes mas que não demonstram funcionamento correto ficam
  só em `logs/`, citadas na seção "Referência" do README.
- `logs/` é ignorado pelo git; só `logs/working/README.md` é versionado.

## Fluxo

- Planejamento em OpenSpec (`openspec/`) — ver `openspec/project.md` e
  `openspec/ROADMAP.md`.
- Build/testes: `.venv/bin/pio run` (todos os envs), `.venv/bin/pio test -e
  native_test`. Parar o logger (`pkill -f tools/logger`) antes de gravar
  firmware — ele segura as portas seriais.
