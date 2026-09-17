"""
TCC — Painel de Monitoramento do Sistema

Visualiza em tempo real o estado dos 4 nós (mestre, escravo, analisador,
jammer) a partir da serial das placas, sem assumir qual porta USB é qual
placa — cada linha recebida é classificada pelo próprio conteúdo (prefixo
"[MESTRE]", "PLR;...", CSV de espectro, etc.) e atualiza o painel do nó
correspondente. Funciona tanto com a bancada de 2 placas (env:placa_a +
env:placa_b, cada uma com 2 nós combinados) quanto com a de 4 placas
standalone — o parser não depende da topologia, só do que aparece na serial.

Uso:
    cd tools/dashboard
    pip install -r ../spectrum/requirements.txt
    python main.py
"""

import re
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from queue import Empty, Queue
from typing import Optional

import numpy as np
import serial
import serial.tools.list_ports
import tkinter as tk
from tkinter import ttk, scrolledtext
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from matplotlib.colors import LinearSegmentedColormap

# ---------------------------------------------------------------------------
# Constants (espelham shared/hopping.h e analisador/src/node.h)
# ---------------------------------------------------------------------------
BAUD_RATE = 115200
N_CHANNELS = 126
HOPPING_CHANNELS = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]
WIFI_CHANNELS = {1: 12, 6: 37, 11: 62}
SPECTRUM_ROWS = 90  # profundidade do histórico no heatmap (linhas)
PLR_POINTS = 120  # pontos no gráfico de PLR
HEARTBEAT_TIMEOUT_S = 3.0  # sem '.' do jammer por esse tempo -> considerado parado

_HEATMAP_COLORS = [(0, 0, 0.8), (0, 0.8, 0), (0.8, 0.8, 0), (0.8, 0, 0)]
SPECTRUM_CMAP = LinearSegmentedColormap.from_list("spectrum", _HEATMAP_COLORS, N=256)

COLOR_OK = "#2e7d32"
COLOR_WARN = "#e65100"
COLOR_BAD = "#c62828"
COLOR_IDLE = "#757575"


def guess_ports() -> list:
    keywords = ("usb", "serial", "cp210", "ch340", "ch910", "silicon", "esp32", "uart", "enhanced", "acm")
    scored = []
    for info in serial.tools.list_ports.comports():
        desc = f"{info.device} {info.description} {info.manufacturer or ''}".lower()
        scored.append((0 if any(k in desc for k in keywords) else 1, info.device))
    scored.sort()
    return [dev for _, dev in scored]


# ---------------------------------------------------------------------------
# Node state (atualizado pelo classificador de linhas, lido pela UI)
# ---------------------------------------------------------------------------
@dataclass
class MestreState:
    connected: bool = False
    link_established: bool = False
    seed_confirmed: bool = False
    hop_counter: int = 0
    current_channel: int = 0
    frag: str = "-"
    ack_count: int = 0
    timeout_count: int = 0
    fail_count: int = 0
    alert_active: bool = False
    last_event: str = "—"
    last_seen: float = 0.0


@dataclass
class EscravoState:
    connected: bool = False
    link_established: bool = False
    hopping: bool = False  # inferido de Canal=/PLR, independe de ter visto "Seed recebida"
    seed: Optional[int] = None
    current_channel: int = 0
    plr_window: float = 0.0
    plr_session: float = 0.0
    plr_history: deque = field(default_factory=lambda: deque(maxlen=PLR_POINTS))
    messages_done: int = 0
    last_event: str = "—"
    last_seen: float = 0.0


@dataclass
class AnalisadorState:
    connected: bool = False
    attack_active: bool = False
    last_alert_detail: str = "—"
    spectrum_rows: deque = field(default_factory=lambda: deque(maxlen=SPECTRUM_ROWS))
    last_row: Optional[list] = None
    last_seen: float = 0.0


@dataclass
class JammerState:
    connected: bool = False
    mode: str = "desconhecido"
    last_heartbeat: float = 0.0
    heartbeat_count: int = 0
    last_event: str = "—"
    error: Optional[str] = None
    last_seen: float = 0.0


class SystemState:
    def __init__(self):
        self.mestre = MestreState()
        self.escravo = EscravoState()
        self.analisador = AnalisadorState()
        self.jammer = JammerState()
        self.lock = threading.Lock()


