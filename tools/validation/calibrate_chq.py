#!/usr/bin/env python3
"""
calibrate_chq -- calibracao OFFLINE de T_on / T_off / W do change
channel-quality-sensing (task 1.1/1.3). Nao toca em bancada nem firmware.

Cruza, por canal da tabela de salto e por janela de tempo:
  * ocupacao do analisador (logs/spectrum.csv; cada celula = % de
    SCAN_SAMPLES=10 amostras de testCarrier() que acenderam naquela varredura)
  * miss% do mestre = miss / (ack + miss) (eventos ack/miss, fields.ch, da sessao)

Uso:
  calibrate_chq.py window --t0 EPOCH --t1 EPOCH [--session F]   # uma janela
  calibrate_chq.py all [--out FILE.json]                        # conjunto embutido

Ocupacao reportada de duas formas (ver design.md D1):
  occ_mean  = media do valor da celula (0..100)     -> "% de amostras acesas"
  occ_sweep = % de varreduras com celula > 0        -> "fracao das varreduras com atividade"
"""
import argparse
import bisect
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "..", "logs")
SPECTRUM = os.path.join(LOGS, "spectrum.csv")
HOP = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]
WIFI = [27, 34, 46]


# ---------------------------------------------------------------- spectrum
class Spectrum:
    """Acesso por epoch ao spectrum.csv (monotonico) via busca binaria em bytes."""

    def __init__(self, path=SPECTRUM):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def _epoch_at(self, pos):
        """Epoch da primeira linha completa a partir de pos -> (epoch, offset)."""
        if pos > 0:
            self.f.seek(pos - 1)
            self.f.readline()  # descarta resto de linha parcial
        else:
            self.f.seek(0)
        off = self.f.tell()
        line = self.f.readline()
        if not line:
            return None, off
        try:
            return float(line.split(b",", 1)[0]), off
        except ValueError:
            return None, off

    def seek_epoch(self, t):
        lo, hi = 0, self.size
        while hi - lo > 4096:
            mid = (lo + hi) // 2
            ep, _ = self._epoch_at(mid)
            if ep is None or ep >= t:
                hi = mid
            else:
                lo = mid
        _, off = self._epoch_at(lo)
        return off

    def rows(self, t0, t1):
        """Lista de (epoch, [126 ints]) com t0 <= epoch < t1."""
        off = self.seek_epoch(t0)
        self.f.seek(off)
        out = []
        for line in self.f:
            p = line.rstrip(b"\n").split(b",")
            if len(p) != 128:
                continue
            try:
                ep = float(p[0])
            except ValueError:
                continue
            if ep < t0:
                continue
            if ep >= t1:
                break
            out.append((ep, [int(x) for x in p[2:]]))
        return out


# ---------------------------------------------------------------- sessions
_sess_index = None


def session_index():
    """[(first_epoch, last_epoch, path)] de todas as sessoes."""
    global _sess_index
    if _sess_index is not None:
        return _sess_index
    idx = []
    for p in sorted(glob.glob(os.path.join(LOGS, "session-*.jsonl"))):
        try:
            with open(p, "rb") as fh:
                first = fh.readline()
                fh.seek(0, 2)
                size = fh.tell()
                fh.seek(max(0, size - 8192))
                tail = fh.read().splitlines()
            e0 = json.loads(first)["epoch"]
            e1 = None
            for ln in reversed(tail):
                try:
                    e1 = json.loads(ln)["epoch"]
                    break
                except (json.JSONDecodeError, KeyError):
                    continue
            if e1 is not None:
                idx.append((e0, e1, p))
        except (OSError, json.JSONDecodeError, KeyError):
            continue
    _sess_index = idx
    return idx


def find_session(t0, t1):
    for e0, e1, p in session_index():
        if e0 <= t0 and t1 <= e1 + 1:
            return p
    return None


