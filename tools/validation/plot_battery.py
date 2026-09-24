#!/usr/bin/env python3
"""
scenario-automation: lê um logs/battery-*.json (gerado por
run_battery.py) + o session .jsonl referenciado nele, e produz
logs/battery-<ts>.png com 2 painéis (throughput × tempo, latência ×
tempo), uma linha por cenário (A/B/C), eixo do tempo relativo ao início
da fase 'baseline' de CADA cenário (pra sobrepor diretamente, já que os
3 cenários rodam em sequência na mesma sessão, não em paralelo). A janela
de jamming (mesma duração/config nos 3, por design -- ver run_battery.py)
fica sombreada em vermelho claro.

Também imprime um resumo textual: PLR médio de janela por cenário durante
a fase 'jamming', e throughput médio (frags/s) por cenário/fase -- para
conferir rapidamente se A (sem hopping) degrada mais que B (hopping sem
self-healing), que por sua vez degrada mais que C (sistema completo),
como o guia da monografia espera.

Uso:
    python3 tools/validation/plot_battery.py logs/battery-<ts>.json
    python3 tools/validation/plot_battery.py   # usa o mais recente
"""
import glob
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")

SCENARIO_COLOR = {"A": "#d62728", "B": "#ff7f0e", "C": "#2ca02c"}


def latest_battery_file():
    files = sorted(glob.glob(os.path.join(LOGS_DIR, "battery-*.json")))
    if not files:
        print(
            "Nenhum logs/battery-*.json encontrado -- rode run_battery.py "
            "primeiro, ou passe o caminho explicitamente.",
            file=sys.stderr,
        )
        sys.exit(1)
    return files[-1]


def load_session_events(session_path, t_start, t_end):
    events = []
    try:
        f = open(session_path)
    except FileNotFoundError:
        print(f"AVISO: session file {session_path} não existe mais.", file=sys.stderr)
        return events
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            ep = d.get("epoch")
            # limite superior exclusivo: os cenários rodam em sequência na
            # mesma sessão, sem gap garantido -- inclusivo deixaria a
            # primeira amostra do PRÓXIMO cenário vazar pra este quando o
            # "end" de um coincide com o "baseline" do outro.
            if ep is None or not (t_start <= ep < t_end):
                continue
            events.append(d)
    return events


def phases_by_scenario(battery):
    by_scenario = {}
    for p in battery["phases"]:
        by_scenario.setdefault(p["scenario"], []).append(p)
    return by_scenario


def scenario_window(phases):
    epochs = [p["start_epoch"] for p in phases]
    by_name = {p["name"]: p["start_epoch"] for p in phases}
    return min(epochs), max(epochs), by_name


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else latest_battery_file()
    with open(path) as f:
        battery = json.load(f)

    session_path = battery["session_file"]
    scenarios = battery["scenarios"]
    labels = battery.get("scenario_labels", {})
    by_scenario = phases_by_scenario(battery)

    fig, (ax_thr, ax_lat) = plt.subplots(2, 1, figsize=(11, 8), sharex=False)

    print("=" * 78)
    print(f"Bateria comparativa: {path}")
    print(f"Cenários: {scenarios}  (jammer mode={battery.get('jammer_mode')} power={battery.get('jammer_power')})")
    print("=" * 78)

    summary_rows = []

    for scenario in scenarios:
        phases = by_scenario.get(scenario, [])
        if not phases:
            continue
        t_start, t_end, named = scenario_window(phases)
        events = load_session_events(session_path, t_start, t_end)

        thr = [(d["epoch"] - t_start, float(d["fields"]["frags_per_s"]))
               for d in events if d.get("src") == "mestre" and d.get("event") == "throughput" and "frags_per_s" in d.get("fields", {})]
        lat = [(d["epoch"] - t_start, float(d["fields"]["ms"]))
               for d in events if d.get("src") == "mestre" and d.get("event") == "latency" and "ms" in d.get("fields", {})]
        plr = [(d["epoch"], float(d["fields"]["window"]))
               for d in events if d.get("src") == "escravo" and d.get("event") == "plr" and "window" in d.get("fields", {})]

        color = SCENARIO_COLOR.get(scenario, "gray")
        label = f"{scenario}: {labels.get(scenario, '')}"

        if thr:
            xs, ys = zip(*sorted(thr))
            ax_thr.plot(xs, ys, marker="o", markersize=3, color=color, label=label)
        if lat:
            xs, ys = zip(*sorted(lat))
            ax_lat.scatter(xs, ys, s=6, color=color, alpha=0.5, label=label)

        jam_start = named.get("jamming")
        jam_end = named.get("recovery")
        if jam_start is not None and jam_end is not None:
            ax_thr.axvspan(jam_start - t_start, jam_end - t_start, color=color, alpha=0.05)
            ax_lat.axvspan(jam_start - t_start, jam_end - t_start, color=color, alpha=0.05)

            plr_during_jam = [v for ep, v in plr if jam_start <= ep <= jam_end]
            thr_during_jam = [v for t, v in thr if (jam_start - t_start) <= t <= (jam_end - t_start)]
            lat_during_jam = [v for t, v in lat if (jam_start - t_start) <= t <= (jam_end - t_start)]
            summary_rows.append((scenario, mean(plr_during_jam), mean(thr_during_jam), mean(lat_during_jam)))

    ax_thr.set_ylabel("throughput (fragmentos confirmados/s)")
    ax_thr.set_title("Throughput × tempo (relativo ao início da baseline de cada cenário)")
    ax_thr.grid(alpha=0.3)
    ax_thr.legend(loc="upper right", fontsize=8)

    ax_lat.set_ylabel("latência por fragmento (ms)")
    ax_lat.set_xlabel("tempo (s, desde o início da baseline do cenário)")
    ax_lat.set_title("Latência × tempo (relativo ao início da baseline de cada cenário)")
    ax_lat.grid(alpha=0.3)
    ax_lat.legend(loc="upper right", fontsize=8)

    fig.suptitle("Cenários comparativos A/B/C: throughput e latência sob o mesmo jamming", fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    ts = os.path.basename(path).replace("battery-", "").replace(".json", "")
    out_png = os.path.join(LOGS_DIR, f"battery-{ts}.png")
    fig.savefig(out_png, dpi=140)
    print(f"\nGráfico salvo em {out_png}\n")

    print(f"{'Cenário':<10} {'PLR média sob jamming (%)':>26} {'throughput médio (f/s)':>24} {'latência média (ms)':>22}")
    for scenario, plr_avg, thr_avg, lat_avg in summary_rows:
        print(f"{scenario:<10} {plr_avg:>26.1f} {thr_avg:>24.2f} {lat_avg:>22.1f}")
    print(
        "\nEsperado pelo guia: PLR/latência pioram e throughput cai de C -> B -> A "
        "(C com self-healing deveria degradar menos que B, que por sua vez deveria "
        "degradar menos que A, sem hopping nenhum)."
    )


if __name__ == "__main__":
    main()
