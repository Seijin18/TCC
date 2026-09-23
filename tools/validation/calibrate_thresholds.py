#!/usr/bin/env python3
"""
spectrum-attack-classification: analisa o CSV bruto do analisador (raw,
capturado por tools/logger/main.py durante uma ou mais execucoes de
tools/validation/run_scenario.py) para propor valores calibrados de
LIMIAR_ALVO/LIMIAR_CONCENTRACAO/HEAT_HIT_THRESHOLD/HEAT_DECAY_PER_CYCLE
contra o piso de ruido REAL medido nesta bancada, em vez dos valores
default herdados sem calibracao.

Simula channelHeat[] (mesmo algoritmo de analisador/src/node.h) sobre a
sequencia real de leituras de cada sessao, separando fase baseline
(jammer desligado) de fase jamming (jammer ligado), e varre uma grade de
(HEAT_HIT_THRESHOLD, HEAT_DECAY_PER_CYCLE) buscando a combinacao que
maximiza a separacao (alvo_avg - fora_avg) entre as duas fases enquanto
mantem o falso positivo de baseline abaixo de um teto de seguranca.

Uso:
    python3 tools/validation/calibrate_thresholds.py logs/scenario-*.json
    python3 tools/validation/calibrate_thresholds.py  # usa todos os scenario-*.json de logs/
"""
import glob
import json
import os
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")

HOPPING_CHANNELS = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]
HOPPING_SET = set(HOPPING_CHANNELS)
NUM_CHANNELS = 126


def load_csv_rows(session_path, t_lo, t_hi):
    rows = []
    try:
        f = open(session_path)
    except FileNotFoundError:
        return rows
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("board") != "placa_a" or d.get("event") != "raw":
                continue
            ep = d.get("epoch")
            if ep is None or not (t_lo <= ep <= t_hi):
                continue
            raw = d.get("raw", "")
            if raw.count(",") < 100:
                continue
            vals = [int(x) for x in raw.split(",") if x.strip().lstrip("-").isdigit()]
            if len(vals) < NUM_CHANNELS:
                continue
            rows.append(vals[:NUM_CHANNELS])
    return rows


def detect_jam_mode(session_path, jam_lo, jam_hi):
    mode = "unknown"
    try:
        f = open(session_path)
    except FileNotFoundError:
        return mode
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            ep = d.get("epoch", 0)
            if jam_lo - 2 <= ep <= jam_hi and d.get("event") in ("jam_mode_sweep", "jam_mode_targeted"):
                mode = d["event"]
    return mode


def collect_phase_rows(scenario_path):
    with open(scenario_path) as f:
        scenario = json.load(f)
    phases = scenario["phases"]
    session_path = scenario["session_file"]
    # phases: baseline, jamming, recovery, end
    baseline_lo, baseline_hi = phases[0]["start_epoch"], phases[1]["start_epoch"]
    jam_lo, jam_hi = phases[1]["start_epoch"], phases[2]["start_epoch"]
    return {
        "baseline": load_csv_rows(session_path, baseline_lo, baseline_hi),
        "jamming": load_csv_rows(session_path, jam_lo, jam_hi),
        "jam_mode": detect_jam_mode(session_path, jam_lo, jam_hi),
        "path": scenario_path,
    }


def simulate_heat(rows, hit_threshold, decay_per_cycle):
    """Returns list of (alvo_avg, fora_avg) after each row, simulating
    channelHeat[] exactly like analisador/src/node.h's updateHeat()."""
    heat = [0] * NUM_CHANNELS
    out = []
    for vals in rows:
        for ch in range(NUM_CHANNELS):
            if vals[ch] > hit_threshold:
                heat[ch] = 100
            elif heat[ch] > 0:
                heat[ch] = max(0, heat[ch] - decay_per_cycle)
        alvo = [heat[ch] for ch in HOPPING_CHANNELS]
        fora = [heat[ch] for ch in range(NUM_CHANNELS) if ch not in HOPPING_SET]
        out.append((sum(alvo) / len(alvo), sum(fora) / len(fora)))
    return out


def raw_snapshot_stats(rows):
    """Old (pre-heat) behavior: alvo_avg/fora_avg per single snapshot."""
    out = []
    for vals in rows:
        alvo = [vals[ch] for ch in HOPPING_CHANNELS]
        fora = [vals[ch] for ch in range(NUM_CHANNELS) if ch not in HOPPING_SET]
        out.append((sum(alvo) / len(alvo), sum(fora) / len(fora)))
    return out


def summarize(label, series):
    if not series:
        print(f"  {label}: sem amostras")
        return None
    alvo = [a for a, _ in series]
    fora = [f for _, f in series]
    conc = [a - f for a, f in series]
    print(
        f"  {label}: n={len(series):4d}  alvo_avg(mean/max)={sum(alvo)/len(alvo):5.1f}/{max(alvo):5.1f}  "
        f"fora_avg(mean/max)={sum(fora)/len(fora):5.1f}/{max(fora):5.1f}  "
        f"concentracao(mean/min/max)={sum(conc)/len(conc):6.1f}/{min(conc):6.1f}/{max(conc):6.1f}"
    )
    return {"alvo_mean": sum(alvo) / len(alvo), "alvo_max": max(alvo),
            "fora_mean": sum(fora) / len(fora), "fora_max": max(fora),
            "conc_mean": sum(conc) / len(conc), "conc_min": min(conc), "conc_max": max(conc)}


