#!/usr/bin/env python3
"""
telemetry-logging: daemon headless, dono exclusivo das duas portas seriais
da bancada. Persiste tudo em disco (logs/session-*.jsonl), deriva um
snapshot de estado (logs/state.json) e uma visão humana (logs/events.log),
e aceita comandos de volta para as placas via logs/cmd.in.

Uso:
    python3 tools/logger/main.py
    python3 tools/logger/main.py --placa-a /dev/ttyUSB0 --placa-b /dev/ttyACM0

Sem argumentos, tenta os caminhos estáveis de /dev/serial/by-id/ conhecidos
desta bancada (ver PLACA_A_ID/PLACA_B_ID abaixo) -- ajuste se o hardware
mudar.

Enviar comando às placas: escrever uma linha em logs/cmd.in no formato
    <placa_a|placa_b> <comando>
ex.: echo "placa_b jam sweep" >> logs/cmd.in
"""
import argparse
import glob
import json
import os
import re
import signal
import sys
import threading
import time
from datetime import datetime, timezone

import serial

BAUD_RATE = 115200

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGS_DIR = os.path.join(REPO_ROOT, "logs")

# Caminhos estáveis desta bancada (/dev/serial/by-id/ sobrevive a
# replug/reboot, diferente de /dev/ttyUSB0 que pode trocar de número).
PLACA_A_ID = "usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"
PLACA_B_ID = "usb-1a86_USB_Single_Serial_5483018532-if00"
BY_ID_DIR = "/dev/serial/by-id"

MAX_SESSION_BYTES = 100 * 1024 * 1024  # rotação simples por tamanho

EVENT_RE = re.compile(r"^@(?P<src>[^;]+);t=(?P<t>\d+);e=(?P<event>[^;]*)(?P<rest>;.*)?$")

# Eventos de rotina, omitidos de events.log (ainda vão para o JSONL bruto).
ROUTINE_EVENTS = {"hop", "ack", "miss", "pair_scan"}

CSV_MIN_FIELDS = 100


def resolve_default_port(by_id_name, fallback_glob):
    path = os.path.join(BY_ID_DIR, by_id_name)
    if os.path.exists(path):
        return path
    candidates = sorted(glob.glob(fallback_glob))
    return candidates[0] if candidates else None


def parse_fields(rest):
    fields = {}
    if not rest:
        return fields
    for part in rest.strip(";").split(";"):
        if not part:
            continue
        if "=" in part:
            k, v = part.split("=", 1)
            fields[k] = v
        else:
            fields[part] = True
    return fields


def looks_like_csv(line):
    if not line or not (line[0].isdigit() or line[0] == "-"):
        return False
    parts = line.split(",")
    if len(parts) < CSV_MIN_FIELDS:
        return False
    for p in parts[:5]:
        p = p.strip()
        if not p.lstrip("-").isdigit():
            return False
    return True


class StateStore:
    def __init__(self):
        self.lock = threading.Lock()
        self.state = {"updated_at": None, "nodes": {}}
        self.dirty = False

    def apply_event(self, board, src, event, fields, t_ms, epoch):
        with self.lock:
            node = self.state["nodes"].setdefault(src, {})
            node["board"] = board
            node["last_event"] = event
            node["last_t_ms"] = t_ms
            node["last_seen_epoch"] = epoch

            if event == "boot":
                node["boot_epoch"] = epoch
            elif event == "pair_ok":
                node["link"] = True
                node["pair_channel"] = fields.get("ch")
            elif event == "loss_of_sync":
                node["link"] = False
            elif event == "seed":
                node["seed"] = fields.get("seed")
            elif event == "hop":
                node["hop_counter"] = fields.get("n")
                node["channel"] = fields.get("ch")
            elif event == "plr":
                node["plr_window"] = fields.get("window")
                node["plr_session"] = fields.get("session")
            elif event in ("alert_on", "alert_off"):
                node["alert"] = event == "alert_on"
            elif event == "blacklist":
                node["channel_mask"] = fields.get("mask")
            elif event in ("jam_mode_sweep", "jam_mode_targeted"):
                node["jam_mode"] = "targeted" if event == "jam_mode_targeted" else "sweep"
            elif event in ("jam_state_on", "jam_state_off"):
                node["jam_enabled"] = event == "jam_state_on"
            elif event in ("rekey_tx", "rekey_ack", "rekey_apply", "rekey_failed"):
                node.setdefault("last_rekey", {})
                node["last_rekey"] = {"phase": event, **fields}

            self.dirty = True

    def snapshot(self):
        with self.lock:
            self.state["updated_at"] = datetime.now(timezone.utc).isoformat()
            return json.dumps(self.state, indent=2, ensure_ascii=False)

    def is_dirty(self):
        with self.lock:
            return self.dirty

    def clear_dirty(self):
        with self.lock:
            self.dirty = False


