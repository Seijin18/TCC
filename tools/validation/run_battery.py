#!/usr/bin/env python3
"""
scenario-automation: orquestra a bateria de 3 cenários comparativos do
guia (A: sem hopping · B: hopping fixo, sem self-healing · C: sistema
completo adaptativo), todos contra o MESMO tratamento de jamming (mesma
duração/modo/potência), pra isolar o efeito de cada camada de defesa.
Depende de tools/logger/main.py já rodando (dono das portas) -- este
script só escreve comandos em logs/cmd.in e lê logs/state.json, igual
tools/validation/run_scenario.py (que também dá a base de fases
baseline->jamming->recovery reaproveitada aqui, por cenário).

Decisão de design (openspec/changes/scenario-automation/design.md): os 3
cenários usam o MESMO padrão baseline->jamming->recovery e o MESMO modo de
jammer (default: targeted, o único que estruturalmente consegue disparar
o alerta do analisador -- ver aviso de --jammer-mode em run_scenario.py) e
a MESMA potência, variando só o MODO DE FIRMWARE do mestre/escravo:

    A: "mode fixed"    (placa_a e placa_b) -- sem hopping, sem adaptação
    B: "mode hopping" + "mitigation off" (placa_a), "mode hopping" (placa_b)
    C: "mode hopping" + "mitigation on"  (placa_a, default)

Cada cenário começa com "reset" (mestre e escravo) para não herdar
blacklist/pareamento do cenário anterior na mesma sessão contínua.

Uso:
    python3 tools/logger/main.py &         # se ainda não estiver rodando
    python3 tools/validation/run_battery.py \
        --baseline 20 --jam 60 --recovery 20 \
        --jammer-mode targeted --jammer-power high

Ao final, escreve logs/battery-<timestamp>.json (fases de todos os 3
cenários, cada uma marcada com "scenario": "A"/"B"/"C") e chama
tools/validation/plot_battery.py automaticamente -- passe --no-plot para
pular essa etapa.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(__file__))
from run_scenario import current_session_path, read_state, require_logger_running, send_cmd  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")

MESTRE_BOARD = "placa_a"
ESCRAVO_BOARD = "placa_b"  # mesma placa do jammer -- ver jammer_set_unknown_cmd_sink()

SCENARIOS = ["A", "B", "C"]
SCENARIO_LABELS = {
    "A": "sem hopping (canal fixo, nenhuma adaptação)",
    "B": "hopping fixo (FHSS normal, self-healing desligado)",
    "C": "sistema completo (FHSS + detecção + rekey/blacklist)",
}


def configure_scenario(scenario):
    print(f"[battery] Configurando Cenário {scenario}: {SCENARIO_LABELS[scenario]}")
    # Fronteira limpa: zera blacklist/pareamento antes de cada cenário,
    # não herda estado do anterior na mesma sessão contínua.
    send_cmd(MESTRE_BOARD, "reset")
    send_cmd(ESCRAVO_BOARD, "reset")
    time.sleep(1.0)

    if scenario == "A":
        send_cmd(MESTRE_BOARD, "mode fixed")
        send_cmd(ESCRAVO_BOARD, "mode fixed")
    elif scenario == "B":
        send_cmd(MESTRE_BOARD, "mode hopping")
        send_cmd(ESCRAVO_BOARD, "mode hopping")
        time.sleep(0.2)
        send_cmd(MESTRE_BOARD, "mitigation off")
    else:  # C
        send_cmd(MESTRE_BOARD, "mode hopping")
        send_cmd(ESCRAVO_BOARD, "mode hopping")
        time.sleep(0.2)
        send_cmd(MESTRE_BOARD, "mitigation on")

    # Janela pro re-pareamento (forçado pelo "reset" acima) completar antes
    # de começar a contar a fase de baseline deste cenário.
    time.sleep(3.0)


def run_one_scenario(scenario, args):
    configure_scenario(scenario)

    phases = []
    print(f"[battery] Garantindo jammer desligado antes da baseline (Cenário {scenario})...")
    send_cmd(args.jammer_board, "jam off")
    time.sleep(1)

    phases.append({"scenario": scenario, "name": "baseline", "start_epoch": time.time()})
    print(f"[battery] Cenário {scenario} / fase 'baseline': {args.baseline:.0f}s (jammer desligado)")
    time.sleep(args.baseline)

    print(f"[battery] Cenário {scenario} / fase 'jamming': mode={args.jammer_mode}, {args.jam:.0f}s")
    # jamming-mitigation-hardening: modo "hold" = portadora parada num canal
    # fixo (atacante confiável, ver jammer-emission-hardware). Precisa do canal
    # no comando; default = TEST_FIXED_CHANNEL (10), que é exatamente o canal do
    # enlace no Cenário A e um dos 16 de hopping em B/C -- mesmo atacante nos 3.
    if args.jammer_mode == "hold":
        send_cmd(args.jammer_board, f"mode hold {args.jammer_hold_ch}")
    else:
        send_cmd(args.jammer_board, f"mode {args.jammer_mode}")
    time.sleep(0.2)
    if args.jammer_power:
        send_cmd(args.jammer_board, f"power {args.jammer_power}")
        time.sleep(0.5)
    send_cmd(args.jammer_board, args.jammer_cmd)
    phases.append({"scenario": scenario, "name": "jamming", "start_epoch": time.time()})
    time.sleep(args.jam)

    print(f"[battery] Cenário {scenario} / fase 'recovery': {args.recovery:.0f}s")
    send_cmd(args.jammer_board, "jam off")
    phases.append({"scenario": scenario, "name": "recovery", "start_epoch": time.time()})
    time.sleep(args.recovery)

    phases.append({"scenario": scenario, "name": "end", "start_epoch": time.time()})
    return phases


def restore_default_mode():
    # Higiene: não deixar a bancada presa num modo de teste degradado
    # depois que o script termina.
    print("[battery] Restaurando modo default (hopping + mitigation on) em ambas as placas...")
    send_cmd(MESTRE_BOARD, "mode hopping")
    send_cmd(ESCRAVO_BOARD, "mode hopping")
    send_cmd(MESTRE_BOARD, "mitigation on")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", type=float, default=20.0, help="segundos de baseline por cenário (default: 20)")
    ap.add_argument("--jam", type=float, default=60.0, help="segundos de jamming por cenário (default: 60)")
    ap.add_argument("--recovery", type=float, default=20.0, help="segundos de recovery por cenário (default: 20)")
    ap.add_argument("--jammer-board", default="placa_b")
    ap.add_argument("--jammer-cmd", default="jam on")
    ap.add_argument(
        "--jammer-mode", choices=["sweep", "targeted", "adaptive", "hold"], default="targeted",
        help="default 'targeted'. 'hold' = portadora parada num canal fixo "
        "(--jammer-hold-ch, default 10) -- o atacante forte/confiável de "
        "jammer-emission-hardware, mesmo canal do enlace no Cenário A. 'sweep' não "
        "cobre os 16 canais; 'adaptive' é o follower reativo.",
    )
    ap.add_argument("--jammer-hold-ch", type=int, default=10,
                    help="canal RF do modo 'hold' (default 10 = TEST_FIXED_CHANNEL)")
    ap.add_argument("--jammer-power", choices=["min", "low", "high", "max"], default="high")
    ap.add_argument(
        "--scenarios", default="ABC",
        help="subconjunto/ordem de cenários a rodar, ex. 'AC' para pular B (default: ABC)",
    )
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    scenarios = [c for c in args.scenarios.upper() if c in SCENARIOS]
    if not scenarios:
        print(f"[battery] ERRO: --scenarios deve conter algum de {SCENARIOS}", file=sys.stderr)
        sys.exit(1)

    require_logger_running()
    print(f"[battery] Sessão ativa: {current_session_path()}")
    print(f"[battery] Bateria: {scenarios}, jammer mode={args.jammer_mode} power={args.jammer_power}")

    # pairing-handshake-hardening (tarefa 0.2): jam off é o PRIMEIRO comando
    # da execução inteira, antes de qualquer "reset" (que configure_scenario()
    # manda logo no início de cada cenário) -- um "jam on"+"mode adaptive" de
    # um teste anterior deixado armado (ver openspec/changes/
    # pairing-handshake-hardening/tasks.md 0.1) não deve sobreviver nem um
    # instante na sessão seguinte.
    print("[battery] Garantindo jammer desligado antes de qualquer reset...")
    send_cmd(args.jammer_board, "jam off")
    time.sleep(1)

    t0 = time.time()
    all_phases = []
    try:
        for scenario in scenarios:
            all_phases.extend(run_one_scenario(scenario, args))
    finally:
        restore_default_mode()

    end_epoch = time.time()
    battery = {
        "started_at": datetime.fromtimestamp(t0, tz=timezone.utc).isoformat(),
        "ended_at": datetime.fromtimestamp(end_epoch, tz=timezone.utc).isoformat(),
        "session_file": current_session_path(),
        "jammer_board": args.jammer_board,
        "jammer_mode": args.jammer_mode,
        "jammer_power": args.jammer_power,
        "scenarios": scenarios,
        "scenario_labels": SCENARIO_LABELS,
        "phases": all_phases,
    }

    ts = datetime.fromtimestamp(t0, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(LOGS_DIR, f"battery-{ts}.json")
    with open(out_path, "w") as f:
        json.dump(battery, f, indent=2, ensure_ascii=False)
    print(f"[battery] Metadados salvos em {out_path}")

    if not args.no_plot:
        plot_script = os.path.join(os.path.dirname(__file__), "plot_battery.py")
        subprocess.run([sys.executable, plot_script, out_path], check=True)


if __name__ == "__main__":
    main()
