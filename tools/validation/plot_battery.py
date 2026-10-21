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


def phase_windows(phases):
    """Retorna {phase_name: (start_epoch, end_epoch)} usando o start da fase
    seguinte como fim (baseline->jamming->recovery->end)."""
    ordered = [p for p in phases if p["name"] in ("baseline", "jamming", "recovery", "end")]
    ordered.sort(key=lambda p: p["start_epoch"])
    wins = {}
    for i, p in enumerate(ordered):
        if p["name"] == "end":
            continue
        end = ordered[i + 1]["start_epoch"] if i + 1 < len(ordered) else p["start_epoch"] + 1e9
        wins[p["name"]] = (p["start_epoch"], end)
    return wins


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else latest_battery_file()
    with open(path) as f:
        battery = json.load(f)

    session_path = battery["session_file"]
    scenarios = battery["scenarios"]
    labels = battery.get("scenario_labels", {})
    by_scenario = phases_by_scenario(battery)

    fig, (ax_thr, ax_ack, ax_bar) = plt.subplots(3, 1, figsize=(11, 12), sharex=False)
    # jamming-mitigation-hardening: métricas por fase (baseline/jamming/
    # recovery) por cenário, para o painel de barras e a taxa de sobrevivência.
    per_phase_thr = {}  # scenario -> {phase: mean throughput (telemetria frags/s)}
    per_phase_acks = {}  # scenario -> {phase: total de ACKs recebidos}
    per_phase_ackrate = {}  # scenario -> {phase: ACKs/s (contagem direta, confiável)}
    per_phase_lat = {}  # scenario -> {phase: [latências ms]}

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
        # latência: event-driven emite num evento "latency" separado; time-slotted
        # embute "ms" no próprio "ack". Lê dos dois pra funcionar nas duas variantes.
        lat = [(d["epoch"] - t_start, float(d["fields"]["ms"]))
               for d in events if d.get("src") == "mestre" and d.get("event") in ("latency", "ack") and "ms" in d.get("fields", {})]
        # ACKs recebidos (um por fragmento confirmado) -- para o painel de acks/s
        # e a contagem por fase. src=mestre, event=ack nas duas variantes.
        acks = [(d["epoch"] - t_start) for d in events
                if d.get("src") == "mestre" and d.get("event") == "ack"]
        plr = [(d["epoch"], float(d["fields"]["window"]))
               for d in events if d.get("src") == "escravo" and d.get("event") == "plr" and "window" in d.get("fields", {})]

        color = SCENARIO_COLOR.get(scenario, "gray")
        label = f"{scenario}: {labels.get(scenario, '')}"

        if thr:
            # jamming-mitigation-hardening: quebra a linha onde há um GAP de
            # dados (> 6 s sem evento de throughput) -- senão o plot conecta
            # baseline e recovery por cima de uma fase de colapso (0 throughput,
            # ex. Cenário A totalmente negado), desenhando uma linha reta falsa
            # que parece "throughput normal durante o jamming".
            pts = sorted(thr)
            xs, ys = [], []
            prev_t = None
            for (t, y) in pts:
                if prev_t is not None and (t - prev_t) > 6.0:
                    xs.append(float("nan"))
                    ys.append(float("nan"))  # NaN quebra a linha em matplotlib
                xs.append(t)
                ys.append(y)
                prev_t = t
            ax_thr.plot(xs, ys, marker="o", markersize=3, color=color, label=label)
        # ACKs/s ao longo do tempo: bina os eventos de ack em janelas de 2s.
        if acks:
            BIN = 2.0
            tmax = max(acks)
            nb = int(tmax // BIN) + 1
            counts = [0] * nb
            for t in acks:
                counts[int(t // BIN)] += 1
            axk = [i * BIN + BIN / 2 for i in range(nb)]
            ayk = [c / BIN for c in counts]
            ax_ack.plot(axk, ayk, marker="o", markersize=3, color=color, label=label)

        jam_start = named.get("jamming")
        jam_end = named.get("recovery")
        if jam_start is not None and jam_end is not None:
            ax_thr.axvspan(jam_start - t_start, jam_end - t_start, color=color, alpha=0.05)
            ax_ack.axvspan(jam_start - t_start, jam_end - t_start, color=color, alpha=0.05)

            plr_during_jam = [v for ep, v in plr if jam_start <= ep <= jam_end]
            thr_during_jam = [v for t, v in thr if (jam_start - t_start) <= t <= (jam_end - t_start)]
            lat_during_jam = [v for t, v in lat if (jam_start - t_start) <= t <= (jam_end - t_start)]
            summary_rows.append((scenario, mean(plr_during_jam), mean(thr_during_jam), mean(lat_during_jam)))

        # throughput médio por fase (para o painel de barras + taxa de sobrevivência)
        wins = phase_windows(phases)
        ph_means = {}
        ph_acks = {}
        ph_ackrate = {}
        for ph_name, (ws, we) in wins.items():
            vals = [y for (ep, y) in [(t + t_start, y) for (t, y) in thr] if ws <= ep < we]
            ph_means[ph_name] = mean(vals)
            # total de ACKs recebidos na fase (contagem absoluta) e taxa (acks/s).
            n = sum(1 for t in acks if ws <= (t + t_start) < we)
            ph_acks[ph_name] = n
            dur = (we - ws)
            ph_ackrate[ph_name] = (n / dur) if dur and dur < 1e8 else 0.0
        # amostras de latência (ms) por fase, para o painel de distribuição.
        per_phase_lat[scenario] = {
            ph_name: [v for (t, v) in lat if ws <= (t + t_start) < we]
            for ph_name, (ws, we) in wins.items()
        }
        per_phase_thr[scenario] = ph_means
        per_phase_acks[scenario] = ph_acks
        per_phase_ackrate[scenario] = ph_ackrate

    ax_thr.set_ylabel("throughput (fragmentos confirmados/s)")
    ax_thr.set_title("Throughput × tempo (relativo ao início da baseline de cada cenário)")
    ax_thr.grid(alpha=0.3)
    ax_thr.legend(loc="upper right", fontsize=8)

    ax_ack.set_ylabel("ACKs recebidos/s")
    ax_ack.set_title("ACKs recebidos × tempo (janela de 2 s; total por fase no resumo)")
    ax_ack.grid(alpha=0.3)
    ax_ack.legend(loc="upper right", fontsize=8)

    # Painel de latência removido: no sistema por slots a latência fica
    # estruturalmente constante (~3 ms mediana / ~19 ms p95) em todos os
    # cenários e fases, inclusive sob jamming -- não diferencia nada
    # visualmente. O fato fica registrado no resumo em texto (lat jam ms).
    import numpy as np  # usado pelo painel de barras abaixo

    # -------- Painel de barras: ACKs/s médio por fase, por cenário -------
    # Usa a TAXA DE ACKS (contagem direta), não a telemetria de throughput: sob
    # colapso, a janela de throughput do firmware (350 hops) atravessa a baseline
    # e reporta uma taxa de jamming falsamente alta (ex.: event-driven cenário A
    # aparentava ~48% quando só houve ~11 acks). A contagem de acks é confiável
    # nas duas variantes.
    phase_order = ["baseline", "jamming", "recovery"]
    phase_labels = {"baseline": "baseline\n(sem jammer)", "jamming": "jamming\n(hold PA_MAX)", "recovery": "recovery\n(jammer off)"}
    x = np.arange(len(phase_order))
    nscen = max(len(scenarios), 1)
    width = 0.8 / nscen
    for i, scenario in enumerate(scenarios):
        ph = per_phase_ackrate.get(scenario, {})
        vals = [ph.get(p, 0.0) for p in phase_order]
        offs = x + (i - (nscen - 1) / 2) * width
        ax_bar.bar(offs, vals, width, color=SCENARIO_COLOR.get(scenario, "gray"),
                   label=f"{scenario}: {labels.get(scenario, '')[:38]}")
        # taxa de sobrevivência sob jamming (acks/s jamming ÷ baseline).
        base = ph.get("baseline")
        jam = ph.get("jamming")
        if base and base > 0:
            surv = 100.0 * (jam or 0.0) / base
            ax_bar.annotate(f"{surv:.0f}%", (offs[1], vals[1]), textcoords="offset points",
                            xytext=(0, 3), ha="center", fontsize=9, fontweight="bold",
                            color=SCENARIO_COLOR.get(scenario, "gray"))
    ax_bar.set_xticks(x)
    ax_bar.set_xticklabels([phase_labels[p] for p in phase_order])
    ax_bar.set_ylabel("ACKs recebidos/s (médio)")
    ax_bar.set_title("ACKs/s por fase (%=taxa de sobrevivência sob jamming vs baseline do cenário)")
    ax_bar.grid(alpha=0.3, axis="y")
    ax_bar.legend(loc="upper right", fontsize=8)

    jm = battery.get("jammer_mode")
    jp = battery.get("jammer_power")
    fig.suptitle(f"Bateria A/B/C sob jammer confiável (mode={jm}, power={jp})", fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.97])

    ts = os.path.basename(path).replace("battery-", "").replace(".json", "")
    out_png = os.path.join(LOGS_DIR, f"battery-{ts}.png")
    fig.savefig(out_png, dpi=140)
    print(f"\nGráfico salvo em {out_png}\n")

    print(f"{'Cenário':<8} {'base f/s':>10} {'jam f/s':>10} {'recov f/s':>10} {'sobreviv.jam':>13} {'PLR jam %':>10} {'lat jam ms':>11}")
    plr_by = {s: p for (s, p, _, _) in summary_rows}
    lat_by = {s: l for (s, _, _, l) in summary_rows}
    for scenario in scenarios:
        ph = per_phase_thr.get(scenario, {})
        base = ph.get("baseline", float("nan"))
        jam = ph.get("jamming", float("nan"))
        recov = ph.get("recovery", float("nan"))
        jam_disp = 0.0 if jam != jam else jam  # NaN (colapso total) -> 0
        ret = (100.0 * jam_disp / base) if (base and base > 0) else float("nan")
        print(f"{scenario:<8} {base:>10.2f} {jam_disp:>10.2f} {recov:>10.2f} {ret:>12.0f}% "
              f"{plr_by.get(scenario, float('nan')):>10.1f} {lat_by.get(scenario, float('nan')):>11.1f}")
    # jamming-mitigation-hardening / time-slotted: total de ACKs recebidos por
    # fase e cenário (contagem absoluta) -- "quantas mensagens/fragmentos foram
    # confirmados em cada cenário".
    print(f"\n{'Cenário':<8} {'ACKs base':>10} {'ACKs jam':>10} {'ACKs recov':>11}")
    for scenario in scenarios:
        a = per_phase_acks.get(scenario, {})
        print(f"{scenario:<8} {a.get('baseline', 0):>10} {a.get('jamming', 0):>10} {a.get('recovery', 0):>11}")
    print(
        "\nLeitura: A (sem hopping) deve colapsar sob o hold no canal fixo; B (hopping "
        "sem mitigação) sofre o dessincronismo; C (mitigação on) deve reter mais que B. "
        "Taxa de sobrevivência = throughput na fase jamming / baseline do mesmo cenário."
    )


if __name__ == "__main__":
    main()
