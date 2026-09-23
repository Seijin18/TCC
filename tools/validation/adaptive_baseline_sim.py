#!/usr/bin/env python3
"""
Prototype/validation (NOT wired into firmware yet): per-channel adaptive
baseline detector, simulated offline against real raw CSV from
tools/logger sessions, to check whether it separates ambient noise from
real jamming where the global 16-channel average (analisador/src/node.h
classifyAttack(), and the channelHeat[] fix) could not (see
openspec/changes/spectrum-attack-classification/design.md, "Achado de
calibração 2026-09-23").

Idea: instead of comparing a channel's activity against OTHER channels'
average (global threshold), compare it against ITS OWN recent history --
an EWMA-based per-channel baseline (slow-moving "what's normal here") and
mean-absolute-deviation (how noisy is "normal" for this channel), akin to
a temporal CFAR / EWMA control chart. A channel is flagged only when its
CURRENT reading deviates from its OWN baseline by several
deviation-units AND stays deviated for several consecutive sweeps
(persistence, filters single-sample bursts). Crucially, the baseline is
only updated (allowed to adapt) while the channel is NOT currently
flagged, so a sustained attack can't slowly get "learned" as the new
normal.

Usage:
    python3 tools/validation/adaptive_baseline_sim.py [--k K] [--window W] [--votes M] [--alpha A]
    (no args: uses all logs/scenario-*.json, prints a parameter sweep)
"""
import argparse
import glob
import json
import os

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")
HOPPING_CHANNELS = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]
NUM_CHANNELS = 126


def load_sequence(scenario_path):
    """Returns chronological list of (epoch, phase, vals[126]) from
    baseline start through jamming end."""
    with open(scenario_path) as f:
        scenario = json.load(f)
    phases = scenario["phases"]
    t0, jam_start, jam_end = phases[0]["start_epoch"], phases[1]["start_epoch"], phases[2]["start_epoch"]
    session_path = scenario["session_file"]
    out = []
    try:
        f = open(session_path)
    except FileNotFoundError:
        return out, jam_start
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("board") != "placa_a" or d.get("event") != "raw":
                continue
            ep = d.get("epoch")
            if ep is None or not (t0 <= ep <= jam_end):
                continue
            raw = d.get("raw", "")
            if raw.count(",") < 100:
                continue
            vals = [int(x) for x in raw.split(",") if x.strip().lstrip("-").isdigit()]
            if len(vals) < NUM_CHANNELS:
                continue
            phase = "baseline" if ep < jam_start else "jamming"
            out.append((ep, phase, vals[:NUM_CHANNELS]))
    return out, jam_start