class SessionWriter:
    """Único escritor de logs/session-*.jsonl, logs/events.log e
    logs/spectrum.csv -- serializado por lock para evitar linhas
    intercaladas quando os dois leitores de porta escrevem ao mesmo tempo."""

    def __init__(self, logs_dir):
        self.logs_dir = logs_dir
        self.lock = threading.Lock()
        os.makedirs(logs_dir, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.session_path = os.path.join(logs_dir, f"session-{ts}.jsonl")
        self.session_file = open(self.session_path, "a", buffering=1, encoding="utf-8")
        self.events_path = os.path.join(logs_dir, "events.log")
        self.events_file = open(self.events_path, "a", buffering=1, encoding="utf-8")
        self.csv_path = os.path.join(logs_dir, "spectrum.csv")
        self._relink_latest()

    def _relink_latest(self):
        latest = os.path.join(self.logs_dir, "latest.jsonl")
        try:
            if os.path.islink(latest) or os.path.exists(latest):
                os.remove(latest)
        except OSError:
            pass
        os.symlink(os.path.basename(self.session_path), latest)

    def _maybe_rotate(self):
        try:
            if os.path.getsize(self.session_path) < MAX_SESSION_BYTES:
                return
        except OSError:
            return
        self.session_file.close()
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.session_path = os.path.join(self.logs_dir, f"session-{ts}.jsonl")
        self.session_file = open(self.session_path, "a", buffering=1, encoding="utf-8")
        self._relink_latest()

    def write_raw(self, board, line, parsed):
        record = {
            "epoch": time.time(),
            "board": board,
            "raw": line,
        }
        if parsed:
            record["src"] = parsed[0]
            record["t_ms"] = parsed[1]
            record["event"] = parsed[2]
            record["fields"] = parsed[3]
        else:
            record["event"] = "raw"

        with self.lock:
            self.session_file.write(json.dumps(record, ensure_ascii=False) + "\n")
            self._maybe_rotate()

            if parsed and parsed[2] not in ROUTINE_EVENTS:
                ts_human = datetime.now().strftime("%H:%M:%S")
                fields_str = " ".join(f"{k}={v}" for k, v in parsed[3].items())
                self.events_file.write(f"[{ts_human}] {board}/{parsed[0]} {parsed[2]} {fields_str}\n")

    def write_csv_row(self, board, line):
        with self.lock:
            with open(self.csv_path, "a", encoding="utf-8") as f:
                f.write(f"{time.time()},{board},{line}\n")

    def close(self):
        with self.lock:
            self.session_file.close()
            self.events_file.close()


class SerialBoard(threading.Thread):
    def __init__(self, board_name, port_path, session_writer, state_store, stop_event):
        super().__init__(daemon=True, name=f"reader-{board_name}")
        self.board_name = board_name
        self.port_path = port_path
        self.session_writer = session_writer
        self.state_store = state_store
        self.stop_event = stop_event
        self.serial_obj = None
        self.write_lock = threading.Lock()

    def _open(self):
        while not self.stop_event.is_set():
            try:
                ser = serial.Serial()
                ser.port = self.port_path
                ser.baudrate = BAUD_RATE
                ser.dtr = False
                ser.rts = False
                ser.timeout = 1
                ser.open()
                print(f"[logger] {self.board_name}: conectado em {self.port_path}", file=sys.stderr)
                return ser
            except (serial.SerialException, OSError) as exc:
                print(f"[logger] {self.board_name}: falha ao abrir {self.port_path} ({exc}); tentando de novo em 2s", file=sys.stderr)
                time.sleep(2)
        return None

    def send_line(self, text):
        with self.write_lock:
            if self.serial_obj and self.serial_obj.is_open:
                try:
                    self.serial_obj.write((text.strip() + "\n").encode("utf-8"))
                    return True
                except (serial.SerialException, OSError):
                    return False
        return False

    def run(self):
        while not self.stop_event.is_set():
            self.serial_obj = self._open()
            if self.serial_obj is None:
                return
            try:
                while not self.stop_event.is_set():
                    raw = self.serial_obj.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    if not line:
                        continue
                    self._handle_line(line)
            except (serial.SerialException, OSError) as exc:
                print(f"[logger] {self.board_name}: porta caiu ({exc}); reconectando...", file=sys.stderr)
                try:
                    self.serial_obj.close()
                except Exception:
                    pass
                time.sleep(2)

    def _handle_line(self, line):
        if looks_like_csv(line):
            self.session_writer.write_csv_row(self.board_name, line)
            self.session_writer.write_raw(self.board_name, line, None)
            return

        m = EVENT_RE.match(line)
        parsed = None
        if m:
            src = m.group("src")
            t_ms = int(m.group("t"))
            event = m.group("event")
            fields = parse_fields(m.group("rest"))
            parsed = (src, t_ms, event, fields)
            self.state_store.apply_event(self.board_name, src, event, fields, t_ms, time.time())

        self.session_writer.write_raw(self.board_name, line, parsed)


class CommandWatcher(threading.Thread):
    """Lê logs/cmd.in a partir do fim do arquivo no momento em que o
    daemon inicia -- comandos de sessões anteriores não são reenviados."""

    def __init__(self, cmd_path, boards, stop_event):
        super().__init__(daemon=True, name="cmd-watcher")
        self.cmd_path = cmd_path
        self.boards = boards
        self.stop_event = stop_event

    def run(self):
        if not os.path.exists(self.cmd_path):
            open(self.cmd_path, "a", encoding="utf-8").close()
        with open(self.cmd_path, "r", encoding="utf-8") as f:
            f.seek(0, os.SEEK_END)
            while not self.stop_event.is_set():
                line = f.readline()
                if not line:
                    time.sleep(0.2)
                    continue
                line = line.strip()
                if not line:
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    print(f"[logger] cmd.in: linha ignorada (formato esperado '<placa> <comando>'): {line}", file=sys.stderr)
                    continue
                board_name, cmd = parts
                board = self.boards.get(board_name)
                if not board:
                    print(f"[logger] cmd.in: placa desconhecida '{board_name}'", file=sys.stderr)
                    continue
                ok = board.send_line(cmd)
                print(f"[logger] cmd.in -> {board_name}: {cmd!r} ({'ok' if ok else 'falhou, porta fechada'})", file=sys.stderr)


class StateWriter(threading.Thread):
    def __init__(self, state_path, state_store, stop_event, interval=1.0):
        super().__init__(daemon=True, name="state-writer")
        self.state_path = state_path
        self.state_store = state_store
        self.stop_event = stop_event
        self.interval = interval

    def run(self):
        while not self.stop_event.is_set():
            time.sleep(self.interval)
            if not self.state_store.is_dirty():
                continue
            payload = self.state_store.snapshot()
            tmp_path = self.state_path + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                f.write(payload)
            os.replace(tmp_path, self.state_path)
            self.state_store.clear_dirty()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--placa-a", default=None, help="Porta serial da Placa A (mestre+analisador)")
    parser.add_argument("--placa-b", default=None, help="Porta serial da Placa B (escravo+jammer)")
    parser.add_argument("--logs-dir", default=LOGS_DIR)
    args = parser.parse_args()

    port_a = args.placa_a or resolve_default_port(PLACA_A_ID, "/dev/ttyUSB*")
    port_b = args.placa_b or resolve_default_port(PLACA_B_ID, "/dev/ttyACM*")

    if not port_a and not port_b:
        print("[logger] Nenhuma porta encontrada (nem --placa-a/--placa-b, nem /dev/serial/by-id, nem fallback glob).", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.logs_dir, exist_ok=True)

    stop_event = threading.Event()
    session_writer = SessionWriter(args.logs_dir)
    state_store = StateStore()

    boards = {}
    if port_a:
        boards["placa_a"] = SerialBoard("placa_a", port_a, session_writer, state_store, stop_event)
    else:
        print("[logger] Placa A não encontrada, seguindo só com Placa B.", file=sys.stderr)
    if port_b:
        boards["placa_b"] = SerialBoard("placa_b", port_b, session_writer, state_store, stop_event)
    else:
        print("[logger] Placa B não encontrada, seguindo só com Placa A.", file=sys.stderr)

    for b in boards.values():
        b.start()

    cmd_path = os.path.join(args.logs_dir, "cmd.in")
    cmd_watcher = CommandWatcher(cmd_path, boards, stop_event)
    cmd_watcher.start()

    state_path = os.path.join(args.logs_dir, "state.json")
    state_writer = StateWriter(state_path, state_store, stop_event)
    state_writer.start()

    def handle_sigterm(signum, frame):
        stop_event.set()

    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGINT, handle_sigterm)

    print(f"[logger] Sessao: {session_writer.session_path}", file=sys.stderr)
    print(f"[logger] Placas: {list(boards.keys())}", file=sys.stderr)
    print(f"[logger] Comandos: echo '<placa> <comando>' >> {cmd_path}", file=sys.stderr)

    try:
        while not stop_event.is_set():
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop_event.set()
    finally:
        session_writer.close()


if __name__ == "__main__":
    main()
