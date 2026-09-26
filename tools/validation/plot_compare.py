#!/usr/bin/env python3
"""
time-slotted-hopping: compara DUAS baterias A/B/C (ex.: event-driven vs
time-slotted) num único gráfico. Lê dois logs/battery-*.json (cada um com o
session .jsonl referenciado) e produz um PNG com:
  - Painel 1: retenção sob jamming (msg_done/s jamming / baseline) por cenário
    A/B/C, barras lado a lado das duas variantes.
  - Painel 2: throughput absoluto (msg_done/s) na fase jamming, por cenário.
  - Painel 3: throughput × tempo do Cenário C nas duas variantes sobrepostas,
    com a janela de jamming sombreada.

Uso:
    python3 tools/validation/plot_compare.py \
        --a logs/battery-EV.json --a-label "Event-driven" \
        --b logs/battery-TD.json --b-label "Time-slotted" \
        [--out logs/compare.png]
"""
import argparse
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")
SCEN = ["A", "B", "C"]


def load(bpath):
    b = json.load(open(bpath))
    sess = b["session_file"]
    if not os.path.exists(sess):
        sess = os.path.join(LOGS_DIR, os.path.basename(sess))
    ph = b["phases"]
    for i, p in enumerate(ph):
        p["end"] = ph[i + 1]["start_epoch"] if i + 1 < len(ph) else p["start_epoch"] + 100
    # msg_done timestamps (placa_a = mestre)
    done = []
    if os.path.exists(sess):
        for line in open(sess):
            try:
                o = json.loads(line)
            except Exception:
                continue
            if o.get("board") == "placa_a" and o.get("event") == "msg_done" and o.get("epoch"):
                done.append(o["epoch"])
    return b, ph, done


def phase_rate(ph, done, scen, name):
    d = [p for p in ph if p["scenario"] == scen and p["name"] == name]
    if not d:
        return 0.0, 0.0
    p = d[0]
    dur = p["end"] - p["start_epoch"]
    n = sum(1 for e in done if p["start_epoch"] <= e < p["end"])
    return (n / dur if dur else 0.0), dur


def retention(ph, done):
    out = {}
    for s in SCEN:
        br, _ = phase_rate(ph, done, s, "baseline")
        jr, _ = phase_rate(ph, done, s, "jamming")
        out[s] = (br, jr, (100 * jr / br) if br else 0.0)
    return out


def c_timeline(ph, done, bin_s=2.0):
    """throughput (msg/s) do Cenário C ao longo do tempo, relativo ao início do baseline C."""
    cph = [p for p in ph if p["scenario"] == "C"]
    if not cph:
        return [], [], (0, 0)
    t0 = cph[0]["start_epoch"]
    tend = cph[-1]["end"]
    jam = [p for p in cph if p["name"] == "jamming"]
    jam_win = ((jam[0]["start_epoch"] - t0), (jam[0]["end"] - t0)) if jam else (0, 0)
    xs, ys = [], []
    t = 0.0
    while t0 + t < tend:
        lo, hi = t0 + t, t0 + t + bin_s
        n = sum(1 for e in done if lo <= e < hi)
        xs.append(t + bin_s / 2)
        ys.append(n / bin_s)
        t += bin_s
    return xs, ys, jam_win


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="battery json da variante A")
    ap.add_argument("--a-label", default="A")
    ap.add_argument("--b", required=True, help="battery json da variante B")
    ap.add_argument("--b-label", default="B")
    ap.add_argument("--out", default=os.path.join(LOGS_DIR, "compare.png"))
    args = ap.parse_args()

    ba, pha, da = load(args.a)
    bb, phb, db = load(args.b)
    ra, rb = retention(pha, da), retention(phb, db)

    COL_A, COL_B = "#ff7f0e", "#2ca02c"
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 12))
    import numpy as np

    x = np.arange(len(SCEN))
    w = 0.38

    # Painel 1: retenção %
    va = [ra[s][2] for s in SCEN]
    vb = [rb[s][2] for s in SCEN]
    ax1.bar(x - w / 2, va, w, label=args.a_label, color=COL_A)
    ax1.bar(x + w / 2, vb, w, label=args.b_label, color=COL_B)
    for i, v in enumerate(va):
        ax1.text(i - w / 2, v + 1, f"{v:.0f}%", ha="center", va="bottom", fontsize=9)
    for i, v in enumerate(vb):
        ax1.text(i + w / 2, v + 1, f"{v:.0f}%", ha="center", va="bottom", fontsize=9)
    ax1.set_xticks(x)
    ax1.set_xticklabels(["A: sem hopping", "B: FHSS s/ mitig.", "C: mitigação completa"])
    ax1.set_ylabel("Retenção sob jamming\n(msg/s jam ÷ baseline)")
    ax1.set_title("Retenção sob jamming (hold ch10 PA_MAX) — maior é melhor")
    ax1.set_ylim(0, 105)
    ax1.legend()
    ax1.grid(axis="y", alpha=0.3)

    # Painel 2: throughput absoluto na fase jamming
    ja = [phase_rate(pha, da, s, "jamming")[0] for s in SCEN]
    jb = [phase_rate(phb, db, s, "jamming")[0] for s in SCEN]
    ax2.bar(x - w / 2, ja, w, label=args.a_label, color=COL_A)
    ax2.bar(x + w / 2, jb, w, label=args.b_label, color=COL_B)
    for i, v in enumerate(ja):
        ax2.text(i - w / 2, v + 0.1, f"{v:.1f}", ha="center", va="bottom", fontsize=9)
    for i, v in enumerate(jb):
        ax2.text(i + w / 2, v + 0.1, f"{v:.1f}", ha="center", va="bottom", fontsize=9)
    ax2.set_xticks(x)
    ax2.set_xticklabels(["A", "B", "C"])
    ax2.set_ylabel("msg/s durante jamming")
    ax2.set_title("Throughput absoluto durante o jamming")
    ax2.legend()
    ax2.grid(axis="y", alpha=0.3)

    # Painel 3: timeline do Cenário C
    xa, ya, wa = c_timeline(pha, da)
    xb, yb, wb = c_timeline(phb, db)
    ax3.plot(xa, ya, color=COL_A, label=f"{args.a_label} (C)")
    ax3.plot(xb, yb, color=COL_B, label=f"{args.b_label} (C)")
    win = wa if wa != (0, 0) else wb
    if win != (0, 0):
        ax3.axvspan(win[0], win[1], color="red", alpha=0.12, label="jamming")
    ax3.set_xlabel("tempo no Cenário C (s, relativo ao baseline)")
    ax3.set_ylabel("msg/s")
    ax3.set_title("Cenário C (mitigação completa): throughput × tempo")
    ax3.legend()
    ax3.grid(alpha=0.3)

    plt.tight_layout()
    plt.savefig(args.out, dpi=110)
    print(f"salvo: {args.out}")
    # resumo textual
    print(f"\n{'cenário':<28}{args.a_label:>16}{args.b_label:>16}")
    for s in SCEN:
        print(f"  {s} retenção            {ra[s][2]:>14.0f}%{rb[s][2]:>15.0f}%")
    for s in SCEN:
        print(f"  {s} baseline msg/s      {ra[s][0]:>15.1f}{rb[s][0]:>16.1f}")
        print(f"  {s} jamming  msg/s      {phase_rate(pha,da,s,'jamming')[0]:>15.1f}{phase_rate(phb,db,s,'jamming')[0]:>16.1f}")


if __name__ == "__main__":
    main()
