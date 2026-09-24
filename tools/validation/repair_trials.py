#!/usr/bin/env python3
"""
pairing-handshake-hardening (tarefa 1.7): número-base de tempo de
re-pareamento -- N ciclos de "enlace estável -> força perda -> mede tempo
até religar" em bancada, sem depender de intervenção manual. Ver
openspec/changes/pairing-handshake-hardening/design.md ("Goals": >= 19/20
tentativas em <= 10s é a meta que a etapa 3 (handshake v2) precisa bater;
este script mede o ANTES e, reexecutado depois de cada etapa, o efeito
dela).

Este script NÃO abre nenhuma porta serial -- só escreve comandos em
logs/cmd.in (mesmo mecanismo de run_scenario.py/run_battery.py) e lê
logs/latest.jsonl (symlink pro session-*.jsonl corrente, escrito por
tools/logger/main.py, que precisa estar rodando).

Cada trial:
  1. Espera o enlace ficar "estável": os dois lados pareados (pair_ok) e
     dado fluindo de verdade (mestre emitindo ack/msg_done, escravo tendo
     recebido seed) por pelo menos --stable-window segundos seguidos sem
     nenhuma nova perda (loss_of_sync/pair_timeout/boot) no meio.
  2. Força uma perda de enlace escrevendo "<placa> reset" em cmd.in,
     alternando entre placa_b e placa_a a cada trial -- mesmo comando
     "reset" que reset_to_pairing()/reset_to_scanning() já tratam
     (mestre/src/node.h, escravo/src/node.h).
  3. Mede o tempo até os dois lados pareados de novo (pair_ok) E o mestre
     confirmando dado fluindo (ack/msg_done) depois do pair_ok dele --
     timeout por trial de --trial-timeout segundos; se estourar, o trial
     conta como falha e o script segue pro próximo mesmo assim (o reset do
     próximo trial força uma nova tentativa de pareamento independente do
     estado atual, não importa se o anterior nunca se recuperou).

Uso:
    python3 tools/logger/main.py &         # se ainda não estiver rodando
    python3 tools/validation/repair_trials.py -n 20

Ao final, escreve logs/repair-<timestamp>.json (tempos por trial, sucesso,
e os eventos "regs"/"pair_stats" capturados durante cada trial) e imprime
um resumo (sucessos, mediana, p95).
"""
import argparse
import json
import math
import os
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(__file__))
from run_scenario import (  # noqa: E402
    LATEST_PATH,
    LOGS_DIR,
    current_session_path,
    require_logger_running,
    send_cmd,
)

# Alterna entre as duas placas a cada trial (tarefa 1.7 pede explicitamente
# "placa_b reset / placa_a reset, alternando") -- cada uma simula uma perda
# de enlace de um lado diferente (mestre vs. escravo+jammer).
RESET_BOARDS = ["placa_b", "placa_a"]

# Eventos que invalidam qualquer evidência de enlace anterior -- uma nova
# ocorrência de qualquer um destes reinicia o relógio de estabilidade.
DISRUPTION_EVENTS = {"loss_of_sync", "pair_timeout", "boot"}
# "dado fluindo" do lado do mestre, ver design.md/tasks.md 1.7.
MESTRE_DATA_EVENTS = {"ack", "msg_done"}


class JsonlTailer:
    """Segue logs/latest.jsonl a partir de ONDE ESTIVER no momento em que
    é criado (não relê histórico) -- e tolera rotação do SessionWriter
    (tools/logger/main.py troca de arquivo ao passar de MAX_SESSION_BYTES,
    o que reaponta o symlink)."""

    def __init__(self, latest_path):
        self.latest_path = latest_path
        self.cur_target = None
        self.fh = None

    def _ensure_open(self):
        target = os.path.realpath(self.latest_path)
        if target == self.cur_target:
            return
        first_open = self.cur_target is None
        if self.fh:
            self.fh.close()
        self.cur_target = target
        self.fh = open(target, "r", encoding="utf-8")
        if first_open:
            # Primeira abertura: não relê histórico, só o que vier daqui
            # pra frente.
            self.fh.seek(0, os.SEEK_END)
        # Reabertura por rotação (SessionWriter trocou de arquivo ao passar
        # de MAX_SESSION_BYTES): o novo arquivo é conteúdo NUNCA lido ainda
        # -- lê desde o início dele, não do fim, senão os primeiros eventos
        # do arquivo novo (escritos entre a rotação e este poll()) seriam
        # perdidos.

    def poll(self):
        """Retorna os registros novos (dict) desde a última chamada."""
        self._ensure_open()
        records = []
        while True:
            pos = self.fh.tell()
            line = self.fh.readline()
            if not line:
                break
            if not line.endswith("\n"):
                # Linha parcial -- o logger ainda não terminou de escrevê-la
                # (buffering=1, mas a escrita em si não é atômica). Recua e
                # tenta de novo na próxima chamada.
                self.fh.seek(pos)
                break
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        return records