class PerChannelDetector:
    """One instance per analisador session -- mirrors EXACTLY what's
    implemented in analisador/src/node.h (updateAdaptiveBaseline()) as
    parallel arrays indexed by channel. Must be kept byte-for-byte
    equivalent to the C version.

    Achado de processo (2026-09-23): uma primeira versão deste detector
    usava esta mesma lógica de janela deslizante e foi validada
    (63/432 combinações passaram). Ao portar para C, a janela literal
    (buffer circular de W booleans por canal) foi trocada por uma
    aproximação EWMA "pra economizar memória" -- SEM revalidar a
    aproximação. Rodado em bancada real, a máscara nunca saiu de 0x0000
    a sessão inteira. Rodando essa MESMA aproximação EWMA de volta nesta
    simulação confirmou exatamente o mesmo resultado (zero detecções) --
    ou seja, a aproximação não era equivalente, e o "custo de memória"
    que ela evitava (per-channel ring buffer, ~2.7KB pros 126 canais)
    é irrelevante nos 320KB de RAM do ESP32. Reversão: usar a janela
    literal também em C (ver analisador/src/node.h) -- nunca trocar um
    mecanismo já validado por uma aproximação não testada só por
    "eficiência" quando o recurso que ela economiza não é escasso.

    Persistência por JANELA DESLIZANTE (votos M-de-W), não por sweeps
    CONSECUTIVOS: medido que o sinal real do jammer por canal também é
    intermitente (o jammer visita os outros 15 canais entre uma
    passagem e outra por este), então exigir sweeps consecutivos
    descartava o sinal real junto com falsos positivos de rajadas
    isoladas (ex.: um dispositivo ligando por ~3s). Contar "M candidatos
    dentre os últimos W sweeps" separa rajada isolada (poucos votos,
    concentrados, depois nada) de ataque real (votos recorrentes ao
    longo de toda a janela de ataque)."""

    def __init__(self, alpha=0.1, k=4.0, window=10, votes=3, min_abs_dev=15.0, min_dev_floor=3.0):
        self.alpha = alpha
        self.k = k
        self.window = window
        self.votes = votes
        self.min_abs_dev = min_abs_dev  # channel must ALSO be this many points above baseline in absolute terms
        self.baseline = [0.0] * NUM_CHANNELS
        self.mad = [min_dev_floor] * NUM_CHANNELS  # mean absolute deviation, floored to avoid div-by-~0
        self.vote_history = [[] for _ in range(NUM_CHANNELS)]  # ring buffer of bool, len<=window
        self.flagged = [False] * NUM_CHANNELS
        self.min_dev_floor = min_dev_floor
        self.primed = [False] * NUM_CHANNELS

    def update(self, vals):
        newly_flagged_this_row = []
        for ch in range(NUM_CHANNELS):
            v = vals[ch]
            if not self.primed[ch]:
                self.baseline[ch] = v
                self.mad[ch] = self.min_dev_floor
                self.primed[ch] = True
                continue

            dev = v - self.baseline[ch]
            score = dev / self.mad[ch]
            candidate = (score > self.k) and (dev > self.min_abs_dev)

            hist = self.vote_history[ch]
            hist.append(candidate)
            if len(hist) > self.window:
                hist.pop(0)
            self.flagged[ch] = sum(hist) >= self.votes

            # Só deixa a baseline se adaptar quando o canal NAO esta
            # marcado como comprometido -- senao um ataque sustentado
            # seria lentamente "aprendido" como normal.
            if not self.flagged[ch]:
                self.baseline[ch] += self.alpha * (v - self.baseline[ch])
                self.mad[ch] = max(self.min_dev_floor, self.mad[ch] + self.alpha * (abs(v - self.baseline[ch]) - self.mad[ch]))

            if self.flagged[ch]:
                newly_flagged_this_row.append(ch)
        return newly_flagged_this_row

    def hopping_flagged_count(self):
        return sum(1 for ch in HOPPING_CHANNELS if self.flagged[ch])


def run_one(scenario_path, alpha, k, window, votes, min_abs_dev):
    seq, jam_start = load_sequence(scenario_path)
    if not seq:
        return None
    det = PerChannelDetector(alpha=alpha, k=k, window=window, votes=votes, min_abs_dev=min_abs_dev)
    baseline_false_positive_channels = set()
    baseline_max_hopping_flagged = 0
    jamming_true_positive_channels = set()
    jamming_max_hopping_flagged = 0
    jamming_any_flagged_rows = 0
    jamming_rows = 0
    for ep, phase, vals in seq:
        flagged_now = det.update(vals)
        hop_flagged = det.hopping_flagged_count()
        if phase == "baseline":
            for ch in flagged_now:
                if ch in HOPPING_CHANNELS:
                    baseline_false_positive_channels.add(ch)
            baseline_max_hopping_flagged = max(baseline_max_hopping_flagged, hop_flagged)
        else:
            jamming_rows += 1
            if hop_flagged > 0:
                jamming_any_flagged_rows += 1
            for ch in flagged_now:
                if ch in HOPPING_CHANNELS:
                    jamming_true_positive_channels.add(ch)
            jamming_max_hopping_flagged = max(jamming_max_hopping_flagged, hop_flagged)
    return {
        "path": os.path.basename(scenario_path),
        "baseline_fp_channels": sorted(baseline_false_positive_channels),
        "baseline_max_hop_flagged": baseline_max_hopping_flagged,
        "jamming_tp_channels": sorted(jamming_true_positive_channels),
        "jamming_max_hop_flagged": jamming_max_hopping_flagged,
        "jamming_rows_with_any_flag_pct": (100.0 * jamming_any_flagged_rows / jamming_rows) if jamming_rows else 0.0,
    }


