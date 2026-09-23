#!/usr/bin/env python3
"""
spectrum-attack-classification: visualiza o MECANISMO do detector por
canal (updateAdaptiveBaseline() em analisador/src/node.h) sobre dados
reais de uma sessao -- nao so o resultado (chmap/alert_on), mas o
"porque": leitura bruta vs. baseline aprendida (EWMA) vs. limiar
(baseline + k*mad), por canal, com a regiao onde o canal acumulou votos
suficientes (>= ADAPTIVE_VOTES em ADAPTIVE_WINDOW sweeps) sombreada.

Reusa PerChannelDetector de adaptive_baseline_sim.py (mesma logica,
mesmos parametros do firmware) para nao duplicar/divergir da simulacao
ja validada.

Uso:
    python3 tools/validation/plot_detector_mechanism.py [scenario.json] [--channels 10,22]
    (sem args: usa o scenario mais recente e os canais do chmap final)
"""
import argparse
import glob
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(__file__))
from adaptive_baseline_sim import HOPPING_CHANNELS, PerChannelDetector, load_sequence

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")

# Mesmos valores de analisador/src/node.h (ADAPTIVE_*)
ALPHA = 0.1
K = 3.0
WINDOW = 60
VOTES = 5
MIN_ABS_DEV = 10.0


def latest_scenario_file():
    files = sorted(glob.glob(os.path.join(LOGS_DIR, "scenario-*.json")))
    if not files:
        print("Nenhum logs/scenario-*.json encontrado.", file=sys.stderr)
        sys.exit(1)
    return files[-1]


def find_flagged_channels(scenario_path):
    with open(scenario_path) as f:
        scenario = json.load(f)
    session_path = scenario["session_file"]
    mask = 0
    try:
        f = open(session_path)
    except FileNotFoundError:
        return []
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("event") == "chmap":
                fields = d.get("fields") or d.get("f") or {}
                m = fields.get("mask")
                if m:
                    mask = max(mask, int(m, 16))
    return [HOPPING_CHANNELS[i] for i in range(16) if mask & (1 << i)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", nargs="?", default=None)
    ap.add_argument("--channels", default=None, help="ex: 10,22")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    scenario_path = args.scenario or latest_scenario_file()
    with open(scenario_path) as f:
        scenario = json.load(f)
    phases = scenario["phases"]
    t0 = phases[0]["start_epoch"]
    jam_start = phases[1]["start_epoch"]
    jam_end = phases[2]["start_epoch"]

    if args.channels:
        channels = [int(x) for x in args.channels.split(",")]
    else:
        channels = find_flagged_channels(scenario_path)
    if not channels:
        print("Nenhum canal marcado (chmap sempre 0x0000) -- nada pra visualizar em detalhe.", file=sys.stderr)
        sys.exit(1)
    channels = channels[:2]  # no maximo 2 paineis, pra nao poluir

    seq, _ = load_sequence(scenario_path)
    if not seq:
        print("Sessao sem dados CSV brutos no periodo do cenario.", file=sys.stderr)
        sys.exit(1)

    det = PerChannelDetector(alpha=ALPHA, k=K, window=WINDOW, votes=VOTES, min_abs_dev=MIN_ABS_DEV)
    t_rel = []
    raw = {ch: [] for ch in channels}
    baseline = {ch: [] for ch in channels}
    threshold = {ch: [] for ch in channels}
    flagged = {ch: [] for ch in channels}

    for ep, phase, vals in seq:
        det.update(vals)
        t_rel.append(ep - t0)
        for ch in channels:
            raw[ch].append(vals[ch])
            baseline[ch].append(det.baseline[ch])
            threshold[ch].append(det.baseline[ch] + K * det.mad[ch])
            flagged[ch].append(det.flagged[ch])

    fig, axes = plt.subplots(len(channels), 1, figsize=(11, 3.2 * len(channels)), sharex=True)
    if len(channels) == 1:
        axes = [axes]

    for ax, ch in zip(axes, channels):
        ax.axvspan(jam_start - t0, jam_end - t0, color="red", alpha=0.08, label="jammer ativo")
        ax.plot(t_rel, raw[ch], color="#888", linewidth=0.6, label="leitura bruta (RSSI/atividade)")
        ax.plot(t_rel, baseline[ch], color="#1f77b4", linewidth=1.6, label="baseline aprendida (EWMA)")
        ax.plot(t_rel, threshold[ch], color="#d62728", linewidth=1.2, linestyle="--", label=f"limiar (baseline + {K:.0f}×mad)")

        # sombreia onde o canal esta "comprometido" (votos >= ADAPTIVE_VOTES)
        in_flag = False
        start = None
        for i, f in enumerate(flagged[ch]):
            if f and not in_flag:
                start = t_rel[i]
                in_flag = True
            elif not f and in_flag:
                ax.axvspan(start, t_rel[i], color="#d62728", alpha=0.18)
                in_flag = False
        if in_flag:
            ax.axvspan(start, t_rel[-1], color="#d62728", alpha=0.18)

        ax.set_ylabel("atividade do canal")
        ax.set_title(f"Canal {ch} (índice HOPPING_CHANNELS)")
        ax.grid(alpha=0.25)

    axes[0].legend(loc="upper left", fontsize=8, ncol=2)
    axes[-1].set_xlabel("tempo (s, desde o início da sessão)")
    fig.suptitle(
        "Mecanismo do detector CFAR por canal: leitura vs. baseline própria vs. limiar\n"
        "(região vermelha sólida = canal votado \"comprometido\", ativa o alert_on/chmap)",
        fontsize=10,
    )
    fig.tight_layout(rect=[0, 0, 1, 0.94])

    out = args.out or os.path.join(LOGS_DIR, f"detector-mechanism-{os.path.basename(scenario_path).replace('scenario-', '').replace('.json', '')}.png")
    fig.savefig(out, dpi=140)
    print(f"Salvo em {out}")


if __name__ == "__main__":
    main()