# ---------------------------------------------------------------------------
# Line classification — independe de qual porta/placa a linha veio
# ---------------------------------------------------------------------------
RE_HOP = re.compile(r"Hop=(\d+) canal=(\d+) frag=(\d+)/(\d+) len=(\d+) enviada")
RE_ACK = re.compile(r"ACK recebido para frag=(\d+)")
RE_TIMEOUT = re.compile(r"Timeout ACK no hop=(\d+)")
RE_FAIL = re.compile(r"Falha de envio no hop=(\d+)")

RE_PLR = re.compile(r"PLR;t_ms=(\d+);window=([\d.]+);session=([\d.]+);recv=(\d+);exp=(\d+)")
RE_ESCRAVO_SEED = re.compile(r"Seed recebida: (\d+)")
RE_ESCRAVO_CH = re.compile(r"Canal=(\d+) frag=(\d+)/(\d+) recebida")

RE_ALVO = re.compile(r"alvo_avg=(-?\d+) fora_avg=(-?\d+)")


def parse_csv_row(line: str) -> Optional[list]:
    if not line or not line[0].isdigit():
        return None
    parts = [p.strip() for p in line.split(",") if p.strip() != ""]
    if len(parts) < N_CHANNELS:
        return None
    try:
        values = [int(p) for p in parts[:N_CHANNELS]]
    except ValueError:
        return None
    if any(v < 0 or v > 100 for v in values):
        return None
    return values


