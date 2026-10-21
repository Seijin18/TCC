#!/usr/bin/env python3
"""
diag_battery -- bateria DIAGNOSTICA reutilizavel (nao altera firmware).

Cada subcomando escreve logs/diag-<teste>-<UTCstamp>.json e imprime um
resumo compacto.

  t2-offline <session.jsonl>   deriva de relogio (regressao t_ms x epoch por
                               janela entre time_sync) -- sem bancada
  t3-offline <battery.json>    linha do tempo de alert_on/off + chmap por
                               fase da bateria -- sem bancada
  t3-run                       (bancada, ~4 min) off / hold ch70 / recovery
  t2-run                       (bancada, 10 min) soak sem interferencia
  t1-run                       (bancada, ate ~25 min) reproducao do
                               handshake half-open + protocolo de isolamento

Bancada: nao abre portas seriais; so escreve em logs/cmd.in e le
logs/latest.jsonl (o logger tools/logger/main.py precisa estar rodando --
NUNCA reinicia-lo: reiniciar reseta a placa_a).

Se o enlace ja estiver travado (sem msg_done) ao iniciar um subcomando de
bancada, o protocolo de isolamento (regs -> reset so placa_b -> reset so
placa_a -> reset ambos) roda ANTES do teste e e registrado em
"preflight_isolation".
"""
import argparse
import bisect
import json
import math
import os
import sys
import time
from collections import Counter, namedtuple
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_scenario import CMD_PATH, LATEST_PATH, LOGS_DIR, read_state, require_logger_running  # noqa: E402
from repair_trials import JsonlTailer  # noqa: E402

HOP_CHANNELS = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]
SPECTRUM_PATH = os.path.join(LOGS_DIR, "spectrum.csv")
BOARD_OF = {"mestre": "placa_a", "analisador": "placa_a", "escravo": "placa_b", "jammer": "placa_b"}
SRC_OF = {"placa_a": "mestre", "placa_b": "escravo"}
ANOM_KEYS = ("rst:", "Guru", "Backtrace", "abort()", "assert failed", "Brownout", "Rebooting", "panic", "Fatal exception")
MITIG_EVENTS = ("blacklist", "rekey_tx", "rekey_ack", "rekey_apply", "rekey_failed", "pair_mask")

Rec = namedtuple("Rec", "epoch src event t_ms f raw")


# ----------------------------------------------------------------- helpers
def to_rec(d):
    """Reduz um registro JSONL a uma tupla leve (guarda so o necessario)."""
    ep = d.get("epoch")
    if ep is None:
        return None
    src = d.get("src")
    ev = d.get("event")
    if src is None:
        raw = d.get("raw") or ""
        if "[JAMMER]" in raw or any(k in raw for k in ANOM_KEYS):
            return Rec(ep, "raw", "raw", None, {"board": d.get("board")}, raw)
        return None
    f = d.get("fields") or {}
    if ev in ("hop", "ack"):
        f = {"ch": f.get("ch")}
    t = d.get("t_ms")
    return Rec(ep, src, ev, t if isinstance(t, int) else None, f, None)


def utc_stamp():
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")


def hms(ep):
    return datetime.fromtimestamp(ep, timezone.utc).strftime("%H:%M:%S")


def write_report(test, report):
    path = os.path.join(LOGS_DIR, f"diag-{test}-{utc_stamp()}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, default=str)
    print(f"[diag] relatorio: {path}")
    return path


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def r2(x, n=2):
    return None if x is None else round(x, n)


def compress(seq):
    out = []
    for v in seq:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return ",".join(f"{v}x{n}" for v, n in out)


def longest_run_ge1(seq):
    best = cur = 0
    for v in seq:
        if v >= 1:
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    return best


def pearson(xs, ys):
    pts = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    n = len(pts)
    if n < 3:
        return None
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    syy = sum((p[1] - my) ** 2 for p in pts)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pts)
    if sxx <= 0 or syy <= 0:
        return None
    return sxy / math.sqrt(sxx * syy)


def linfit(xs, ys):
    """Regressao y = a + b x. Retorna n, slope, resid_std, slope_se."""
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx <= 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    a = my - b * mx
    ss = sum((y - a - b * x) ** 2 for x, y in zip(xs, ys))
    sd = math.sqrt(ss / (n - 2)) if n > 2 else 0.0
    return {"n": n, "slope_ms_per_s": b, "resid_std_ms": sd, "slope_se": sd / math.sqrt(sxx)}


def load_session_recs(path, prefilter=None):
    recs = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if prefilter is not None and not prefilter(line):
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            r = to_rec(d)
            if r is not None:
                recs.append(r)
    return recs


