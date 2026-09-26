#!/usr/bin/env python3
"""
time-slotted-hopping: comparação DETALHADA de duas baterias A/B/C (ex.:
event-driven vs time-slotted). Para cada cenário (A/B/C) e fase (baseline sem
jammer / jamming com jammer), agrega métricas do lado do mestre (placa_a) e
plota, por métrica, barras agrupadas: cenário no eixo x, 4 barras por cenário
(variante A baseline/jamming, variante B baseline/jamming).

Métricas:
  - throughput  (msg_done/s)  -- mensagens completas entregues
  - acks/s                    -- FragmentAck recebidos (nível de fragmento)
  - misses/s                  -- slots/hops sem ack
  - loss_of_sync (contagem)   -- quantas vezes o enlace caiu e re-pareou
  - latência de ack (mediana, ms)

Uso:
    python3 tools/validation/plot_compare_metrics.py \
        --a logs/battery-EV.json --a-label "Event-driven" \
        --b logs/battery-TD.json --b-label "Time-slotted" \
        [--out logs/compare-metrics.png]
"""
import argparse
import json
import os
import sys
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")
SCEN = ["A", "B", "C"]
PHASES = ["baseline", "jamming"]


def load(bpath):
    b = json.load(open(bpath))
    sess = b["session_file"]
    if not os.path.exists(sess):
        sess = os.path.join(LOGS_DIR, os.path.basename(sess))
    ph = b["phases"]
    for i, p in enumerate(ph):
        p["end"] = ph[i + 1]["start_epoch"] if i + 1 < len(ph) else p["start_epoch"] + 100
    # windows[(scenario,phase)] = (start,end,dur)
    win = {}
    for p in ph:
        win[(p["scenario"], p["name"])] = (p["start_epoch"], p["end"], p["end"] - p["start_epoch"])
    # aggregate per window
    agg = {k: {"msg_done": 0, "ack": 0, "miss": 0, "loss": 0, "lat": []} for k in win}
    if os.path.exists(sess):
        for line in open(sess):
            try:
                o = json.loads(line)
            except Exception:
                continue
            ep = o.get("epoch")
            e = o.get("event")
            bd = o.get("board")
            if ep is None or e is None:
                continue
            for k, (ws, we, _) in win.items():
                if ws <= ep < we:
                    if e == "loss_of_sync":
                        agg[k]["loss"] += 1
                    if bd == "placa_a":
                        f = o.get("fields") or {}
                        if e == "msg_done":
                            agg[k]["msg_done"] += 1
                        elif e == "ack":
                            agg[k]["ack"] += 1
                            # time-slotted: ms embutido no ack.
                            if "ms" in f:
                                try:
                                    agg[k]["lat"].append(int(f["ms"]))
                                except Exception:
                                    pass
                        elif e == "latency":
                            # event-driven: latência num evento separado.
                            if "ms" in f:
                                try:
                                    agg[k]["lat"].append(int(f["ms"]))
                                except Exception:
                                    pass
                        elif e == "miss":
                            agg[k]["miss"] += 1
                    break
    return win, agg


def rate(agg, win, scen, phase, field):
    k = (scen, phase)
    if k not in agg:
        return 0.0
    dur = win[k][2] or 1
    return agg[k][field] / dur


def med_lat(agg, scen, phase):
    k = (scen, phase)
    xs = agg.get(k, {}).get("lat", [])
    return statistics.median(xs) if xs else 0.0