def classify_line(line: str, state: SystemState, now: float, log_cb) -> None:
    line = line.strip()
    if not line:
        return

    # O heartbeat do jammer ('.' sem newline) pode ficar colado à frente da
    # próxima linha: em placa_b, escravo e jammer rodam em tasks FreeRTOS
    # separadas escrevendo na mesma Serial sem sincronismo, então um "."
    # pode aparecer como prefixo de "PLR;..." ou "[ESCRAVO] ..." em vez de
    # sozinho. Consome os pontos como pulsos de heartbeat antes de
    # classificar o restante da linha.
    stripped = line.lstrip(".")
    n_dots = len(line) - len(stripped)
    if n_dots:
        with state.lock:
            j = state.jammer
            j.connected = True
            j.last_seen = now
            j.last_heartbeat = now
            j.heartbeat_count += n_dots
        line = stripped.strip()
        if not line:
            return

    with state.lock:
        if line.startswith("[MESTRE]"):
            m = state.mestre
            m.connected = True
            m.last_seen = now
            body = line[len("[MESTRE]"):].strip()
            hop = RE_HOP.search(body)
            if hop:
                m.hop_counter = int(hop.group(1))
                m.current_channel = int(hop.group(2))
                m.frag = f"{hop.group(3)}/{hop.group(4)}"
                m.last_event = "Fragmento enviado"
                # Só ocorre hop se pareamento+seed já foram concluídos;
                # cobre o caso de conectar o painel depois desses eventos.
                m.link_established = True
                m.seed_confirmed = True
            elif RE_ACK.search(body):
                m.ack_count += 1
                m.last_event = "ACK recebido"
            elif RE_TIMEOUT.search(body):
                m.timeout_count += 1
                m.last_event = "Timeout de ACK"
            elif RE_FAIL.search(body):
                m.fail_count += 1
                m.last_event = "Falha de envio"
            elif "Pareamento confirmado" in body:
                m.link_established = True
                m.last_event = "Pareamento confirmado"
            elif "Seed confirmada" in body:
                m.seed_confirmed = True
                m.last_event = "Seed confirmada"
            elif "ALERTA: ataque detectado" in body:
                m.alert_active = True
                m.last_event = "ALERTA recebido do analisador"
            elif "ALERTA: ataque encerrado" in body:
                m.alert_active = False
                m.last_event = "Alerta encerrado"
            elif "Mensagem completa confirmada" in body:
                m.last_event = "Mensagem confirmada pelo escravo"
            else:
                m.last_event = body
            log_cb("MESTRE", line)
            return

        if line.startswith("[ESCRAVO]") or line.startswith("PLR;"):
            e = state.escravo
            e.connected = True
            e.last_seen = now
            plr = RE_PLR.match(line)
            if plr:
                e.link_established = True  # PLR só é logado durante hopping
                e.hopping = True
                e.plr_window = float(plr.group(2))
                e.plr_session = float(plr.group(3))
                e.plr_history.append((now, e.plr_window, e.plr_session))
                log_cb("ESCRAVO", line)
                return
            body = line[len("[ESCRAVO]"):].strip()
            seed = RE_ESCRAVO_SEED.search(body)
            ch = RE_ESCRAVO_CH.search(body)
            if "Pareamento confirmado" in body:
                e.link_established = True
                e.last_event = "Pareamento confirmado"
            elif seed:
                e.seed = int(seed.group(1))
                e.last_event = f"Seed recebida: {e.seed}"
            elif ch:
                e.current_channel = int(ch.group(1))
                e.last_event = f"Fragmento {ch.group(2)}/{ch.group(3)} recebido"
                # Só recebe fragmento em canal de hopping se já pareou —
                # cobre o caso de conectar o painel depois desse evento.
                e.link_established = True
                e.hopping = True
            elif "Mensagem completa reconstruida" in body:
                e.messages_done += 1
                e.last_event = "Mensagem reconstruída"
            else:
                e.last_event = body
            log_cb("ESCRAVO", line)
            return

        if line.startswith("[ALERTA]"):
            a = state.analisador
            a.connected = True
            a.last_seen = now
            alvo = RE_ALVO.search(line)
            if "detectado" in line:
                a.attack_active = True
                a.last_alert_detail = (
                    f"alvo={alvo.group(1)} fora={alvo.group(2)}" if alvo else "ataque detectado"
                )
            elif "encerrado" in line:
                a.attack_active = False
                a.last_alert_detail = "ataque encerrado"
            log_cb("ANALISADOR", line)
            return

        csv_row = parse_csv_row(line)
        if csv_row is not None:
            a = state.analisador
            a.connected = True
            a.last_seen = now
            a.last_row = csv_row
            a.spectrum_rows.append(csv_row)
            return  # linhas de CSV não vão pro log (uma por ciclo de varredura)

        if line.startswith("[MODE]"):
            j = state.jammer
            j.connected = True
            j.last_seen = now
            j.mode = "sweep" if "sweep" in line else ("targeted" if "targeted" in line else line)
            j.last_event = line
            log_cb("JAMMER", line)
            return

        if set(line) == {"."}:
            j = state.jammer
            j.connected = True
            j.last_seen = now
            j.last_heartbeat = now
            j.heartbeat_count += len(line)
            return  # heartbeat não polui o log

        if line.startswith("[JAMMER]") or line.startswith("[OK]") and ("radio" in line.lower()):
            j = state.jammer
            j.connected = True
            j.last_seen = now
            j.last_event = line
            j.error = None
            log_cb("JAMMER", line)
            return

        if line.startswith("[ERRO]") and "radio" in line.lower():
            j = state.jammer
            j.connected = True
            j.last_seen = now
            j.error = line
            log_cb("JAMMER", line)
            return

    log_cb("OUTRO", line)


# ---------------------------------------------------------------------------
# Serial reader thread — generico, so entrega linhas cruas pra fila
# ---------------------------------------------------------------------------
class SerialReaderThread(threading.Thread):
    def __init__(self, port: str, line_queue: Queue, status_cb=None):
        super().__init__(daemon=True)
        self.port = port
        self.line_queue = line_queue
        self.status_cb = status_cb
        self._stop_event = threading.Event()
        self.error: Optional[str] = None
        self.bytes_received = 0
        self.raw_lines = 0
        self.last_line = ""
        self._ser: Optional[serial.Serial] = None
        self._write_lock = threading.Lock()

    def send_line(self, text: str) -> bool:
        """Escreve texto + '\\n' na serial — necessário pro fluxo de setup
        interativo do mestre (\"Digite a seed desejada (0-100)...\"), que
        bloqueia em setup() esperando entrada antes de iniciar o pareamento."""
        with self._write_lock:
            if not self._ser or not self._ser.is_open:
                return False
            try:
                self._ser.write((text.strip() + "\n").encode("utf-8"))
                self._ser.flush()
                return True
            except serial.SerialException:
                return False

    def run(self):
        try:
            ser = serial.Serial()
            ser.port = self.port
            ser.baudrate = BAUD_RATE
            ser.timeout = 1
            ser.dtr = False
            ser.rts = False
            ser.open()
            self._ser = ser
            time.sleep(0.3)
            while not self._stop_event.is_set():
                raw = ser.readline()
                if not raw:
                    continue
                self.bytes_received += len(raw)
                line = raw.decode("utf-8", errors="ignore").strip()
                if line:
                    self.raw_lines += 1
                    self.last_line = line
                    self.line_queue.put(line)
            ser.close()
        except serial.SerialException as exc:
            self.error = f"Nao foi possivel abrir {self.port}: {exc}"
            if self.status_cb:
                self.status_cb(self.error)
        except Exception as exc:  # pragma: no cover
            self.error = str(exc)
            if self.status_cb:
                self.status_cb(self.error)
        finally:
            self._ser = None

    def stop(self):
        self._stop_event.set()


