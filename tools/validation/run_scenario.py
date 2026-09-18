#!/usr/bin/env python3
"""
adaptive-rendezvous / dynamic-seed-blacklist: cenário de validação
fim-a-fim em bancada -- liga o jammer por uma janela, observa a queda de
PLR, a troca de seed/blacklist (dynamic-seed-blacklist) e, se o enlace
cair de vez, a recuperação via pareamento (fallback, adaptive-rendezvous),
tudo a partir do que tools/logger/main.py já grava em logs/.

Este script NÃO abre nenhuma porta serial -- só escreve comandos em
logs/cmd.in (o mesmo mecanismo do dashboard) e lê logs/state.json /
logs/latest.jsonl. tools/logger/main.py precisa estar rodando (dono
exclusivo das portas) antes de chamar isto.

Uso:
    python3 tools/logger/main.py &         # se ainda não estiver rodando
    python3 tools/validation/run_scenario.py \
        --baseline 20 --jam 25 --recovery 20

Ao final, escreve logs/scenario-<timestamp>.json (janelas de tempo de
cada fase + caminho do session file usado) e chama
tools/validation/plot_scenario.py automaticamente para gerar o resumo
textual e o PNG -- passe --no-plot para pular essa etapa.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")
CMD_PATH = os.path.join(LOGS_DIR, "cmd.in")
STATE_PATH = os.path.join(LOGS_DIR, "state.json")
LATEST_PATH = os.path.join(LOGS_DIR, "latest.jsonl")


def send_cmd(board, cmd):
    with open(CMD_PATH, "a") as f:
        f.write(f"{board} {cmd}\n")
    print(f"[scenario] -> {board}: {cmd}")


def read_state():
    try:
        with open(STATE_PATH) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def require_logger_running():
    if not os.path.exists(LATEST_PATH):
        print(
            "[scenario] ERRO: logs/latest.jsonl não existe -- rode "
            "'python3 tools/logger/main.py &' antes deste script.",
            file=sys.stderr,
        )
        sys.exit(1)
    state = read_state()
    nodes = state.get("nodes", {})
    stale = True
    now = time.time()
    for info in nodes.values():
        if now - info.get("last_seen_epoch", 0) < 15:
            stale = False
            break
    if not nodes or stale:
        print(
            "[scenario] AVISO: logs/state.json vazio ou sem eventos "
            "recentes (<15s) de nenhum nó -- confirme que as placas estão "
            "ligadas e o logger está de fato conectado às portas antes de "
            "confiar no resultado.",
            file=sys.stderr,
        )


def current_session_path():
    # latest.jsonl é symlink para logs/session-*.jsonl
    return os.path.realpath(LATEST_PATH)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--baseline", type=float, default=20.0,
        help="segundos de operação normal antes de ligar o jammer (default: 20)",
    )
    ap.add_argument(
        "--jam", type=float, default=25.0,
        help="segundos com o jammer ligado (default: 25)",
    )
    ap.add_argument(
        "--recovery", type=float, default=20.0,
        help="segundos observando recuperação após desligar o jammer (default: 20)",
    )
    ap.add_argument(
        "--jammer-board", default="placa_b",
        help="placa onde roda o jammer (default: placa_b)",
    )
    ap.add_argument(
        "--jammer-cmd", default="jam on",
        help="comando enviado para ligar o jammer (default: 'jam on')",
    )
    ap.add_argument(
        "--jammer-mode", choices=["sweep", "targeted"], default="sweep",
        help="modo do jammer, enviado como 'mode <x>' antes de ligar "
        "(default: sweep). IMPORTANTE: 'sweep' varre só 2-80 MHz, que "
        "cobre apenas 8 dos 16 canais de HOPPING_CHANNELS -- alvo_avg "
        "(media sobre os 16) tem teto estrutural de ~50%%, abaixo do "
        "LIMIAR_ALVO=70%% do analisador, entao o alerta NUNCA dispara em "
        "sweep não importa a potência/tempo. Use 'targeted' (cobre os 16 "
        "canais direto) para de fato disparar alert_on/rekey.",
    )
    ap.add_argument(
        "--jammer-power", choices=["min", "low", "high", "max"], default=None,
        help="se dado, envia 'power <nível>' antes de 'jam on' -- default "
        "da placa combinada é LOW (ver docs/pinout.md); PA_LOW pode ser "
        "fraco demais pra disparar o alerta do analisador nesta bancada",
    )
    ap.add_argument(
        "--no-plot", action="store_true",
        help="não gerar o gráfico/resumo ao final (só roda o cenário)",
    )
    args = ap.parse_args()

    require_logger_running()

    print(f"[scenario] Sessão ativa: {current_session_path()}")
    print("[scenario] Garantindo jammer desligado antes da baseline...")
    send_cmd(args.jammer_board, "jam off")
    time.sleep(1)

    t0 = time.time()
    phases = [{"name": "baseline", "start_epoch": t0}]

    print(f"[scenario] Fase 'baseline': {args.baseline:.0f}s (jammer desligado)")
    time.sleep(args.baseline)

    print(f"[scenario] Fase 'jamming': ligando jammer (mode={args.jammer_mode}, {args.jammer_cmd!r}), {args.jam:.0f}s")
    send_cmd(args.jammer_board, f"mode {args.jammer_mode}")
    time.sleep(0.2)
    if args.jammer_power:
        send_cmd(args.jammer_board, f"power {args.jammer_power}")
        time.sleep(0.5)
    send_cmd(args.jammer_board, args.jammer_cmd)
    phases.append({"name": "jamming", "start_epoch": time.time()})
    time.sleep(args.jam)

    print(f"[scenario] Fase 'recovery': desligando jammer, {args.recovery:.0f}s")
    send_cmd(args.jammer_board, "jam off")
    phases.append({"name": "recovery", "start_epoch": time.time()})
    time.sleep(args.recovery)

    end_epoch = time.time()
    phases.append({"name": "end", "start_epoch": end_epoch})

    scenario = {
        "started_at": datetime.fromtimestamp(t0, tz=timezone.utc).isoformat(),
        "ended_at": datetime.fromtimestamp(end_epoch, tz=timezone.utc).isoformat(),
        "session_file": current_session_path(),
        "jammer_board": args.jammer_board,
        "phases": phases,
    }

    ts = datetime.fromtimestamp(t0, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(LOGS_DIR, f"scenario-{ts}.json")
    with open(out_path, "w") as f:
        json.dump(scenario, f, indent=2)
    print(f"[scenario] Metadados salvos em {out_path}")

    if not args.no_plot:
        plot_script = os.path.join(os.path.dirname(__file__), "plot_scenario.py")
        subprocess.run([sys.executable, plot_script, out_path], check=True)


if __name__ == "__main__":
    main()