# ------------------------------------------------------------ t2 analysis
def analyze_t2(recs, lo=None, hi=None):
    recs = [r for r in recs if (lo is None or r.epoch >= lo) and (hi is None or r.epoch <= hi)]
    if not recs:
        return {"error": "sem registros no intervalo"}
    t0 = lo if lo is not None else recs[0].epoch
    t1 = hi if hi is not None else recs[-1].epoch

    ser = {"mestre": ([], []), "escravo": ([], [])}
    for r in recs:
        if r.src in ser and r.t_ms is not None:
            ser[r.src][0].append(r.epoch)
            ser[r.src][1].append(r.t_ms)

    ser_md = {"mestre": ([], []), "escravo": ([], [])}
    for r in recs:
        if r.src in ser_md and r.event == "msg_done" and r.t_ms is not None:
            ser_md[r.src][0].append(r.epoch)
            ser_md[r.src][1].append(r.t_ms)

    ts_m = sorted(r.epoch for r in recs if r.src == "mestre" and r.event == "time_sync")
    loss = sorted((r.epoch, r.src) for r in recs if r.event == "loss_of_sync" and r.src in ("mestre", "escravo"))
    reanch = sorted((r.epoch, r.f.get("master")) for r in recs if r.src == "escravo" and r.event == "reanchor")

    starts = []
    if not ts_m or ts_m[0] - t0 > 5.0:
        starts.append((t0, "range_start"))
    starts += [(e, "time_sync") for e in ts_m]

    windows = []
    for i, (s, why) in enumerate(starts):
        nxt = starts[i + 1][0] if i + 1 < len(starts) else t1
        first_loss = next((e for e, _ in loss if e > s), None)
        if first_loss is not None and first_loss < nxt:
            we, end_reason = first_loss, "loss_of_sync"
        else:
            we, end_reason = nxt, ("next_time_sync" if i + 1 < len(starts) else "range_end")
        w = {
            "idx": i, "start_epoch": s, "start_rel_s": s - t0, "start_reason": why,
            "end_epoch": we, "end_reason": end_reason, "dur_s": we - s, "full_span_s": nxt - s,
        }
        fits = {}
        for name, (eps, tms) in ser.items():
            a, b = bisect.bisect_left(eps, s), bisect.bisect_right(eps, we)
            xs = [e - s for e in eps[a:b]]
            ys = [t - tms[a] for t in tms[a:b]]
            fits[name] = linfit(xs, ys) if b - a >= 3 else None
        w["fit_mestre"], w["fit_escravo"] = fits["mestre"], fits["escravo"]
        if fits["mestre"] and fits["escravo"]:
            r_ppm = (fits["mestre"]["slope_ms_per_s"] - fits["escravo"]["slope_ms_per_s"]) / 1000.0 * 1e6
            se = math.sqrt(fits["mestre"]["slope_se"] ** 2 + fits["escravo"]["slope_se"] ** 2) / 1000.0 * 1e6
            w["r_ppm"], w["r_se_ppm_naive"] = r_ppm, se
            w["pred_15ms_s"] = 15.0 / abs(r_ppm) * 1e3 if r_ppm else None
            w["pred_30ms_s"] = 30.0 / abs(r_ppm) * 1e3 if r_ppm else None
        # estimativa alternativa: so msg_done (mesmo tipo de linha nos dois lados)
        fits2 = {}
        for name, (eps, tms) in ser_md.items():
            a, b = bisect.bisect_left(eps, s), bisect.bisect_right(eps, we)
            fits2[name] = linfit([e - s for e in eps[a:b]], [t - tms[a] for t in tms[a:b]]) if b - a >= 3 else None
        if fits2["mestre"] and fits2["escravo"]:
            w["r_ppm_msgdone_only"] = (fits2["mestre"]["slope_ms_per_s"] - fits2["escravo"]["slope_ms_per_s"]) / 1000.0 * 1e6
            w["msgdone_slopes"] = [fits2["mestre"]["slope_ms_per_s"], fits2["escravo"]["slope_ms_per_s"]]
            w["msgdone_resid_sd_ms"] = [fits2["mestre"]["resid_std_ms"], fits2["escravo"]["resid_std_ms"]]
        w["loss_rel_s"] = [(src, round(e - s, 2)) for e, src in loss if s < e <= nxt]
        w["reanchor_rel_s"] = [(round(e - s, 2), m) for e, m in reanch if s <= e <= nxt]
        windows.append(w)

    # buckets de 60 s
    buckets = []
    nb = int(math.ceil((t1 - t0) / 60.0))
    fps = [[] for _ in range(nb)]
    plr = [[] for _ in range(nb)]
    rean = [0] * nb
    lossn = [0] * nb
    miss = [0] * nb

    def bi(e):
        return min(nb - 1, max(0, int((e - t0) // 60)))

    for r in recs:
        if nb == 0:
            break
        if r.src == "mestre" and r.event == "throughput":
            try:
                fps[bi(r.epoch)].append(float(r.f.get("frags_per_s")))
            except (TypeError, ValueError):
                pass
        elif r.src == "escravo" and r.event == "plr":
            try:
                plr[bi(r.epoch)].append(float(r.f.get("window")))
            except (TypeError, ValueError):
                pass
        elif r.src == "escravo" and r.event == "reanchor":
            rean[bi(r.epoch)] += 1
        elif r.event == "loss_of_sync" and r.src in ("mestre", "escravo"):
            lossn[bi(r.epoch)] += 1
        elif r.src == "mestre" and r.event == "miss":
            miss[bi(r.epoch)] += 1
    for k in range(nb):
        mid = t0 + 60 * k + 30
        wi = None
        for w in windows:
            if w["start_epoch"] <= mid <= w["start_epoch"] + w["full_span_s"]:
                wi = w["idx"]
        buckets.append({
            "bucket": k, "t_rel_s": 60 * k, "window_idx": wi,
            "fps_mean": r2(mean(fps[k])), "fps_n": len(fps[k]),
            "plr_win_mean_pct": r2(mean(plr[k])), "plr_win_max_pct": r2(max(plr[k])) if plr[k] else None,
            "reanchor": rean[k], "loss_of_sync": lossn[k], "mestre_miss": miss[k],
        })
    return {"t0": t0, "t1": t1, "span_s": t1 - t0, "windows": windows, "buckets": buckets,
            "loss_of_sync_all": [(round(e - t0, 2), src) for e, src in loss],
            "reanchor_all": [(round(e - t0, 2), m) for e, m in reanch]}


def print_t2(res):
    if "error" in res:
        print("[t2]", res["error"])
        return
    print(f"[t2] intervalo {hms(res['t0'])} -> {hms(res['t1'])} ({res['span_s']:.0f} s)")
    print("  win  start_rel  dur_s  end_reason       n_m    n_e   slope_m   slope_e   r_ppm  (se~)  sd_m  sd_e  r_md  t15ms_s  t30ms_s  losses(rel s)  reanchors")
    for w in res["windows"]:
        fm, fe = w["fit_mestre"], w["fit_escravo"]
        if not (fm and fe):
            print(f"  {w['idx']:>3}  {w['start_rel_s']:>8.1f}  {w['dur_s']:>5.1f}  {w['end_reason']:<15}  (sem pontos suficientes)")
            continue
        loss = ";".join(f"{s[0]}{t}" for s, t in [(x[0], x[1]) for x in w["loss_rel_s"]])
        print(
            f"  {w['idx']:>3}  {w['start_rel_s']:>8.1f}  {w['dur_s']:>5.1f}  {w['end_reason']:<15}  {fm['n']:>5} {fe['n']:>5}"
            f"  {fm['slope_ms_per_s']:>8.4f}  {fe['slope_ms_per_s']:>8.4f}  {w['r_ppm']:>+7.1f} ({w['r_se_ppm_naive']:.1f})"
            f"  {fm['resid_std_ms']:>4.1f}  {fe['resid_std_ms']:>4.1f}  {r2(w.get('r_ppm_msgdone_only'), 1)!s:>6}  {r2(w.get('pred_15ms_s'), 0)!s:>7}  {r2(w.get('pred_30ms_s'), 0)!s:>7}"
            f"  {loss or '-'}  {len(w['reanchor_rel_s'])}"
        )
    print("  bucket t_rel win  fps   plr%(mean/max)  reanchor loss miss")
    for b in res["buckets"]:
        print(
            f"  {b['bucket']:>5} {b['t_rel_s']:>5} {b['window_idx']!s:>3} {b['fps_mean']!s:>6} "
            f"{b['plr_win_mean_pct']!s:>6}/{b['plr_win_max_pct']!s:<6} {b['reanchor']:>5} {b['loss_of_sync']:>5} {b['mestre_miss']:>5}"
        )
    print(f"  loss_of_sync (rel s): {res['loss_of_sync_all']}")


# --------------------------------------------------------- alert timeline
def alert_timeline(recs):
    """Pares alert_on -> proximo alert_off (mesmo src)."""
    out = []
    open_on = {}
    for r in sorted((r for r in recs if r.event in ("alert_on", "alert_off") and r.src in ("analisador", "mestre")),
                    key=lambda r: r.epoch):
        if r.event == "alert_on":
            ent = {"src": r.src, "on": r.epoch, "off": None, "count": r.f.get("count"), "mask": r.f.get("mask")}
            out.append(ent)
            open_on[r.src] = ent
        else:
            ent = open_on.pop(r.src, None)
            if ent is not None:
                ent["off"] = r.epoch
    return out


def alert_outstanding_at(timeline, src, t):
    for e in timeline:
        if e["src"] == src and e["on"] <= t and (e["off"] is None or e["off"] > t):
            return True
    return False


def phase_alert_chmap(recs, s, e, tl):
    """Eventos de alerta + sequencia de chmap.count numa fase [s, e)."""
    edges = [(round(r.epoch - s, 2), r.src, r.event, r.f.get("count"), r.f.get("mask"))
             for r in recs if s <= r.epoch < e and r.event in ("alert_on", "alert_off") and r.src in ("analisador", "mestre")]
    counts = []
    for r in recs:
        if s <= r.epoch < e and r.src == "analisador" and r.event == "chmap":
            try:
                counts.append(int(r.f.get("count")))
            except (TypeError, ValueError):
                pass
    return {
        "alert_edges": edges,
        "alert_edges_by_src": dict(Counter(x[1] for x in edges)),
        "outstanding_at_start": {src: alert_outstanding_at(tl, src, s) for src in ("analisador", "mestre")},
        "outstanding_at_end": {src: alert_outstanding_at(tl, src, e) for src in ("analisador", "mestre")},
        "chmap_n": len(counts),
        "chmap_seq": compress(counts),
        "chmap_longest_run_ge1": longest_run_ge1(counts),
        "chmap_frac_ge1": r2(sum(1 for c in counts if c >= 1) / len(counts), 3) if counts else None,
    }


# ------------------------------------------------------------- t2-offline
def cmd_t2_offline(a):
    path = a.session
    print(f"[t2-offline] lendo {path} ...")
    recs = load_session_recs(path, prefilter=lambda l: '"src"' in l)
    first = recs[0].epoch
    jam = [r.epoch for r in recs if r.src == "jammer" and r.event in ("jam_state_on", "jam_mode_hold", "jam_mode_sweep", "jam_mode_targeted", "jam_mode_adaptive")]
    lo = a.start if a.start is not None else first
    if a.end is not None:
        hi = a.end
    elif jam and not a.all:
        hi = min(jam)
    else:
        hi = recs[-1].epoch
    print(f"[t2-offline] sessao {hms(first)}..{hms(recs[-1].epoch)}; primeiro evento jammer: {hms(min(jam)) if jam else '-'}; analisando {hms(lo)}..{hms(hi)}")
    res = analyze_t2(recs, lo, hi)
    res.update({"session": path, "first_jammer_event_epoch": min(jam) if jam else None})
    print_t2(res)
    write_report("t2-offline", res)


# ------------------------------------------------------------- t3-offline
def cmd_t3_offline(a):
    bat = json.load(open(a.battery))
    sess = a.session or bat.get("session_file")
    if not sess or not os.path.exists(sess):
        sess = os.path.join(LOGS_DIR, "session-20261021-054356.jsonl")
    print(f"[t3-offline] sessao: {sess}")
    recs = load_session_recs(sess, prefilter=lambda l: ("alert_" in l) or ("chmap" in l))
    recs = [r for r in recs if r.src in ("analisador", "mestre")]
    tl = alert_timeline(recs)
    t_first = recs[0].epoch if recs else 0
    t_last = recs[-1].epoch if recs else 0
    print(f"[t3-offline] {len(recs)} registros ({hms(t_first)}..{hms(t_last)})")
    print("  alert edges (sessao inteira), epoch / hh:mm:ss UTC / src / evento:")
    edges = sorted(((r.epoch, r.src, r.event, r.f.get("count"), r.f.get("mask")) for r in recs if r.event in ("alert_on", "alert_off")))
    for e in edges:
        print(f"   {e[0]:.2f} {hms(e[0])} {e[1]:<10} {e[2]:<9} count={e[3]} mask={e[4]}")
    print("  alert_on e o off seguinte (None = pendente/outstanding):")
    for ent in tl:
        print(f"   {ent['src']:<10} on={ent['on']:.2f} ({hms(ent['on'])})  off={ent['off'] if ent['off'] is None else round(ent['off'], 2)}"
              f"  dur={'-' if ent['off'] is None else round(ent['off'] - ent['on'], 1)}s")
    outstanding_end = {src: alert_outstanding_at(tl, src, 1e12) for src in ("analisador", "mestre")}
    print(f"  pendente ao fim da sessao: {outstanding_end}")

    phases = bat["phases"]
    end_all = datetime.fromisoformat(bat["ended_at"]).timestamp()
    rows = []
    for i, p in enumerate(phases):
        s = p["start_epoch"]
        e = phases[i + 1]["start_epoch"] if i + 1 < len(phases) else end_all
        ph = phase_alert_chmap(recs, s, e, tl)
        ph.update({"scenario": p["scenario"], "phase": p["name"], "start_epoch": s, "end_epoch": e, "dur_s": e - s})
        rows.append(ph)
        print(f"  [{p['scenario']}/{p['name']}] {hms(s)} dur={e - s:.0f}s  chmap n={ph['chmap_n']} maxrun>=1={ph['chmap_longest_run_ge1']}"
              f" frac>=1={ph['chmap_frac_ge1']}  edges={ph['alert_edges_by_src']}  outstanding_start={ph['outstanding_at_start']}")
        print(f"      seq: {ph['chmap_seq']}")
        for ed in ph["alert_edges"]:
            print(f"      edge +{ed[0]}s {ed[1]} {ed[2]} count={ed[3]} mask={ed[4]}")
    outstanding_during_battery = {
        src: alert_outstanding_at(tl, src, phases[0]["start_epoch"]) for src in ("analisador", "mestre")
    }
    print(f"  alerta pendente NO INICIO da bateria: {outstanding_during_battery}")
    write_report("t3-offline", {
        "battery": a.battery, "session": sess, "alert_edges_session": edges, "alert_pairs": tl,
        "outstanding_at_session_end": outstanding_end, "outstanding_at_battery_start": outstanding_during_battery,
        "phases": rows,
    })


# ------------------------------------------------------------------ Bench
class Bench:
    """Envia comandos via cmd.in, segue logs/latest.jsonl a partir de agora."""

    def __init__(self):
        require_logger_running()
        self.tailer = JsonlTailer(LATEST_PATH)
        self.recs = []
        self.cmd_log = []
        self.t_start = time.time()
        try:
            self.spec_off = os.path.getsize(SPECTRUM_PATH)
        except OSError:
            self.spec_off = None
        self.pump()

    # -- leitura
    def pump(self):
        for d in self.tailer.poll():
            r = to_rec(d)
            if r is not None:
                self.recs.append(r)

    def sleep(self, secs, tick=0.2):
        end = time.time() + secs
        while time.time() < end:
            self.pump()
            time.sleep(min(tick, max(0.0, end - time.time())))
        self.pump()

    def mark(self):
        self.pump()
        return len(self.recs)

    # -- comandos
    @staticmethod
    def ack_rule(board, cmd):
        src = SRC_OF[board]
        if cmd in ("mode fixed", "mode hopping"):
            ev = cmd.replace(" ", "_")
            return lambda r: r.src == src and r.event == ev
        if cmd in ("mitigation on", "mitigation off") and board == "placa_a":
            ev = cmd.replace(" ", "_")
            return lambda r: r.src == "mestre" and r.event == ev
        if cmd == "reset":
            return lambda r: r.src == src and r.event in ("pair_scan", "boot")
        if cmd == "regs":
            return lambda r: r.src == src and r.event == "regs" and r.f.get("why") == "cmd"
        if board == "placa_b":
            if cmd == "jam on":
                return lambda r: r.src == "jammer" and r.event == "jam_state_on"
            if cmd == "jam off":
                return lambda r: r.src == "jammer" and r.event == "jam_state_off"
            if cmd.startswith("mode hold"):
                return lambda r: r.src == "jammer" and r.event == "jam_mode_hold"
            if cmd.startswith("mode ") and cmd.split()[1] in ("sweep", "targeted", "adaptive"):
                ev = "jam_mode_" + cmd.split()[1]
                return lambda r: r.src == "jammer" and r.event == ev
            if cmd.startswith("power "):
                key = "Potencia: " + cmd.split()[1].upper()
                return lambda r: r.src == "raw" and key in (r.raw or "")
        return None

    def send(self, board, cmd, ack_timeout=4.0):
        rule = self.ack_rule(board, cmd)
        entry = {"epoch": None, "board": board, "cmd": cmd, "acked": None, "attempts": 0}
        for attempt in (1, 2):
            self.pump()
            i0 = len(self.recs)
            t = time.time()
            with open(CMD_PATH, "a", encoding="utf-8") as f:
                f.write(f"{board} {cmd}\n")
            entry["attempts"] = attempt
            if entry["epoch"] is None:
                entry["epoch"] = t
            if rule is None:
                entry["acked"] = None
                break
            acked = False
            while time.time() < t + ack_timeout:
                self.pump()
                if any(rule(r) for r in self.recs[i0:]):
                    acked = True
                    break
                time.sleep(0.1)
            entry["acked"] = acked
            if acked:
                break
        self.cmd_log.append(entry)
        flag = "" if entry["acked"] in (True, None) else "  ** SEM ACK **"
        print(f"[cmd] {board} {cmd}{flag}")
        return entry["acked"]

    # -- estado do enlace
    def link_up(self, idx, t0, require_resync):
        """Epoch em que o enlace voltou, ou None. Criterio: time_sync nos dois
        lados (depois de t0 / da ultima loss_of_sync do lado) E msg_done nos dois
        lados depois disso, e ainda fluindo agora."""
        ts = {"mestre": [], "escravo": []}
        md = {"mestre": [], "escravo": []}
        loss = {}
        for r in self.recs[idx:]:
            if r.epoch < t0 or r.src not in ts:
                continue
            if r.event == "time_sync":
                ts[r.src].append(r.epoch)
            elif r.event == "msg_done":
                md[r.src].append(r.epoch)
            elif r.event == "loss_of_sync":
                loss[r.src] = r.epoch
        ref = {}
        for s in ("mestre", "escravo"):
            ref_s = t0
            need = require_resync
            if s in loss:
                need, ref_s = True, loss[s]
            if need:
                after = [e for e in ts[s] if e > ref_s]
                if not after:
                    return None
                ref_s = after[0]
            ref[s] = ref_s
        base = max(ref.values())
        firsts = []
        for s in md:
            ok = [x for x in md[s] if x > base + 0.3]
            if not ok:
                return None
            firsts.append(ok[0])
            if time.time() - md[s][-1] > 3.0:
                return None
        return max(firsts)

    def wait_link(self, idx, t0, timeout, require_resync):
        end = time.time() + timeout
        while True:
            self.pump()
            up = self.link_up(idx, t0, require_resync)
            if up is not None:
                return up
            if time.time() >= end:
                return None
            time.sleep(0.2)

    def flowing_now(self, secs=3.0):
        """msg_done nos dois lados nos ultimos `secs` segundos?"""
        self.sleep(secs + 0.5)
        now = time.time()
        got = {"mestre": False, "escravo": False}
        for r in reversed(self.recs):
            if now - r.epoch > secs:
                break
            if r.event == "msg_done" and r.src in got:
                got[r.src] = True
        return all(got.values())

    def clean_repair(self, timeout=30.0):
        self.send("placa_b", "jam off")
        i0 = self.mark()
        t0 = time.time()
        self.send("placa_a", "reset")
        self.send("placa_b", "reset")
        up = self.wait_link(i0, t0, timeout, True)
        print(f"[repair] link up em {None if up is None else round(up - t0, 1)} s")
        return None if up is None else up - t0

    # -- protocolo de isolamento
    def isolate(self, where):
        now = time.time()
        rec = {"where": where, "epoch": now}
        ms = [r for r in self.recs if r.src == "mestre" and r.event == "pair_stats"][-2:]
        rec["mestre_pair_stats_last2"] = [dict(rel_s=round(r.epoch - now, 1), **r.f) for r in ms]
        es = [r for r in self.recs if r.src == "escravo" and r.event == "pair_stats"][-1:]
        rec["escravo_pair_stats_last"] = [dict(rel_s=round(r.epoch - now, 1), **r.f) for r in es]
        win = [r for r in self.recs if now - 10 <= r.epoch <= now]
        rec["escravo_last10s"] = {k: sum(1 for r in win if r.src == "escravo" and r.event == k)
                                  for k in ("pair_ok", "pair_reack", "pair_timeout", "pair_scan", "loss_of_sync", "time_sync", "msg_done")}
        rec["mestre_last10s"] = {k: sum(1 for r in win if r.src == "mestre" and r.event == k)
                                 for k in ("pair_ok", "pair_scan", "loss_of_sync", "time_sync", "msg_done", "pair_stats")}
        # compact: non-bulk events in last 30 s
        bulk = {"hop", "ack", "chmap", "miss", "throughput", "plr", "msg_done", "pair_reack"}
        rec["events_last30s"] = dict(Counter(f"{r.src}.{r.event}" for r in self.recs
                                             if now - 30 <= r.epoch <= now and r.event not in bulk and r.src != "raw"))
        # b. regs
        i0 = self.mark()
        t = time.time()
        self.send("placa_a", "regs")
        self.send("placa_b", "regs")
        self.sleep(2.0)
        rec["regs_cmd"] = [dict(src=r.src, rel_s=round(r.epoch - t, 2), **r.f) for r in self.recs[i0:]
                           if r.event == "regs" and r.src in ("mestre", "escravo")]
        # c/d/e
        steps = []
        resolved = None
        for name, boards, timeout in (("c_reset_b_only", ["placa_b"], 15.0), ("d_reset_a_only", ["placa_a"], 15.0),
                                      ("e_reset_both", ["placa_a", "placa_b"], 30.0)):
            i1 = self.mark()
            t1 = time.time()
            for bd in boards:
                self.send(bd, "reset")
            up = self.wait_link(i1, t1, timeout, True)
            steps.append({"step": name, "wait_s": timeout, "up": up is not None,
                          "t_to_up_s": None if up is None else round(up - t1, 2)})
            print(f"[isolate:{where}] {name}: {'UP em %.1fs' % (up - t1) if up else 'ainda travado'}")
            if up is not None:
                resolved = name
                break
        rec["steps"] = steps
        rec["resolved_by"] = resolved
        return rec

    def preflight(self):
        """Se o enlace ja esta travado, roda o isolamento e devolve o registro."""
        if self.flowing_now():
            print("[preflight] enlace fluindo.")
            return None
        print("[preflight] enlace NAO esta fluindo -> protocolo de isolamento (travamento pre-existente).")
        return self.isolate("preexisting")

    # -- relatorio comum
    def anomalies(self):
        out = []
        for r in self.recs:
            if r.src == "raw":
                out.append({"rel_s": round(r.epoch - self.t_start, 1), "kind": "raw", "text": (r.raw or "")[:160]})
            elif r.event == "boot":
                out.append({"rel_s": round(r.epoch - self.t_start, 1), "kind": "boot_event", "src": r.src})
        unacked = [c for c in self.cmd_log if c["acked"] is False]
        return {"crash_or_boot_lines": [x for x in out if "[JAMMER]" not in x.get("text", "")], "unacked_commands": unacked,
                "n_commands": len(self.cmd_log)}

    def spectrum_rows(self):
        rows = []
        if self.spec_off is None:
            return rows
        with open(SPECTRUM_PATH, "rb") as fh:
            fh.seek(max(0, self.spec_off - 1))
            fh.readline()
            for line in fh:
                p = line.decode("utf-8", "replace").strip().split(",")
                if len(p) < 128 or not p[-1].isdigit():
                    continue
                try:
                    rows.append((float(p[0]), [int(x) for x in p[2:128]]))
                except ValueError:
                    continue
        return rows


def alerts_in_state():
    nodes = read_state().get("nodes", {})
    return {k: nodes.get(k, {}).get("alert") for k in ("mestre", "analisador")}


def restore_end_state(b):
    b.send("placa_b", "jam off")
    b.send("placa_b", "power low")
    b.send("placa_a", "mode hopping")
    b.send("placa_b", "mode hopping")
    b.send("placa_a", "mitigation on")


# -------------------------------------------------------------------- t3
def phase_channel_table(recs, spec_rows, s, e):
    ack = Counter()
    miss = Counter()
    for r in recs:
        if s <= r.epoch < e and r.src == "mestre":
            try:
                ch = int(r.f.get("ch"))
            except (TypeError, ValueError):
                continue
            if r.event == "ack":
                ack[ch] += 1
            elif r.event == "miss":
                miss[ch] += 1
    rows = [row for ep, row in spec_rows if s <= ep < e]
    out = []
    for ch in HOP_CHANNELS:
        n = ack[ch] + miss[ch]
        out.append({
            "ch": ch, "acks": ack[ch], "misses": miss[ch],
            "miss_pct": r2(100.0 * miss[ch] / n) if n else None,
            "activity_mean": r2(mean([row[ch] for row in rows])) if rows else None,
        })
    corr = pearson([x["activity_mean"] for x in out], [x["miss_pct"] for x in out])
    return {"channels": out, "pearson_activity_vs_miss": r2(corr, 3), "spectrum_rows": len(rows)}


def mitigation_events(recs, s, e):
    out = []
    for r in recs:
        if s <= r.epoch < e and r.src in ("mestre", "escravo") and r.event in MITIG_EVENTS:
            keep = {k: v for k, v in r.f.items() if k in ("kind", "mask", "prev", "seed", "apply", "hop")}
            out.append({"rel_s": round(r.epoch - s, 2), "src": r.src, "event": r.event, **keep})
    return out


def cmd_t3_run(a):
    b = Bench()
    report = {"started": time.time(), "alert_state_initial_state_json": alerts_in_state()}
    print(f"[t3-run] estado de alerta inicial (state.json): {report['alert_state_initial_state_json']}")
    report["preflight_isolation"] = b.preflight()
    up = b.clean_repair()
    if up is None:
        print("[t3-run] re-pair falhou; segunda tentativa")
        up = b.clean_repair()
    report["repair_time_s"] = up
    if up is None:
        report["error"] = "enlace nao subiu"
        write_report("t3-run", report)
        return
    b.send("placa_a", "mode hopping")
    b.send("placa_b", "mode hopping")
    b.send("placa_a", "mitigation on")
    b.sleep(1.0)
    report["alert_state_after_repair_state_json"] = alerts_in_state()
    print(f"[t3-run] estado de alerta apos re-pair: {report['alert_state_after_repair_state_json']}")

    phases = {}
    t_off = time.time()
    b.sleep(60 - (time.time() - t_off))
    t_hold = time.time()
    b.send("placa_b", "mode hold 70")
    b.send("placa_b", "power max")
    b.send("placa_b", "jam on")
    b.sleep(60 - (time.time() - t_hold))
    t_rec = time.time()
    b.send("placa_b", "jam off")
    b.send("placa_b", "power low")
    b.sleep(60 - (time.time() - t_rec))
    t_end = time.time()
    # fase "off" comecou ANTES do comando jam off do clean_repair? nao: t_off apos o re-pair.
    bounds = {"off": (t_off, t_hold), "hold": (t_hold, t_rec), "recovery": (t_rec, t_end)}
    b.pump()
    spec = b.spectrum_rows()
    tl = alert_timeline(b.recs)
    # alertas pendentes antes do inicio: usa estado do state.json (a timeline so cobre eventos desta execucao)
    report["alert_pairs_run"] = tl
    for name, (s, e) in bounds.items():
        ph = phase_alert_chmap(b.recs, s, e, tl)
        ph["mitigation_events"] = mitigation_events(b.recs, s, e)
        ph["channel_table"] = phase_channel_table(b.recs, spec, s, e)
        ph["dur_s"] = e - s
        ph["start_epoch"], ph["end_epoch"] = s, e
        ph["n_loss_of_sync"] = sum(1 for r in b.recs if s <= r.epoch < e and r.event == "loss_of_sync")
        ph["throughput_fps_mean"] = r2(mean([float(r.f["frags_per_s"]) for r in b.recs
                                             if s <= r.epoch < e and r.src == "mestre" and r.event == "throughput"]))
        ph["plr_window_mean"] = r2(mean([float(r.f["window"]) for r in b.recs
                                         if s <= r.epoch < e and r.src == "escravo" and r.event == "plr"]))
        phases[name] = ph
        print(f"[t3-run] fase {name}: chmap n={ph['chmap_n']} maxrun>=1={ph['chmap_longest_run_ge1']} frac>=1={ph['chmap_frac_ge1']} "
              f"edges={ph['alert_edges_by_src']} loss={ph['n_loss_of_sync']} fps={ph['throughput_fps_mean']} plr={ph['plr_window_mean']}")
        print(f"    seq: {ph['chmap_seq']}")
        for ed in ph["alert_edges"]:
            print(f"    edge +{ed[0]}s {ed[1]} {ed[2]} count={ed[3]} mask={ed[4]}")
        for me in ph["mitigation_events"]:
            print(f"    mitig +{me['rel_s']}s {me['src']} {me['event']} { {k: v for k, v in me.items() if k not in ('rel_s', 'src', 'event')} }")
        print(f"    pearson(activity, miss%) = {ph['channel_table']['pearson_activity_vs_miss']}  (spectrum rows {ph['channel_table']['spectrum_rows']})")
    print("  ch   | off act/miss%   | hold act/miss%  | recov act/miss%   (acks+miss n)")
    for i, ch in enumerate(HOP_CHANNELS):
        cells = []
        for nm in ("off", "hold", "recovery"):
            c = phases[nm]["channel_table"]["channels"][i]
            cells.append(f"{c['activity_mean']!s:>6}/{c['miss_pct']!s:<6}({c['acks'] + c['misses']:>3})")
        print(f"  {ch:>4} | " + " | ".join(cells))
    report["phases"] = phases
    report["end_alert_state_json"] = alerts_in_state()
    restore_end_state(b)
    report["anomalies"] = b.anomalies()
    report["commands"] = b.cmd_log
    write_report("t3-run", report)
    print(f"[t3-run] anomalias: {report['anomalies']['crash_or_boot_lines']}  sem-ack: {[(c['board'], c['cmd']) for c in report['anomalies']['unacked_commands']]}")


# -------------------------------------------------------------------- t2
def cmd_t2_run(a):
    b = Bench()
    report = {"started": time.time()}
    report["preflight_isolation"] = b.preflight()
    up = b.clean_repair()
    if up is None:
        up = b.clean_repair()
    report["repair_time_s"] = up
    if up is None:
        report["error"] = "enlace nao subiu"
        write_report("t2-run", report)
        return
    b.send("placa_b", "jam off")
    b.send("placa_a", "mode hopping")
    b.send("placa_b", "mode hopping")
    b.send("placa_a", "mitigation off")
    ts = [r.epoch for r in b.recs if r.src == "mestre" and r.event == "time_sync"]
    lo = ts[-1] if ts else time.time()
    t_soak = time.time()
    dur = a.minutes * 60.0
    print(f"[t2-run] soak de {a.minutes} min, inicio {hms(t_soak)}")
    n = 0
    while time.time() - t_soak < dur:
        b.sleep(min(60.0, dur - (time.time() - t_soak)))
        n += 1
        last_md = max((r.epoch for r in b.recs if r.event == "msg_done"), default=0)
        print(f"[t2-run] {time.time() - t_soak:.0f}s; ultimo msg_done ha {time.time() - last_md:.1f}s; "
              f"loss={sum(1 for r in b.recs if r.event == 'loss_of_sync' and r.epoch >= t_soak)}")
    hi = time.time()
    res = analyze_t2(b.recs, lo, hi)
    print_t2(res)
    report["t2"] = res
    report["soak"] = {"lo": lo, "hi": hi, "soak_start": t_soak}
    restore_end_state(b)
    report["anomalies"] = b.anomalies()
    report["commands"] = b.cmd_log
    write_report("t2-run", report)
    print(f"[t2-run] anomalias: {report['anomalies']['crash_or_boot_lines']}  sem-ack: {[(c['board'], c['cmd']) for c in report['anomalies']['unacked_commands']]}")


# -------------------------------------------------------------------- t1
def cmd_t1_run(a):
    b = Bench()
    t_begin = time.time()
    report = {"started": t_begin, "max_cycles": a.cycles, "max_stuck": a.max_stuck}
    pre = b.preflight()
    report["preflight_isolation"] = pre
    b.send("placa_b", "jam off")
    b.send("placa_b", "power low")
    b.send("placa_a", "mode hopping")
    b.send("placa_b", "mode hopping")
    b.send("placa_a", "mitigation off")
    cycles = []
    stuck = []
    for n in range(1, a.cycles + 1):
        if time.time() - t_begin > a.max_minutes * 60:
            print("[t1] limite de tempo atingido")
            break
        typ = "R" if n % 2 == 1 else "L"
        cyc = {"n": n, "type": typ, "start": time.time(), "stuck_at": None}
        print(f"[t1] ---- ciclo {n} tipo {typ} ----")
        stuck_where = None
        if typ == "R":
            b.send("placa_b", "jam off")
            i0 = b.mark()
            t0 = time.time()
            b.send("placa_a", "reset")
            b.send("placa_b", "reset")
            up = b.wait_link(i0, t0, 20.0, True)
            cyc["t_to_up_s"] = None if up is None else round(up - t0, 2)
            if up is None:
                stuck_where = "R-repair"
        else:
            i0 = b.mark()
            t0 = time.time()
            b.send("placa_a", "mode fixed")
            b.send("placa_b", "mode fixed")
            up = b.wait_link(i0, t0, 20.0, False)
            cyc["fixed_up_s"] = None if up is None else round(up - t0, 2)
            if up is None:
                stuck_where = "L-fixed"
            else:
                b.send("placa_b", "mode hold 10")
                b.send("placa_b", "power max")
                i1 = b.mark()
                tj = time.time()
                b.send("placa_b", "jam on")
                lost = {}
                while time.time() - tj < 30.0 and len(lost) < 2:
                    b.pump()
                    for r in b.recs[i1:]:
                        if r.event == "loss_of_sync" and r.src in ("mestre", "escravo") and r.epoch >= tj and r.src not in lost:
                            lost[r.src] = round(r.epoch - tj, 2)
                    time.sleep(0.2)
                cyc["loss_after_jam_on_s"] = lost
                cyc["no_loss"] = len(lost) < 2
                i2 = b.mark()
                tj2 = time.time()
                b.send("placa_b", "jam off")
                b.send("placa_b", "power low")
                up2 = b.wait_link(i2, tj2, 30.0, len(lost) >= 1)
                cyc["recover_s"] = None if up2 is None else round(up2 - tj2, 2)
                if up2 is None:
                    stuck_where = "L-recover"
        if stuck_where:
            print(f"[t1] STUCK ciclo {n} ({stuck_where}) -> isolamento")
            iso = b.isolate(stuck_where)
            iso["cycle"] = n
            iso["type"] = typ
            stuck.append(iso)
            cyc["stuck_at"] = stuck_where
            cyc["resolved_by"] = iso["resolved_by"]
            cycles.append(cyc)
            if iso["resolved_by"] is None:
                print("[t1] isolamento nao recuperou o enlace; abortando")
                report["aborted"] = "isolamento falhou"
                break
        else:
            cycles.append(cyc)
        # restaura
        i3 = b.mark()
        t3 = time.time()
        b.send("placa_b", "jam off")
        b.send("placa_b", "power low")
        b.send("placa_a", "mode hopping")
        b.send("placa_b", "mode hopping")
        b.send("placa_a", "mitigation off")
        up3 = b.wait_link(i3, t3, 20.0, False)
        cyc["restore_ok"] = up3 is not None
        if up3 is None:
            print(f"[t1] STUCK apos restaurar modos (ciclo {n}) -> isolamento")
            iso = b.isolate("restore")
            iso["cycle"] = n
            iso["type"] = typ
            stuck.append(iso)
            if iso["resolved_by"] is None:
                report["aborted"] = "isolamento falhou (restore)"
                break
        if len(stuck) >= a.max_stuck:
            print("[t1] limite de STUCK atingido")
            break

    # fim: estado final
    i4 = b.mark()
    t4 = time.time()
    restore_end_state(b)
    up4 = b.wait_link(i4, t4, 20.0, False)
    report["final_link_up"] = up4 is not None
    if up4 is None:
        up4c = b.clean_repair()
        report["final_clean_repair_s"] = up4c
        restore_end_state(b)
    report["cycles"] = cycles
    report["stuck"] = stuck
    report["n_cycles_R"] = sum(1 for c in cycles if c["type"] == "R")
    report["n_cycles_L"] = sum(1 for c in cycles if c["type"] == "L")
    report["n_stuck_total"] = len(stuck)
    report["anomalies"] = b.anomalies()
    report["commands"] = b.cmd_log
    print(f"[t1] ciclos R={report['n_cycles_R']} L={report['n_cycles_L']} STUCK={len(stuck)} (preexistente: {'sim' if pre else 'nao'}) enlace final: {report['final_link_up']}")
    for c in cycles:
        print(f"   ciclo {c['n']} {c['type']}: "
              f"{ {k: v for k, v in c.items() if k not in ('n', 'type', 'start')} }")
    for s in stuck:
        print(f"   STUCK ciclo {s.get('cycle')} {s['where']}: resolvido por {s['resolved_by']}")
    write_report("t1-run", report)
    print(f"[t1] anomalias: {report['anomalies']['crash_or_boot_lines']}  sem-ack: {[(c['board'], c['cmd']) for c in report['anomalies']['unacked_commands']]}")


# --------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("t2-offline")
    p.add_argument("session")
    p.add_argument("--start", type=float, help="epoch inicial (default: 1o registro)")
    p.add_argument("--end", type=float, help="epoch final (default: 1o evento do jammer)")
    p.add_argument("--all", action="store_true", help="ate o fim do arquivo (ignora o jammer)")
    p.set_defaults(fn=cmd_t2_offline)
    p = sub.add_parser("t3-offline")
    p.add_argument("battery")
    p.add_argument("--session", help="override do session_file do JSON")
    p.set_defaults(fn=cmd_t3_offline)
    p = sub.add_parser("t3-run")
    p.set_defaults(fn=cmd_t3_run)
    p = sub.add_parser("t2-run")
    p.add_argument("--minutes", type=float, default=10.0)
    p.set_defaults(fn=cmd_t2_run)
    p = sub.add_parser("t1-run")
    p.add_argument("--cycles", type=int, default=20)
    p.add_argument("--max-stuck", type=int, default=3)
    p.add_argument("--max-minutes", type=float, default=28.0)
    p.set_defaults(fn=cmd_t1_run)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