# ---------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------
class PortPanel(ttk.LabelFrame):
    """Um seletor de porta + botao conectar/desconectar. O papel (qual nó
    fala nessa porta) é descoberto pelo conteúdo das linhas, não fixado
    aqui — funciona tanto com env:placa_a/placa_b quanto com os 4 nós
    standalone em portas separadas."""

    def __init__(self, parent, title: str, line_queue: Queue, default_port: Optional[str]):
        super().__init__(parent, text=title, padding=8)
        self.line_queue = line_queue
        self.reader: Optional[SerialReaderThread] = None

        row = ttk.Frame(self)
        row.pack(fill=tk.X)
        self.port_cb = ttk.Combobox(row, state="readonly", width=16)
        self.port_cb.pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(row, text="Atualizar", command=self.refresh_ports).pack(side=tk.LEFT)
        self.toggle_btn = ttk.Button(row, text="Conectar", command=self.toggle)
        self.toggle_btn.pack(side=tk.LEFT, padx=(6, 0))

        self.status_lbl = ttk.Label(self, text="Desconectado", foreground=COLOR_IDLE)
        self.status_lbl.pack(anchor="w", pady=(4, 0))
        self.raw_lbl = ttk.Label(self, text="", foreground=COLOR_IDLE, font=("TkDefaultFont", 8))
        self.raw_lbl.pack(anchor="w")

        # Alguns nós esperam entrada interativa antes de seguir em frente
        # (ex.: mestre bloqueia em setup() até receber a seed pelo serial
        # monitor) — sem isso o painel fica só observando, sem forma de
        # destravar o node.
        send_row = ttk.Frame(self)
        send_row.pack(fill=tk.X, pady=(4, 0))
        self.send_entry = ttk.Entry(send_row, width=14)
        self.send_entry.pack(side=tk.LEFT, padx=(0, 4))
        self.send_entry.bind("<Return>", lambda _evt: self.send())
        ttk.Button(send_row, text="Enviar", command=self.send).pack(side=tk.LEFT)

        self.refresh_ports()
        if default_port and default_port in self.port_cb["values"]:
            self.port_cb.set(default_port)

    def refresh_ports(self):
        ports = guess_ports()
        current = self.port_cb.get()
        self.port_cb["values"] = ports
        if current in ports:
            self.port_cb.set(current)
        elif ports:
            self.port_cb.set(ports[0])

    def send(self):
        text = self.send_entry.get()
        if not text or not self.reader:
            return
        ok = self.reader.send_line(text)
        self.send_entry.delete(0, tk.END)
        if not ok:
            self.raw_lbl.config(text=f"Falha ao enviar {text!r}", foreground=COLOR_BAD)

    def toggle(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
            self.toggle_btn.config(text="Conectar")
            self.status_lbl.config(text="Desconectado", foreground=COLOR_IDLE)
            self.raw_lbl.config(text="")
            return

        port = self.port_cb.get()
        if not port:
            self.status_lbl.config(text="Selecione uma porta", foreground=COLOR_BAD)
            return
        self.reader = SerialReaderThread(port, self.line_queue, status_cb=self._on_error)
        self.reader.start()
        self.toggle_btn.config(text="Desconectar")
        self.status_lbl.config(text=f"Conectado em {port}", foreground=COLOR_OK)
        self.raw_lbl.config(text="Aguardando dados...", foreground=COLOR_IDLE)

    def tick(self):
        """Mostra contagem de linhas cruas e a última recebida, mesmo que
        não tenha sido reconhecida por nenhum classificador — sem isso,
        'nada aparece no painel' e 'nenhum byte chega na porta' ficam
        indistinguíveis pra quem está depurando."""
        if not self.reader or not self.reader.is_alive():
            return
        preview = self.reader.last_line[:48]
        if self.reader.raw_lines == 0:
            self.raw_lbl.config(
                text=f"0 linhas recebidas ({self.reader.bytes_received} bytes brutos) — verifique porta/baud/firmware",
                foreground=COLOR_WARN if self.reader.bytes_received == 0 else COLOR_BAD,
            )
        else:
            self.raw_lbl.config(
                text=f"{self.reader.raw_lines} linhas — última: {preview!r}",
                foreground=COLOR_IDLE,
            )

    def _on_error(self, message: str):
        self.status_lbl.config(text=message, foreground=COLOR_BAD)
        self.reader = None
        self.after(0, lambda: self.toggle_btn.config(text="Conectar"))

    def stop(self):
        if self.reader:
            self.reader.stop()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TCC — Painel de Monitoramento do Sistema")
        self.geometry("1360x1040")
        self.minsize(1180, 860)

        self.state_obj = SystemState()
        self.log_lines: deque = deque(maxlen=500)

        self._build_layout()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(150, self._poll_queues)
        self.after(300, self._refresh_ui)

    # -- layout --------------------------------------------------------
    def _build_layout(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill=tk.X)

        ports = guess_ports()
        p1 = ports[0] if len(ports) > 0 else None
        p2 = ports[1] if len(ports) > 1 else None

        self.queue_a: Queue = Queue()
        self.queue_b: Queue = Queue()
        self.panel_a = PortPanel(top, "Porta 1", self.queue_a, p1)
        self.panel_a.pack(side=tk.LEFT, padx=(0, 8))
        self.panel_b = PortPanel(top, "Porta 2", self.queue_b, p2)
        self.panel_b.pack(side=tk.LEFT)

        self.alert_lbl = tk.Label(
            self, text="Sem alerta de ataque", bg="#2e7d32", fg="white",
            font=("TkDefaultFont", 13, "bold"), pady=8,
        )
        self.alert_lbl.pack(fill=tk.X, padx=8, pady=(0, 6))

        # PanedWindow em vez de pack empilhado: numa tela pequena, o corpo
        # (espectro+status) nunca fica menor que seu minsize e o log nunca
        # desaparece por completo (pack simples zera widgets sem espaço
        # sobrando) — o usuário arrasta o divisor pra ver mais de um ou outro.
        main_pane = tk.PanedWindow(self, orient=tk.VERTICAL, sashwidth=6, sashrelief=tk.RAISED)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

        body = ttk.Frame(main_pane)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        # minsize evita que o painel de enlace (~246px de conteúdo) seja
        # espremido abaixo de suas linhas quando a janela fica pequena — o
        # espectro pode encolher sem perder informação, texto de status não.
        body.rowconfigure(0, weight=3, minsize=260)
        body.rowconfigure(1, weight=2, minsize=300)

        # Espectro (canto superior esquerdo, ocupa 2 linhas)
        spectrum_frame = ttk.LabelFrame(body, text="Espectro RF (analisador)", padding=6)
        spectrum_frame.grid(row=0, column=0, rowspan=2, sticky="nsew", padx=(0, 8))
        self._build_spectrum(spectrum_frame)

        # Enlace mestre/escravo
        link_frame = ttk.LabelFrame(body, text="Enlace Mestre ↔ Escravo", padding=8)
        link_frame.grid(row=0, column=1, sticky="nsew", pady=(0, 8))
        self._build_link_panel(link_frame)

        # Jammer + PLR
        bottom_right = ttk.Frame(body)
        bottom_right.grid(row=1, column=1, sticky="nsew")
        bottom_right.rowconfigure(0, weight=1)
        bottom_right.rowconfigure(1, weight=1)

        jammer_frame = ttk.LabelFrame(bottom_right, text="Jammer", padding=8)
        jammer_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 8))
        self._build_jammer_panel(jammer_frame)

        plr_frame = ttk.LabelFrame(bottom_right, text="PLR (escravo)", padding=6)
        plr_frame.pack(fill=tk.BOTH, expand=True)
        self._build_plr_panel(plr_frame)

        # Log
        log_frame = ttk.LabelFrame(main_pane, text="Log combinado", padding=6)
        self.log_text = scrolledtext.ScrolledText(log_frame, height=7, state="disabled", wrap="none")
        self.log_text.pack(fill=tk.BOTH, expand=True)
        for tag, color in (
            ("MESTRE", "#1565c0"), ("ESCRAVO", "#2e7d32"),
            ("ANALISADOR", "#6a1b9a"), ("JAMMER", "#c62828"), ("OUTRO", "#757575"),
        ):
            self.log_text.tag_config(tag, foreground=color)

        main_pane.add(body, minsize=520, stretch="always")
        main_pane.add(log_frame, minsize=90, stretch="never")

    def _build_spectrum(self, parent):
        self.spec_fig = Figure(figsize=(6, 6), tight_layout=True)
        self.spec_ax = self.spec_fig.add_subplot(111)
        self.spec_ax.set_xlabel("Canal NRF24 (2400 + ch MHz)")
        self.spec_ax.set_ylabel("Tempo →")
        self.spec_ax.set_yticks([])
        self.spec_img = self.spec_ax.imshow(
            np.zeros((1, N_CHANNELS)), aspect="auto", cmap=SPECTRUM_CMAP,
            vmin=0, vmax=100, interpolation="nearest", origin="lower",
        )
        for ch in HOPPING_CHANNELS:
            self.spec_ax.axvline(ch, color="white", alpha=0.25, linewidth=0.8)
        for label, ch in WIFI_CHANNELS.items():
            self.spec_ax.axvline(ch, color="orange", linestyle="--", alpha=0.5, linewidth=1)
        self.spec_channel_marker = self.spec_ax.axvline(0, color="cyan", linewidth=1.5, visible=False)
        self.spec_canvas = FigureCanvasTkAgg(self.spec_fig, master=parent)
        self.spec_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_link_panel(self, parent):
        self.link_vars = {}
        rows = [
            ("mestre_status", "Mestre"),
            ("escravo_status", "Escravo"),
            ("channel", "Canal atual (mestre/escravo)"),
            ("hop", "Hop #"),
            ("frag", "Fragmento"),
            ("ack_rate", "Taxa de ACK"),
            ("seed", "Seed"),
            ("mestre_event", "Último evento (mestre)"),
            ("escravo_event", "Último evento (escravo)"),
        ]
        for i, (key, label) in enumerate(rows):
            ttk.Label(parent, text=label + ":", font=("TkDefaultFont", 9, "bold")).grid(
                row=i, column=0, sticky="w", pady=2
            )
            var = tk.StringVar(value="—")
            ttk.Label(parent, textvariable=var, wraplength=260).grid(row=i, column=1, sticky="w", padx=(8, 0))
            self.link_vars[key] = var
        parent.columnconfigure(1, weight=1)

    def _build_jammer_panel(self, parent):
        self.jammer_vars = {}
        for i, (key, label) in enumerate([
            ("status", "Conexão"), ("mode", "Modo"),
            ("heartbeat", "Heartbeat"), ("event", "Último evento"),
        ]):
            ttk.Label(parent, text=label + ":", font=("TkDefaultFont", 9, "bold")).grid(
                row=i, column=0, sticky="w", pady=2
            )
            var = tk.StringVar(value="—")
            ttk.Label(parent, textvariable=var, wraplength=260).grid(row=i, column=1, sticky="w", padx=(8, 0))
            self.jammer_vars[key] = var
        parent.columnconfigure(1, weight=1)

    def _build_plr_panel(self, parent):
        self.plr_fig = Figure(figsize=(4, 2.4), tight_layout=True)
        self.plr_ax = self.plr_fig.add_subplot(111)
        self.plr_ax.set_ylim(0, 100)
        self.plr_ax.set_ylabel("PLR (%)")
        self.plr_ax.grid(True, alpha=0.3)
        (self.plr_window_line,) = self.plr_ax.plot([], [], color="#e65100", label="janela")
        (self.plr_session_line,) = self.plr_ax.plot([], [], color="#1565c0", label="sessão")
        self.plr_ax.legend(loc="upper right", fontsize=8)
        self.plr_canvas = FigureCanvasTkAgg(self.plr_fig, master=parent)
        self.plr_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # -- data pump -------------------------------------------------------
    def _poll_queues(self):
        now = time.time()
        for q in (self.queue_a, self.queue_b):
            drained = 0
            try:
                while drained < 500:
                    line = q.get_nowait()
                    classify_line(line, self.state_obj, now, self._append_log)
                    drained += 1
            except Empty:
                pass
        self.after(150, self._poll_queues)

    def _append_log(self, tag: str, line: str):
        self.log_lines.append((tag, line))

    # -- render ------------------------------------------------------------
    def _refresh_ui(self):
        self.panel_a.tick()
        self.panel_b.tick()
        now = time.time()
        with self.state_obj.lock:
            m = self.state_obj.mestre
            e = self.state_obj.escravo
            a = self.state_obj.analisador
            j = self.state_obj.jammer

            attack = m.alert_active or a.attack_active
            if attack:
                self.alert_lbl.config(
                    text=f"🚨 ATAQUE DETECTADO — {a.last_alert_detail}", bg=COLOR_BAD,
                )
            else:
                self.alert_lbl.config(text="Sem alerta de ataque", bg=COLOR_OK)

            self.link_vars["mestre_status"].set(self._link_status_text(m.connected, m.link_established, m.seed_confirmed, m.last_seen, now))
            self.link_vars["escravo_status"].set(self._link_status_text(e.connected, e.link_established, e.hopping, e.last_seen, now))
            self.link_vars["channel"].set(f"{m.current_channel} / {e.current_channel}")
            self.link_vars["hop"].set(str(m.hop_counter))
            self.link_vars["frag"].set(m.frag)
            total_acks = m.ack_count + m.timeout_count
            rate = f"{100.0 * m.ack_count / total_acks:.0f}% ({m.ack_count}/{total_acks})" if total_acks else "—"
            self.link_vars["ack_rate"].set(rate)
            self.link_vars["seed"].set(str(e.seed) if e.seed is not None else "—")
            self.link_vars["mestre_event"].set(m.last_event)
            self.link_vars["escravo_event"].set(e.last_event)

            self.jammer_vars["status"].set("conectado" if j.connected and (now - j.last_seen) < 5 else "sem dados")
            self.jammer_vars["mode"].set(j.mode)
            if j.last_heartbeat:
                since = now - j.last_heartbeat
                active = since < HEARTBEAT_TIMEOUT_S
                self.jammer_vars["heartbeat"].set(
                    f"{'ativo' if active else 'parado'} (há {since:.1f}s, {j.heartbeat_count} pulsos)"
                )
            else:
                self.jammer_vars["heartbeat"].set("sem heartbeat")
            self.jammer_vars["event"].set(j.error or j.last_event)

            spectrum_rows = list(a.spectrum_rows)
            current_channel = m.current_channel
            plr_history = list(e.plr_history)

        if spectrum_rows:
            self.spec_img.set_data(np.array(spectrum_rows))
            self.spec_img.set_extent((0, N_CHANNELS - 1, 0, len(spectrum_rows)))
            self.spec_canvas.draw_idle()
        if current_channel:
            self.spec_channel_marker.set_xdata([current_channel, current_channel])
            self.spec_channel_marker.set_visible(True)

        if plr_history:
            xs = list(range(len(plr_history)))
            self.plr_window_line.set_data(xs, [p[1] for p in plr_history])
            self.plr_session_line.set_data(xs, [p[2] for p in plr_history])
            self.plr_ax.set_xlim(0, max(10, len(plr_history)))
            self.plr_canvas.draw_idle()

        self._render_log()
        self.after(300, self._refresh_ui)

    @staticmethod
    def _link_status_text(connected, link_established, seed_ok, last_seen, now) -> str:
        if not connected or (now - last_seen) > 6:
            return "sem dados"
        if seed_ok:
            return "hopping (seed ok)"
        if link_established:
            return "pareado, aguardando seed"
        return "aguardando pareamento"

    def _render_log(self):
        self.log_text.config(state="normal")
        self.log_text.delete("1.0", tk.END)
        for tag, line in list(self.log_lines)[-200:]:
            self.log_text.insert(tk.END, line + "\n", tag)
        self.log_text.see(tk.END)
        self.log_text.config(state="disabled")

    def _on_close(self):
        self.panel_a.stop()
        self.panel_b.stop()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