def is_targeted(scenario_path):
    with open(scenario_path) as f:
        scenario = json.load(f)
    jam_start = scenario["phases"][1]["start_epoch"]
    jam_end = scenario["phases"][2]["start_epoch"]
    session_path = scenario["session_file"]
    try:
        f = open(session_path)
    except FileNotFoundError:
        return False
    with f:
        for line in f:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            ep = d.get("epoch", 0)
            if jam_start - 2 <= ep <= jam_end and d.get("event") == "jam_mode_targeted":
                return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--alpha", type=float, default=None)
    ap.add_argument("--k", type=float, default=None)
    ap.add_argument("--window", type=int, default=None)
    ap.add_argument("--votes", type=int, default=None)
    ap.add_argument("--min-abs-dev", type=float, default=None)
    ap.add_argument("scenarios", nargs="*")
    args = ap.parse_args()

    paths = args.scenarios or sorted(glob.glob(os.path.join(LOGS_DIR, "scenario-*.json")))
    paths = [p for p in paths if is_targeted(p)]
    if not paths:
        print("Nenhum scenario targeted encontrado.")
        return

    if args.alpha is not None:
        # single explicit run, verbose per-session
        for p in paths:
            r = run_one(p, args.alpha, args.k or 4.0, args.window or 10, args.votes or 3, args.min_abs_dev or 15.0)
            print(json.dumps(r, indent=2, ensure_ascii=False))
        return

    print("=" * 100)
    print(f"Sessões targeted usadas: {[os.path.basename(p) for p in paths]}")
    print("=" * 100)
    header = f"{'alpha':>6} {'k':>4} {'win':>4} {'votes':>5} {'min_abs':>7} | {'fp (deveria ser [])':>40} | {'# canais-alvo detectados (uniao)':>10}"
    print(header)
    print("-" * len(header))
    best = None
    for alpha in (0.1, 0.15):
        for k in (2.0, 2.5, 3.0, 4.0):
            for window in (10, 15, 20, 30, 45, 60, 90):
                for votes in (1, 2, 3, 4, 5):
                    if votes > window:
                        continue
                    for min_abs_dev in (10.0, 12.0, 15.0):
                        worst_fp = set()
                        worst_baseline_count = 0
                        tp_union = set()
                        for p in paths:
                            r = run_one(p, alpha, k, window, votes, min_abs_dev)
                            if r is None:
                                continue
                            worst_fp |= set(r["baseline_fp_channels"])
                            worst_baseline_count = max(worst_baseline_count, r["baseline_max_hop_flagged"])
                            tp_union |= set(r["jamming_tp_channels"])
                        row = (
                            f"{alpha:>6} {k:>4} {window:>4} {votes:>5} {min_abs_dev:>7} | "
                            f"{str(sorted(worst_fp)):>40} | {len(tp_union):>10} {sorted(tp_union)}"
                        )
                        if worst_baseline_count == 0 and len(tp_union) > 0:
                            print("PASS  " + row)
                            if best is None or len(tp_union) > best[0]:
                                best = (len(tp_union), alpha, k, window, votes, min_abs_dev, sorted(tp_union))
    if best:
        n, alpha, k, window, votes, min_abs_dev, chans = best
        print()
        print(f"Melhor parametrizacao (zero falso positivo em baseline, mais canais-alvo detectados: {n}):")
        print(f"  alpha={alpha} k={k} window={window} votes={votes} min_abs_dev={min_abs_dev}")
        print(f"  canais detectados: {chans}")
    else:
        print("\nNenhuma combinacao com ZERO falso positivo em baseline encontrada.")


if __name__ == "__main__":
    main()