def ack_miss(path, t0, t1):
    """Contagens mestre ack/miss por canal RF em [t0, t1)."""
    ack, miss = {}, {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if '"src": "mestre"' not in line:
                continue
            if '"event": "ack"' in line:
                kind = ack
            elif '"event": "miss"' in line:
                kind = miss
            else:
                continue
            try:
                d = json.loads(line)
                ep = d["epoch"]
                if ep < t0 or ep >= t1:
                    continue
                ch = int(d["fields"]["ch"])
            except (json.JSONDecodeError, KeyError, ValueError, TypeError):
                continue
            kind[ch] = kind.get(ch, 0) + 1
    return ack, miss


# ---------------------------------------------------------------- core
def channel_table(session, spec, t0, t1, rows=None):
    """Por canal da tabela: ocupacao (spectrum) e miss% (sessao) em [t0, t1)."""
    rows = spec.rows(t0, t1) if rows is None else rows
    ack, miss = ack_miss(session, t0, t1)
    out = {}
    for ch in HOP:
        vals = [r[ch] for _, r in rows]
        n = ack.get(ch, 0) + miss.get(ch, 0)
        out[ch] = {
            "occ_mean": sum(vals) / len(vals) if vals else None,
            "occ_sweep": 100.0 * sum(1 for v in vals if v > 0) / len(vals) if vals else None,
            "occ_max": max(vals) if vals else None,
            "n_sweeps": len(vals),
            "ack": ack.get(ch, 0), "miss": miss.get(ch, 0),
            "miss_pct": 100.0 * miss.get(ch, 0) / n if n else None,
            "n_tx": n,
        }
    span = (rows[-1][0] - rows[0][0]) if len(rows) > 1 else 0
    return out, {"n_sweeps": len(rows), "sweep_period_s": span / (len(rows) - 1) if len(rows) > 1 else None}


def sweep_period(spec, t0, t1):
    rows = spec.rows(t0, t1)
    if len(rows) < 2:
        return None
    d = sorted(b[0] - a[0] for a, b in zip(rows, rows[1:]))
    return {"n": len(rows), "median_s": d[len(d) // 2], "p10_s": d[len(d) // 10], "p90_s": d[9 * len(d) // 10]}


# ---------------------------------------------------------------- windows
def load(path):
    with open(path) as fh:
        return json.load(fh)


def battery_windows(trim=3.0):
    """Janelas de bateria hold: (label, t0, t1, jam_ch|None, kind)."""
    w = []
    for f in sorted(glob.glob(os.path.join(LOGS, "battery-*.json"))):
        d = load(f)
        if d.get("jammer_mode") != "hold":
            continue
        tag = os.path.basename(f)[8:-5]
        ph = d["phases"]
        ends = [p["start_epoch"] for p in ph[1:]] + [d["phases"][-1]["start_epoch"] + 5]
        for p, e in zip(ph, ends):
            if p["name"] == "end":
                continue
            kind = {"baseline": "nojam", "jamming": "jam", "recovery": "recov"}[p["name"]]
            s = p["start_epoch"] + (trim if kind != "nojam" else 0)
            if kind == "recov":
                s = p["start_epoch"] + 5.0  # deixa o carrier cair
            w.append((f"bat{tag}-{p['scenario']}-{p['name']}", s, e, 10 if kind == "jam" else None, kind))
    return w


def jam_mode_in(session, t0, t1):
    """(modo, canal|None) do ultimo evento jam_mode_* do jammer em [t0, t1)."""
    mode = None
    with open(session, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if '"src": "jammer"' not in line or "jam_mode_" not in line:
                continue
            d = json.loads(line)
            if t0 <= d["epoch"] < t1:
                mode = (d["event"][len("jam_mode_"):], (d.get("fields") or {}).get("ch"))
    return mode


def scenario_windows(trim=3.0, pattern="scenario-20261021-*.json"):
    """Janelas dos scenario-*.json (1 cenario por arquivo; modo lido do jammer)."""
    w = []
    for f in sorted(glob.glob(os.path.join(LOGS, pattern))):
        d = load(f)
        ph = d["phases"]
        tag = os.path.basename(f)[9:-5]
        sess = d.get("session_file")
        names = [p["name"] for p in ph]
        starts = {p["name"]: p["start_epoch"] for p in ph}
        mode = jam_mode_in(sess, starts["jamming"] - 5, starts["recovery"]) if sess and os.path.exists(sess) else None
        mlabel = "?" if not mode else mode[0] + (f"{mode[1]}" if mode[1] else "")
        jch = int(mode[1]) if mode and mode[1] else None
        w.append((f"scn{tag}-{mlabel}-baseline", starts["baseline"], starts["jamming"], None, "nojam"))
        w.append((f"scn{tag}-{mlabel}-jamming", starts["jamming"] + trim, starts["recovery"], jch, "jam-" + (mode[0] if mode else "?")))
        w.append((f"scn{tag}-{mlabel}-recovery", starts["recovery"] + 5.0, starts["end"], None, "recov"))
    return w


def diag_windows():
    w = []
    d = load(os.path.join(LOGS, "diag-t3-run-20261021-123916.json"))
    ph = d["phases"]
    w.append(("t3run-off", ph["off"]["start_epoch"], ph["off"]["end_epoch"], None, "nojam"))
    w.append(("t3run-hold70", ph["hold"]["start_epoch"] + 3, ph["hold"]["end_epoch"], 70, "jam"))
    w.append(("t3run-recov", ph["recovery"]["start_epoch"] + 5, ph["recovery"]["end_epoch"], None, "recov"))
    d = load(os.path.join(LOGS, "diag-t2-run-20261021-124950.json"))
    w.append(("t2run-soak10min", d["soak"]["lo"], d["soak"]["hi"], None, "nojam"))
    return w


def all_windows():
    return battery_windows() + diag_windows() + scenario_windows()


def print_table(label, t0, t1, tab, meta, jam_ch=None):
    print(f"## {label}  [{t0:.0f}..{t1:.0f}] {t1 - t0:.0f}s  sweeps={meta['n_sweeps']}  jam_ch={jam_ch}")
    print("  ch  occ_mean occ_sweep occ_max |  ack  miss  miss%")
    for ch in HOP:
        r = tab[ch]
        if r["occ_mean"] is None:
            continue
        mp = "  n/a" if r["miss_pct"] is None else f"{r['miss_pct']:5.1f}"
        print(f"  {ch:3d} {r['occ_mean']:8.1f} {r['occ_sweep']:9.1f} {r['occ_max']:7d} | {r['ack']:4d} {r['miss']:5d} {mp}")


# ---------------------------------------------------------------- soak (janelas deslizantes de W varreduras)
def ack_miss_events(path, t0, t1):
    """Lista ordenada de (epoch, ch, is_miss) do mestre em [t0, t1)."""
    ev = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if '"src": "mestre"' not in line:
                continue
            if '"event": "ack"' in line:
                m = 0
            elif '"event": "miss"' in line:
                m = 1
            else:
                continue
            try:
                d = json.loads(line)
                ep = d["epoch"]
                if ep < t0 or ep >= t1:
                    continue
                ev.append((ep, int(d["fields"]["ch"]), m))
            except (json.JSONDecodeError, KeyError, ValueError, TypeError):
                continue
    ev.sort()
    return ev


def soak_points(session, spec, t0, t1, W, min_tx=20):
    """Janelas NAO sobrepostas de W varreduras: pontos (ocupacao, miss%) por canal da tabela.

    Retorna lista de dicts {t, ch, occ_mean, occ_sweep, ack, miss}. Janelas sem
    min_tx tentativas no canal sao descartadas (canal fora da mascara, p.ex.).
    """
    rows = spec.rows(t0, t1)
    ev = ack_miss_events(session, t0, t1)
    ev_t = [e[0] for e in ev]
    pts = []
    for i in range(0, len(rows) - W + 1, W):
        blk = rows[i:i + W]
        a, b = blk[0][0], blk[-1][0] + 0.45  # varredura dura ~0.9 s
        lo, hi = bisect.bisect_left(ev_t, a), bisect.bisect_left(ev_t, b)
        ack, miss = {}, {}
        for _, ch, m in ev[lo:hi]:
            (miss if m else ack)[ch] = (miss if m else ack).get(ch, 0) + 1
        for ch in HOP:
            n = ack.get(ch, 0) + miss.get(ch, 0)
            if n < min_tx:
                continue
            vals = [r[1][ch] for r in blk]
            pts.append({"t": a, "ch": ch, "occ_mean": sum(vals) / W,
                        "occ_sweep": 100.0 * sum(1 for v in vals if v > 0) / W,
                        "ack": ack.get(ch, 0), "miss": miss.get(ch, 0)})
    return pts


BINS = [0, 1, 2, 5, 10, 15, 20, 30, 50, 101]


def bin_summary(pts, key="occ_mean", bins=BINS):
    out = []
    for lo, hi in zip(bins, bins[1:]):
        sel = [p for p in pts if lo <= p[key] < hi]
        if not sel:
            out.append((lo, hi, 0, None, None, None, None))
            continue
        tx = sum(p["ack"] + p["miss"] for p in sel)
        pooled = 100.0 * sum(p["miss"] for p in sel) / tx
        mp = sorted(100.0 * p["miss"] / (p["ack"] + p["miss"]) for p in sel)
        out.append((lo, hi, len(sel), pooled, mp[len(mp) // 2], mp[len(mp) // 10], mp[(9 * len(mp)) // 10]))
    return out


def print_bins(title, pts, key="occ_mean"):
    print(f"-- {title}  ({len(pts)} pontos)  bins por {key}")
    print("   bin        n   pooled_miss%  med   p10   p90")
    for lo, hi, n, pooled, med, p10, p90 in bin_summary(pts, key):
        if n == 0:
            print(f"  [{lo:3d},{hi:3d}) {0:5d}")
        else:
            print(f"  [{lo:3d},{hi:3d}) {n:5d} {pooled:12.1f} {med:6.1f} {p10:5.1f} {p90:5.1f}")


def simulate(rows, ev, W, t_on, t_off, chans=HOP):
    """Maquina de estados por canal sobre varreduras (janela deslizante de W).

    rows: [(epoch, [126])]; ev: [(epoch, ch, is_miss)] ordenado.
    Retorna por canal: fracao do tempo marcado, transicoes, (ack, miss) com o
    canal marcado / nao marcado no instante do evento, e latencia do 1o flag.
    """
    import collections
    n = len(rows)
    ts = [r[0] for r in rows]
    out = {}
    for ch in chans:
        v = [r[1][ch] for r in rows]
        state = [False] * n
        acc = 0.0
        flagged = False
        trans = 0
        for i in range(n):
            acc += v[i]
            if i >= W:
                acc -= v[i - W]
            m = acc / min(i + 1, W)
            if not flagged and i + 1 >= W and m >= t_on:
                flagged = True
                trans += 1
            elif flagged and m <= t_off:
                flagged = False
                trans += 1
            state[i] = flagged
        out[ch] = {"state": state, "trans": trans, "frac_flag": sum(state) / n if n else 0}
    cnt = {ch: [0, 0, 0, 0] for ch in chans}  # ack_flag, miss_flag, ack_unfl, miss_unfl
    for ep, ch, m in ev:
        if ch not in cnt:
            continue
        i = bisect.bisect_right(ts, ep) - 1
        if i < 0 or i >= n or ep - ts[i] > 2.0:
            continue
        k = (0 if out[ch]["state"][i] else 2) + (1 if m else 0)
        cnt[ch][k] += 1
    for ch in chans:
        out[ch]["cnt"] = cnt[ch]
        out[ch]["hours"] = (ts[-1] - ts[0]) / 3600.0 if n > 1 else 0
    return out


def cmd_soak(args, spec):
    sess = args.session
    base = os.path.basename(sess)
    e0, e1 = next((a, b) for a, b, p in session_index() if p == os.path.abspath(sess) or os.path.basename(p) == base)
    pts = soak_points(sess, spec, e0, e1, args.W)
    print_bins(f"{base} W={args.W}", pts, "occ_mean")
    if args.out:
        with open(args.out, "w") as fh:
            json.dump(pts, fh)


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    w = sp.add_parser("window")
    w.add_argument("--t0", type=float, required=True)
    w.add_argument("--t1", type=float, required=True)
    w.add_argument("--session")
    sk = sp.add_parser("soak")
    sk.add_argument("session")
    sk.add_argument("--W", type=int, default=10)
    sk.add_argument("--out")
    a = sp.add_parser("all")
    a.add_argument("--out")
    a.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    spec = Spectrum()
    if args.cmd == "soak":
        return cmd_soak(args, spec)
    if args.cmd == "window":
        sess = args.session or find_session(args.t0, args.t1)
        if not sess:
            sys.exit("sessao nao encontrada para a janela")
        tab, meta = channel_table(sess, spec, args.t0, args.t1)
        print_table("window", args.t0, args.t1, tab, meta)
        return

    res = []
    for label, t0, t1, jch, kind in all_windows():
        sess = find_session(t0, t1)
        if not sess:
            print(f"# {label}: sessao nao encontrada", file=sys.stderr)
            continue
        tab, meta = channel_table(sess, spec, t0, t1)
        if not args.quiet:
            print_table(label, t0, t1, tab, meta, jch)
        res.append({"label": label, "t0": t0, "t1": t1, "jam_ch": jch, "kind": kind,
                    "session": os.path.basename(sess), "meta": meta, "table": {str(k): v for k, v in tab.items()}})
    if args.out:
        with open(args.out, "w") as fh:
            json.dump(res, fh)


if __name__ == "__main__":
    main()