class LinkTracker:
    """Estado derivado dos eventos JSONL vistos até agora -- o suficiente
    pra responder "o enlace está estável?" e "já recuperou depois de X?"
    sem precisar reler o arquivo inteiro a cada pergunta."""

    def __init__(self):
        self.last_disruption_epoch = 0.0
        self.mestre_pair_ok_epoch = None
        self.escravo_pair_ok_epoch = None
        self.mestre_data_epoch = None
        self.escravo_seed_epoch = None

    def update(self, rec):
        event = rec.get("event")
        src = rec.get("src")
        epoch = rec.get("epoch", time.time())

        if event in DISRUPTION_EVENTS:
            if epoch > self.last_disruption_epoch:
                self.last_disruption_epoch = epoch
            # Uma nova disrupção invalida qualquer evidência de enlace
            # anterior -- não vale "herdar" um pair_ok de antes da queda.
            self.mestre_pair_ok_epoch = None
            self.escravo_pair_ok_epoch = None
            self.mestre_data_epoch = None
            self.escravo_seed_epoch = None
        elif event == "pair_ok":
            if src == "mestre":
                self.mestre_pair_ok_epoch = epoch
            elif src == "escravo":
                self.escravo_pair_ok_epoch = epoch
        elif event in MESTRE_DATA_EVENTS and src == "mestre":
            self.mestre_data_epoch = epoch
        elif event == "seed" and src == "escravo":
            self.escravo_seed_epoch = epoch

    def is_stable(self, now, window_s):
        if self.mestre_pair_ok_epoch is None or self.escravo_pair_ok_epoch is None:
            return False
        if self.mestre_data_epoch is None or self.escravo_seed_epoch is None:
            return False
        return (now - self.last_disruption_epoch) >= window_s

    def recovered_at(self, since_epoch):
        """None se ainda não recuperou desde `since_epoch`; caso contrário,
        o epoch do evento que confirma dado fluindo de novo (critério de
        sucesso do trial: os dois lados pareados E o mestre confirmando
        ack/msg_done depois do próprio pair_ok dele)."""
        if self.mestre_pair_ok_epoch is None or self.mestre_pair_ok_epoch < since_epoch:
            return None
        if self.escravo_pair_ok_epoch is None or self.escravo_pair_ok_epoch < since_epoch:
            return None
        if self.mestre_data_epoch is None or self.mestre_data_epoch < self.mestre_pair_ok_epoch:
            return None
        return self.mestre_data_epoch


def poll_and_track(tailer, tracker, sink=None):
    for rec in tailer.poll():
        tracker.update(rec)
        if sink is not None and rec.get("event") in ("regs", "pair_stats"):
            sink.append({k: rec.get(k) for k in ("epoch", "src", "event", "fields")})


def wait_for_stability(tailer, tracker, window_s, timeout_s, poll_interval_s):
    deadline = time.time() + timeout_s
    while True:
        poll_and_track(tailer, tracker)
        now = time.time()
        if tracker.is_stable(now, window_s):
            return True
        if now >= deadline:
            return False
        time.sleep(poll_interval_s)


def wait_for_recovery(tailer, tracker, since_epoch, timeout_s, poll_interval_s, sink):
    deadline = since_epoch + timeout_s
    while True:
        poll_and_track(tailer, tracker, sink)
        recovered_epoch = tracker.recovered_at(since_epoch)
        if recovered_epoch is not None:
            return recovered_epoch
        if time.time() >= deadline:
            return None
        time.sleep(poll_interval_s)