def grouped_bars(ax, title, ylabel, valfun, la, lb, aggA, winA, aggB, winB, fmt="{:.1f}"):
    """4 barras por cenário: A-base, A-jam, B-base, B-jam."""
    x = np.arange(len(SCEN))
    w = 0.2
    # cores: variante A = laranja, B = verde; baseline mais claro, jamming mais forte
    colA_base, colA_jam = "#ffce9e", "#ff7f0e"
    colB_base, colB_jam = "#a9dfa9", "#2ca02c"
    series = [
        (-1.5 * w, colA_base, f"{la} baseline", "baseline", valfun, aggA, winA),
        (-0.5 * w, colA_jam, f"{la} jamming", "jamming", valfun, aggA, winA),
        (0.5 * w, colB_base, f"{lb} baseline", "baseline", valfun, aggB, winB),
        (1.5 * w, colB_jam, f"{lb} jamming", "jamming", valfun, aggB, winB),
    ]
    for off, col, lab, phase, vf, agg, win in series:
        vals = [vf(agg, win, s, phase) for s in SCEN]
        ax.bar(x + off, vals, w, color=col, label=lab)
        for i, v in enumerate(vals):
            if v > 0:
                ax.text(i + off, v, fmt.format(v), ha="center", va="bottom", fontsize=7, rotation=0)
    ax.set_xticks(x)
    ax.set_xticklabels(["A: sem hopping", "B: FHSS s/ mitig.", "C: mitigação"])
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--a-label", default="A")
    ap.add_argument("--b", required=True)
    ap.add_argument("--b-label", default="B")
    ap.add_argument("--out", default=os.path.join(LOGS_DIR, "compare-metrics.png"))
    args = ap.parse_args()

    winA, aggA = load(args.a)
    winB, aggB = load(args.b)
    la, lb = args.a_label, args.b_label

    fig, axes = plt.subplots(3, 2, figsize=(15, 14))

    # throughput
    grouped_bars(axes[0][0], "Throughput (mensagens completas/s)", "msg/s",
                 lambda a, w, s, p: rate(a, w, s, p, "msg_done"), la, lb, aggA, winA, aggB, winB)
    # acks/s
    grouped_bars(axes[0][1], "ACKs recebidos por segundo (nível de fragmento)", "acks/s",
                 lambda a, w, s, p: rate(a, w, s, p, "ack"), la, lb, aggA, winA, aggB, winB)
    # misses/s
    grouped_bars(axes[1][0], "Misses por segundo (slots/hops sem ack)", "miss/s",
                 lambda a, w, s, p: rate(a, w, s, p, "miss"), la, lb, aggA, winA, aggB, winB)
    # ack latency median
    grouped_bars(axes[1][1], "Latência mediana de ACK (ms) — menor é melhor", "ms",
                 lambda a, w, s, p: med_lat(a, s, p), la, lb, aggA, winA, aggB, winB)

    # loss_of_sync: contagem absoluta por fase (não taxa) -- 2 barras/cenário/fase
    ax = axes[2][0]
    x = np.arange(len(SCEN))
    w = 0.2
    for off, col, lab, phase, agg in [
        (-1.5 * w, "#ffce9e", f"{la} baseline", "baseline", aggA),
        (-0.5 * w, "#ff7f0e", f"{la} jamming", "jamming", aggA),
        (0.5 * w, "#a9dfa9", f"{lb} baseline", "baseline", aggB),
        (1.5 * w, "#2ca02c", f"{lb} jamming", "jamming", aggB),
    ]:
        vals = [agg.get((s, phase), {}).get("loss", 0) for s in SCEN]
        ax.bar(x + off, vals, w, color=col, label=lab)
        for i, v in enumerate(vals):
            if v > 0:
                ax.text(i + off, v, str(v), ha="center", va="bottom", fontsize=7)
    ax.set_xticks(x)
    ax.set_xticklabels(["A", "B", "C"])
    ax.set_ylabel("nº de quedas")
    ax.set_title("loss_of_sync (quedas de enlace + re-pareamento) — menor é melhor")
    ax.grid(axis="y", alpha=0.3)

    # legenda compartilhada + tabela textual no último painel
    axes[2][1].axis("off")
    handles, labels = axes[0][0].get_legend_handles_labels()
    axes[2][1].legend(handles, labels, loc="upper center", fontsize=11, title="variante / fase")

    fig.suptitle(f"Comparação por cenário e fase: {la} vs {lb}  (hold ch10 PA_MAX)", fontsize=14)
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.savefig(args.out, dpi=110)
    print(f"salvo: {args.out}")

    # resumo textual
    def row(metric, vf):
        print(f"\n{metric}")
        print(f"  {'cenário':<6}{la+' base':>14}{la+' jam':>13}{lb+' base':>15}{lb+' jam':>13}")
        for s in SCEN:
            print(f"  {s:<6}{vf(aggA,winA,s,'baseline'):>14.2f}{vf(aggA,winA,s,'jamming'):>13.2f}"
                  f"{vf(aggB,winB,s,'baseline'):>15.2f}{vf(aggB,winB,s,'jamming'):>13.2f}")
    row("throughput (msg/s)", lambda a, w, s, p: rate(a, w, s, p, "msg_done"))
    row("acks/s", lambda a, w, s, p: rate(a, w, s, p, "ack"))
    row("miss/s", lambda a, w, s, p: rate(a, w, s, p, "miss"))
    row("latência ack mediana (ms)", lambda a, w, s, p: med_lat(a, s, p))
    print("\nloss_of_sync (contagem)")
    print(f"  {'cenário':<6}{la+' base':>14}{la+' jam':>13}{lb+' base':>15}{lb+' jam':>13}")
    for s in SCEN:
        print(f"  {s:<6}{aggA.get((s,'baseline'),{}).get('loss',0):>14}{aggA.get((s,'jamming'),{}).get('loss',0):>13}"
              f"{aggB.get((s,'baseline'),{}).get('loss',0):>15}{aggB.get((s,'jamming'),{}).get('loss',0):>13}")


if __name__ == "__main__":
    main()