def main():
    paths = sys.argv[1:] or sorted(glob.glob(os.path.join(LOGS_DIR, "scenario-*.json")))
    if not paths:
        print("Nenhum logs/scenario-*.json encontrado.", file=sys.stderr)
        sys.exit(1)

    all_baseline_rows = []
    all_jamming_rows = []
    print("=" * 78)
    print("DADOS BRUTOS POR SESSAO (snapshot unico, sem heat -- comportamento antigo)")
    print("=" * 78)
    for p in paths:
        phases = collect_phase_rows(p)
        print(f"\n{os.path.basename(p)}  [jam_mode={phases['jam_mode']}]")
        summarize("baseline (jammer off)", raw_snapshot_stats(phases["baseline"]))
        summarize("jamming  (jammer on) ", raw_snapshot_stats(phases["jamming"]))
        all_baseline_rows.append(phases["baseline"])
        # Só modo targeted mira nos 16 canais de HOPPING_CHANNELS -- sweep
        # (2-80 MHz) tem teto estrutural de ~50% em alvo_avg (só 8 dos 16
        # canais ficam na faixa varrida) e não deveria disparar o alerta
        # mesmo calibrado corretamente (ver ROADMAP.md). Incluir essas
        # sessões na calibração do LIMIAR_CONCENTRACAO distorceria o "pior
        # caso de jamming" pra baixo sem representar o cenário que o
        # alerta realmente precisa detectar.
        if phases["jam_mode"] == "jam_mode_targeted":
            all_jamming_rows.append(phases["jamming"])
        elif phases["jamming"]:
            print(f"  (jamming desta sessão excluído da calibração -- modo {phases['jam_mode']!r}, não targeted)")

    print()
    print("=" * 78)
    print("VARREDURA DE CALIBRACAO (HEAT_HIT_THRESHOLD x HEAT_DECAY_PER_CYCLE)")
    print("=" * 78)
    print(
        "Para cada combinacao, simula channelHeat[] em CADA sessao separadamente\n"
        "(o acumulador nao eh resetado entre sessoes -- cada uma comeca do zero,\n"
        "como o analisador comecaria numa sessao real) e reporta o PIOR caso\n"
        "(maior concentracao falsa em baseline, menor concentracao real em\n"
        "jamming) entre todas as sessoes fornecidas, para propor um limiar que\n"
        "generalize em vez de sobreajustar a uma unica sessao.\n"
    )

    best = None
    header = f"{'HIT':>4} {'DECAY':>6} | {'baseline conc max':>18} | {'jamming conc min':>17} | {'margem':>8}"
    print(header)
    print("-" * len(header))
    for hit_threshold in (10, 15, 20, 25, 30):
        for decay in (1, 2, 3, 5):
            baseline_worst_conc = float("-inf")
            jamming_worst_conc = float("inf")
            for rows in all_baseline_rows:
                if not rows:
                    continue
                series = simulate_heat(rows, hit_threshold, decay)
                conc = [a - f for a, f in series]
                baseline_worst_conc = max(baseline_worst_conc, max(conc))
            for rows in all_jamming_rows:
                if not rows:
                    continue
                series = simulate_heat(rows, hit_threshold, decay)
                conc = [a - f for a, f in series]
                jamming_worst_conc = min(jamming_worst_conc, max(conc))
            if baseline_worst_conc == float("-inf") or jamming_worst_conc == float("inf"):
                continue
            margin = jamming_worst_conc - baseline_worst_conc
            print(f"{hit_threshold:>4} {decay:>6} | {baseline_worst_conc:>18.1f} | {jamming_worst_conc:>17.1f} | {margin:>8.1f}")
            if best is None or margin > best[0]:
                best = (margin, hit_threshold, decay, baseline_worst_conc, jamming_worst_conc)

    print()
    if best:
        margin, hit_threshold, decay, baseline_worst, jamming_worst = best
        print(f"Melhor combinacao encontrada: HEAT_HIT_THRESHOLD={hit_threshold}, HEAT_DECAY_PER_CYCLE={decay}")
        print(f"  -> pior concentracao vista em baseline (deve ficar ABAIXO de LIMIAR_CONCENTRACAO): {baseline_worst:.1f}")
        print(f"  -> pior concentracao vista em jamming  (deve ficar ACIMA  de LIMIAR_CONCENTRACAO): {jamming_worst:.1f}")
        suggested_limiar_concentracao = baseline_worst + (jamming_worst - baseline_worst) * 0.4
        print(f"  -> LIMIAR_CONCENTRACAO sugerido (margem de seguranca sobre o pior falso positivo): {suggested_limiar_concentracao:.0f}")
    else:
        print("Nenhuma combinacao com dados suficientes para calibrar.")


if __name__ == "__main__":
    main()