def percentile(sorted_values, pct):
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    k = (len(sorted_values) - 1) * (pct / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_values[int(k)]
    return sorted_values[int(f)] * (c - k) + sorted_values[int(c)] * (k - f)


def median(sorted_values):
    if not sorted_values:
        return None
    n = len(sorted_values)
    mid = n // 2
    if n % 2 == 1:
        return sorted_values[mid]
    return (sorted_values[mid - 1] + sorted_values[mid]) / 2.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--trials", type=int, default=20, help="número de trials (default: 20)")
    ap.add_argument(
        "--stable-window", type=float, default=5.0,
        help="segundos de enlace contínuo (pareado + dado fluindo) exigidos antes de forçar a próxima perda (default: 5)",
    )
    ap.add_argument(
        "--trial-timeout", type=float, default=60.0,
        help="timeout por trial esperando re-pareamento + dado fluindo, em segundos (default: 60)",
    )
    ap.add_argument(
        "--initial-stable-timeout", type=float, default=60.0,
        help="quanto esperar pela estabilidade inicial antes do 1º trial "
        "antes de desistir e forçar mesmo assim (default: 60)",
    )
    ap.add_argument("--poll-interval", type=float, default=0.2, help="intervalo de poll do JSONL, em segundos (default: 0.2)")
    ap.add_argument(
        "--start-board", choices=RESET_BOARDS, default="placa_b",
        help="placa resetada no 1º trial -- alterna a partir daí (default: placa_b)",
    )
    args = ap.parse_args()

    require_logger_running()
    print(f"[repair] Sessão ativa: {current_session_path()}")

    # tarefa 1.7 / 0.1: garante jammer desligado ANTES de qualquer coisa --
    # um "jam on"+"mode adaptive" armado de um teste anterior não deve
    # contaminar a medição de tempo de re-pareamento.
    print("[repair] Garantindo jammer desligado (placa_b jam off)...")
    send_cmd("placa_b", "jam off")
    time.sleep(1)

    tailer = JsonlTailer(LATEST_PATH)
    tracker = LinkTracker()

    board_order = RESET_BOARDS if args.start_board == RESET_BOARDS[0] else list(reversed(RESET_BOARDS))

    print(f"[repair] Aguardando enlace estável (>= {args.stable_window:.0f}s) antes do 1º trial...")
    if not wait_for_stability(tailer, tracker, args.stable_window, args.initial_stable_timeout, args.poll_interval):
        print(
            f"[repair] AVISO: enlace não ficou estável em {args.initial_stable_timeout:.0f}s -- "
            "prosseguindo mesmo assim (o próprio 'reset' do trial 1 força uma tentativa de pareamento "
            "independente do estado atual)."
        )

    t0 = time.time()
    trials = []

    for i in range(1, args.trials + 1):
        board = board_order[(i - 1) % len(board_order)]
        print(f"[repair] Trial {i}/{args.trials}: forçando perda via '{board} reset'...")
        send_cmd(board, "reset")
        loss_epoch = time.time()

        trial_events = []
        recovered_epoch = wait_for_recovery(
            tailer, tracker, loss_epoch, args.trial_timeout, args.poll_interval, trial_events
        )
        success = recovered_epoch is not None
        recovery_s = (recovered_epoch - loss_epoch) if success else None

        if success:
            print(f"[repair] Trial {i}/{args.trials}: recuperado em {recovery_s:.2f}s")
        else:
            print(
                f"[repair] Trial {i}/{args.trials}: FALHOU -- enlace não voltou em "
                f"{args.trial_timeout:.0f}s; seguindo pro próximo trial mesmo assim."
            )

        trials.append(
            {
                "trial": i,
                "reset_board": board,
                "loss_epoch": loss_epoch,
                "recovered_epoch": recovered_epoch,
                "recovery_s": recovery_s,
                "success": success,
                "events": trial_events,
            }
        )

        if success and i < args.trials:
            # Best-effort: confirma estabilidade antes do próximo trial,
            # mas sem travar o script inteiro se ela demorar mais que o
            # normal -- o próximo 'reset' funciona independente disso.
            wait_for_stability(tailer, tracker, args.stable_window, args.stable_window + 10.0, args.poll_interval)

    end_epoch = time.time()

    successes = [t for t in trials if t["success"]]
    recovery_times = sorted(t["recovery_s"] for t in successes)
    summary = {
        "trials_total": len(trials),
        "successes": len(successes),
        "success_rate": (len(successes) / len(trials)) if trials else 0.0,
        "median_s": median(recovery_times),
        "p95_s": percentile(recovery_times, 95),
    }

    result = {
        "started_at": datetime.fromtimestamp(t0, tz=timezone.utc).isoformat(),
        "ended_at": datetime.fromtimestamp(end_epoch, tz=timezone.utc).isoformat(),
        "session_file": current_session_path(),
        "trials_requested": args.trials,
        "stable_window_s": args.stable_window,
        "trial_timeout_s": args.trial_timeout,
        "trials": trials,
        "summary": summary,
    }

    ts = datetime.fromtimestamp(t0, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(LOGS_DIR, f"repair-{ts}.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)

    print(f"[repair] Resultados salvos em {out_path}")
    print(
        f"[repair] Resumo: {summary['successes']}/{summary['trials_total']} sucessos "
        f"({100.0 * summary['success_rate']:.0f}%) -- "
        f"mediana={summary['median_s']:.2f}s p95={summary['p95_s']:.2f}s"
        if successes
        else f"[repair] Resumo: {summary['successes']}/{summary['trials_total']} sucessos -- nenhum trial recuperou a tempo."
    )


if __name__ == "__main__":
    main()
