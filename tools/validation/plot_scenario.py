#!/usr/bin/env python3
"""
adaptive-rendezvous / dynamic-seed-blacklist: lê um logs/scenario-*.json
(gerado por run_scenario.py) + o session .jsonl referenciado nele, e
produz:

  1. Um resumo textual PASS/AVISO no terminal -- pareamento, degradação
     de PLR sob jamming, disparo do alerta, rekey de seed/blacklist,
     fallback (loss_of_sync) e recuperação.
  2. Uma figura PNG (logs/scenario-<ts>.png) com 3 painéis compartilhando
     o eixo do tempo:
       - topo: ack (verde) / miss (vermelho) de cada hop do mestre --
         mostra visualmente onde o jamming interrompe a entrega.
       - meio: PLR de janela (escravo) + linhas verticais de
         loss_of_sync (entrada no fallback) e pair_ok (saída dele).
       - baixo: canal usado por hop + linhas verticais de rekey_tx/
         rekey_apply/blacklist (troca de seed/máscara).
     A janela em que o jammer esteve de fato emitindo (jam_state_on..
     jam_state_off, não o instante do comando) fica sombreada em
     vermelho claro nos três painéis.

Uso:
    python3 tools/validation/plot_scenario.py logs/scenario-<ts>.json
    python3 tools/validation/plot_scenario.py   # usa o mais recente
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


def latest_scenario_file():
    files = sorted(glob.glob(os.path.join(LOGS_DIR, "scenario-*.json")))
    if not files:
        print(
            "Nenhum logs/scenario-*.json encontrado -- rode "
            "run_scenario.py primeiro, ou passe o caminho explicitamente.",
            file=sys.stderr,
        )
        sys.exit(1)
    return files[-1]


def load_events(session_path, t_start, t_end, pad=5.0):
    events = []
    try:
        f = open(session_path)
    except FileNotFoundError:
        print(f"AVISO: session file {session_path} não existe mais.", file=sys.stderr)
        return events
    with f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            ep = d.get("epoch")
            if ep is None or ep < t_start - pad or ep > t_end + pad:
                continue
            if d.get("event") in (None, "raw"):
                continue
            events.append(d)
    return events


def check(label, cond, detail=""):
    mark = "PASS " if cond else "AVISO"
    print(f"[{mark}] {label}" + (f" -- {detail}" if detail else ""))


def main():
    scenario_path = sys.argv[1] if len(sys.argv) > 1 else latest_scenario_file()
    with open(scenario_path) as f:
        scenario = json.load(f)

    phases = scenario["phases"]
    t0 = phases[0]["start_epoch"]
    t_end = phases[-1]["start_epoch"]
    session_path = scenario["session_file"]

    events = load_events(session_path, t0, t_end)
    if not events:
        print(
            f"AVISO: nenhum evento estruturado encontrado em {session_path} "
            "na janela do cenário -- o session file pode ter girado "
            "(logger reiniciado no meio?) ou nada foi capturado.",
            file=sys.stderr,
        )

    def rel(ep):
        return ep - t0

    def by(event_name, src=None):
        return [
            e
            for e in events
            if e.get("event") == event_name and (src is None or e.get("src") == src)
        ]

    acks = by("ack", "mestre")
    misses = by("miss", "mestre")
    hops = by("hop", "mestre")
    plrs = by("plr", "escravo")
    loss = by("loss_of_sync")
    pair_ok = by("pair_ok")
    rekey_tx = by("rekey_tx")
    rekey_ack = by("rekey_ack")
    rekey_apply = by("rekey_apply")
    rekey_failed = by("rekey_failed")
    blacklist = by("blacklist")
    jam_on = sorted(by("jam_state_on"), key=lambda e: e["epoch"])
    jam_off = sorted(by("jam_state_off"), key=lambda e: e["epoch"])
    alert_on = by("alert_on")

    # --- janelas de jamming reais (telemetria, não o instante do comando) ---
    jam_windows = []
    for on in jam_on:
        off_ep = t_end
        for off in jam_off:
            if off["epoch"] > on["epoch"]:
                off_ep = off["epoch"]
                break
        jam_windows.append((rel(on["epoch"]), rel(off_ep)))

    # =================== RESUMO TEXTUAL ===================
    print("=" * 70)
    print("RESUMO DO CENÁRIO DE VALIDAÇÃO")
    print("=" * 70)
    print(f"Janela: {rel(t0):.0f}s .. {rel(t_end):.0f}s ({t_end - t0:.0f}s totais)")
    phase_bounds = [
        (phases[i]["name"], rel(phases[i]["start_epoch"]), rel(phases[i + 1]["start_epoch"]))
        for i in range(len(phases) - 1)
    ]
    for name, lo, hi in phase_bounds:
        print(f"  fase '{name}': t={lo:.0f}s..{hi:.0f}s")
    print()

    check("Pareamento observado (pair_ok)", len(pair_ok) > 0, f"{len(pair_ok)} ocorrência(s)")
    check("Jammer realmente emitiu (jam_state_on)", len(jam_on) > 0, f"{len(jam_on)} ocorrência(s)")

    def plr_avg(lo, hi):
        vals = [float(e["fields"]["window"]) for e in plrs if lo <= rel(e["epoch"]) <= hi]
        return sum(vals) / len(vals) if vals else None

    phase_plr = {}
    for name, lo, hi in phase_bounds:
        avg = plr_avg(lo, hi)
        phase_plr[name] = avg
        print(f"  PLR médio de janela na fase '{name}': " + (f"{avg:.1f}%" if avg is not None else "sem amostras"))
    print()

    baseline_plr = phase_plr.get("baseline")
    jam_plr = phase_plr.get("jamming")
    recovery_plr = phase_plr.get("recovery")

    if baseline_plr is not None and jam_plr is not None:
        check(
            "Jamming realmente degradou o PLR",
            jam_plr > baseline_plr + 5,
            f"baseline={baseline_plr:.1f}% jamming={jam_plr:.1f}%",
        )
    else:
        print("[AVISO] PLR insuficiente para comparar baseline vs. jamming (poucas amostras)")

    check("Alerta do analisador disparou (alert_on)", len(alert_on) > 0, f"{len(alert_on)} ocorrência(s)")
    check(
        "Rekey iniciado (rekey_tx)",
        len(rekey_tx) > 0,
        ", ".join(e["fields"].get("kind", "?") for e in rekey_tx) or "nenhum",
    )
    check("Rekey confirmado (rekey_ack)", len(rekey_ack) > 0, f"{len(rekey_ack)} ocorrência(s)")
    check(
        "Seed trocada em voo (rekey_apply kind=seed)",
        len(rekey_apply) > 0,
        ", ".join(f"seed={e['fields'].get('seed', '?')}@hop={e['fields'].get('hop', '?')}" for e in rekey_apply)
        or "nenhuma",
    )
    check(
        "Blacklist de canais aplicada (blacklist)",
        len(blacklist) > 0,
        ", ".join(f"mask={e['fields'].get('mask', '?')}" for e in blacklist) or "nenhuma",
    )
    if rekey_failed:
        print(f"[AVISO] {len(rekey_failed)} rekey_failed -- controle não confirmado dentro do prazo de retries")
    if loss:
        print(f"[INFO ] {len(loss)} loss_of_sync -- enlace caiu e usou o fallback de pareamento/varredura")
    else:
        print("[INFO ] nenhum loss_of_sync -- enlace nunca precisou do fallback completo (bom sinal se PLR ficou baixo)")

    if baseline_plr is not None and recovery_plr is not None:
        check(
            "PLR volta ao patamar normal após 'jam off'",
            recovery_plr < baseline_plr + 10,
            f"baseline={baseline_plr:.1f}% recovery={recovery_plr:.1f}%",
        )
    print("=" * 70)

    # =================== FIGURA ===================
    # Fontes bem maiores que o default -- o gráfico é visto reduzido (chat,
    # slide, PDF), então precisa ser legível mesmo pequeno. Legendas ficam
    # DENTRO da área de cada painel (não em bbox_to_anchor fora do eixo,
    # que ficava minúsculo ao encolher a imagem).
    plt.rcParams.update(
        {
            "font.size": 13,
            "axes.titlesize": 15,
            "axes.labelsize": 13,
            "xtick.labelsize": 12,
            "ytick.labelsize": 12,
            "legend.fontsize": 11.5,
        }
    )

    fig, axes = plt.subplots(
        3, 1, figsize=(16, 12), sharex=True, gridspec_kw={"height_ratios": [1.1, 1, 1.3]}
    )
    ax_hop, ax_plr, ax_ch = axes

    def shade_jam(ax, label_y_frac=0.92):
        for lo, hi in jam_windows:
            ax.axvspan(lo, hi, color="red", alpha=0.12, zorder=0)
            mid = (lo + hi) / 2
            ylo, yhi = ax.get_ylim()
            ax.text(
                mid, ylo + (yhi - ylo) * label_y_frac, "JAMMER LIGADO",
                ha="center", va="top", fontsize=11, color="firebrick", fontweight="bold",
                bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="firebrick", alpha=0.75),
            )

    # --- painel 1: ack/miss por hop ---
    if misses:
        ax_hop.scatter(
            [rel(e["epoch"]) for e in misses], [0] * len(misses),
            marker="x", color="crimson", s=45, linewidths=2, label=f"miss ({len(misses)})", zorder=3,
        )
    if acks:
        ax_hop.scatter(
            [rel(e["epoch"]) for e in acks], [1] * len(acks),
            marker="o", color="seagreen", s=18, label=f"ack ({len(acks)})", zorder=3,
        )
    ax_hop.set_yticks([0, 1])
    ax_hop.set_yticklabels(["miss", "ack"])
    ax_hop.set_ylim(-0.5, 1.5)
    ax_hop.set_title("1. Envio de fragmentos por hop (mestre) — onde o jamming interrompe a entrega")
    if acks or misses:
        ax_hop.legend(loc="upper left", framealpha=0.9)
    shade_jam(ax_hop)

    # --- painel 2: PLR + fallback ---
    if plrs:
        xs = [rel(e["epoch"]) for e in plrs]
        ys = [float(e["fields"]["window"]) for e in plrs]
        ax_plr.plot(xs, ys, color="steelblue", linewidth=2, label="PLR de janela (%)")
    for e in loss:
        ax_plr.axvline(rel(e["epoch"]), color="black", linestyle="--", linewidth=1.5, alpha=0.8)
    for e in pair_ok:
        ax_plr.axvline(rel(e["epoch"]), color="darkgreen", linestyle=":", linewidth=2, alpha=0.9)
    if loss:
        ax_plr.plot([], [], color="black", linestyle="--", linewidth=1.5, label="loss_of_sync (entra no fallback)")
    if pair_ok:
        ax_plr.plot([], [], color="darkgreen", linestyle=":", linewidth=2, label="pair_ok (recuperado)")
    ax_plr.set_ylabel("PLR (%)")
    ax_plr.set_title("2. Packet loss rate + fallback de pareamento (adaptive-rendezvous)")
    ax_plr.legend(loc="upper left", framealpha=0.9)
    shade_jam(ax_plr)

    # --- painel 3: canal + rekey/blacklist ---
    if hops:
        xs = [rel(e["epoch"]) for e in hops]
        ys = [int(e["fields"]["ch"]) for e in hops]
        ax_ch.scatter(xs, ys, s=8, color="gray", alpha=0.6, label="canal usado")
    for e in rekey_tx:
        ax_ch.axvline(rel(e["epoch"]), color="orange", linestyle="-.", linewidth=1.5)
    for e in rekey_apply:
        ax_ch.axvline(rel(e["epoch"]), color="purple", linestyle="-", linewidth=2)
    # blacklist-decay: "prev" (se presente -- sessões antigas não têm)
    # distingue exclusão (bits somem, magenta) de reinclusão por cooldown
    # (bits voltam, verde) sem precisar de estado próprio aqui.
    saw_exclusion = False
    saw_recovery = False
    for e in blacklist:
        prev_raw = e["fields"].get("prev")
        mask_raw = e["fields"].get("mask")
        is_recovery = False
        if prev_raw is not None and mask_raw is not None:
            try:
                is_recovery = bin(int(mask_raw, 16)).count("1") > bin(int(prev_raw, 16)).count("1")
            except ValueError:
                pass
        saw_recovery = saw_recovery or is_recovery
        saw_exclusion = saw_exclusion or not is_recovery
        ax_ch.axvline(rel(e["epoch"]), color="seagreen" if is_recovery else "magenta", linestyle="-", linewidth=2)
    if rekey_tx:
        ax_ch.plot([], [], color="orange", linestyle="-.", linewidth=1.5, label="rekey_tx (seed/mask proposto)")
    if rekey_apply:
        ax_ch.plot([], [], color="purple", linestyle="-", linewidth=2, label="rekey_apply (seed trocada)")
    if saw_exclusion:
        ax_ch.plot([], [], color="magenta", linestyle="-", linewidth=2, label="blacklist (canal excluído)")
    if saw_recovery:
        ax_ch.plot([], [], color="seagreen", linestyle="-", linewidth=2, label="blacklist (canal reincluído)")
    ax_ch.axhspan(2, 80, color="steelblue", alpha=0.06, zorder=0)
    t_span = (t_end - t0) or 1.0
    ax_ch.text(
        0.01 * t_span, 41, "faixa 2–80 MHz (varrida pelo jammer sweep)",
        fontsize=9.5, color="steelblue", va="center", ha="left",
    )
    ax_ch.set_ylabel("Canal RF")
    ax_ch.set_xlabel("Tempo (s desde início do cenário)")
    ax_ch.set_title("3. Canal em uso por hop + troca de seed/blacklist")
    ax_ch.legend(loc="upper left", framealpha=0.9)
    shade_jam(ax_ch, label_y_frac=0.06)

    fig.suptitle("Validação end-to-end: jamming, fallback e rekey", fontsize=18, y=0.995, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.97])

    out_png = scenario_path.replace(".json", ".png")
    fig.savefig(out_png, dpi=170, bbox_inches="tight")
    print(f"\nGráfico salvo em: {out_png}")


if __name__ == "__main__":
    main()
