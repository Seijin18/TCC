"""
TCC RF Spectrum Analyzer
Visualiza dados do analisador NRF24 (env:analisador) para validar jammer vs baseline.

Uso:
    cd tools/spectrum
    pip install -r requirements.txt
    python main.py
"""

import os
import re
import glob
import threading
import time
from datetime import datetime
from typing import List, Optional

import numpy as np
import pandas as pd
import serial
import serial.tools.list_ports
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
from matplotlib.colors import LinearSegmentedColormap


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
SAVED_DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "saved_data")
N_CHANNELS = 126
BAUD_RATE = 115200

# WiFi ch -> NRF24 channel index (2400 + ch MHz)
WIFI_CHANNELS = {1: 12, 6: 37, 11: 62}

# Banda Bluetooth Classic + BLE (2402–2480 MHz)
BT_CH_MIN = 2
BT_CH_MAX = 80

# Canais FHSS do TCC (shared/hopping.h)
HOPPING_CHANNELS = [10, 22, 34, 46, 58, 70, 82, 94, 100, 105, 110, 115, 120, 125, 15, 27]

_HEATMAP_COLORS = [(0, 0, 0.8), (0, 0.8, 0), (0.8, 0.8, 0), (0.8, 0, 0)]
SPECTRUM_CMAP = LinearSegmentedColormap.from_list("spectrum", _HEATMAP_COLORS, N=256)


def draw_spectrum_markers(ax, *, vertical=True, show_labels=True):
    """Marca banda BT, canais Wi-Fi e canais FHSS do TCC."""
    if vertical:
        ax.axvspan(BT_CH_MIN, BT_CH_MAX, color="#2196F3", alpha=0.08, label="BT 2402–2480 MHz")
        for label, ch in WIFI_CHANNELS.items():
            ax.axvline(ch, color="orange", linestyle="--", alpha=0.6, linewidth=1)
            if show_labels:
                ax.text(ch + 0.5, 95, f"Wi-Fi {label}", color="orange", fontsize=7, va="top")
        for ch in HOPPING_CHANNELS:
            if ch >= N_CHANNELS:
                continue
            ax.axvline(ch, color="#4CAF50", linestyle=":", alpha=0.7, linewidth=0.9)
    else:
        ax.axhspan(BT_CH_MIN, BT_CH_MAX, color="#2196F3", alpha=0.12)
        for label, ch in WIFI_CHANNELS.items():
            if ch < N_CHANNELS:
                ax.axhline(ch, color="white", linestyle="--", alpha=0.5, linewidth=1)
                if show_labels:
                    ax.text(0.5, ch + 0.5, f"Wi-Fi {label}", color="white", fontsize=7, va="bottom")
        for ch in HOPPING_CHANNELS:
            if ch < N_CHANNELS:
                ax.axhline(ch, color="#A5D6A7", linestyle=":", alpha=0.6, linewidth=0.8)


# ---------------------------------------------------------------------------
# Serial parsing
# ---------------------------------------------------------------------------
def parse_spectrum_line(line: str) -> Optional[List[int]]:
    """Retorna 126 inteiros (0–100) ou None se a linha nao for um scan valido."""
    line = line.strip()
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


def guess_esp32_port() -> Optional[str]:
    keywords = ("usb", "serial", "cp210", "ch340", "ch910", "silicon", "esp32", "uart", "enhanced")
    for info in serial.tools.list_ports.comports():
        desc = f"{info.device} {info.description} {info.manufacturer or ''}".lower()
        if any(k in desc for k in keywords):
            return info.device
    ports = [p.device for p in serial.tools.list_ports.comports()]
    return ports[0] if ports else None


# ---------------------------------------------------------------------------
# Serial reader thread
# ---------------------------------------------------------------------------
class SerialReaderThread(threading.Thread):
    def __init__(
        self,
        port: str,
        baudrate: int = BAUD_RATE,
        callback=None,
        on_error=None,
        on_stats=None,
        on_jammer_status=None,
    ):
        super().__init__(daemon=True)
        self.port = port
        self.baudrate = baudrate
        self.callback = callback
        self.on_error = on_error
        self.on_stats = on_stats
        self.on_jammer_status = on_jammer_status
        self._stop_event = threading.Event()
        self._ready_event = threading.Event()
        self._jammer_ack = threading.Event()
        self._last_jammer_line = ""
        self._write_lock = threading.Lock()
        self._start_time = None
        self.error: Optional[str] = None
        self.bytes_received = 0
        self.raw_lines = 0
        self.parsed_lines = 0
        self._ser: Optional[serial.Serial] = None

    def send_command(
        self,
        command: str,
        retries: int = 3,
        wait_prefix: Optional[str] = None,
        wait_timeout: float = 2.5,
    ) -> bool:
        if not self._ready_event.wait(timeout=5):
            return False
        payload = (command.strip() + "\n").encode("utf-8")
        if wait_prefix:
            self._jammer_ack.clear()
        for attempt in range(retries):
            if not self._ser or not self._ser.is_open:
                time.sleep(0.05)
                continue
            try:
                with self._write_lock:
                    self._ser.write(payload)
                    self._ser.flush()
                if not wait_prefix:
                    return True
                if self._jammer_ack.wait(timeout=wait_timeout):
                    return self._last_jammer_line.upper().startswith(wait_prefix.upper())
                time.sleep(0.05)
            except serial.SerialException:
                time.sleep(0.05)
        return False

    @staticmethod
    def _handle_line(
        line: str,
        callback,
        on_jammer_status,
        on_stats,
        thread: "SerialReaderThread",
    ) -> None:
        if line.startswith("[JAMMER]") or line.startswith("[STATUS]"):
            thread._last_jammer_line = line
            thread._jammer_ack.set()
            if on_jammer_status:
                on_jammer_status(line)
            if on_stats:
                on_stats(thread.bytes_received, thread.raw_lines, thread.parsed_lines)
            return

        if "[OK] Radios prontos" in line or "Enviando CSV" in line:
            thread._mark_ready()

        channel_values = parse_spectrum_line(line)
        if channel_values is None:
            if on_stats:
                on_stats(thread.bytes_received, thread.raw_lines, thread.parsed_lines)
            return

        thread._mark_ready()
        thread.parsed_lines += 1
        timestamp = time.perf_counter() - thread._start_time
        values = [timestamp] + channel_values
        if callback:
            callback(values)
        if on_stats:
            on_stats(thread.bytes_received, thread.raw_lines, thread.parsed_lines)

    def _mark_ready(self) -> None:
        if not self._ready_event.is_set():
            self._ready_event.set()

    def run(self):
        ser = None
        try:
            ser = serial.Serial()
            ser.port = self.port
            ser.baudrate = self.baudrate
            ser.timeout = 1
            ser.dtr = False
            ser.rts = False
            ser.open()
            self._ser = ser
            self._start_time = time.perf_counter()
            # Nao reinicia o ESP32: reset via DTR interrompe o stream e descarta dados.
            time.sleep(0.3)

            while not self._stop_event.is_set():
                raw = ser.readline()
                if not raw:
                    if self.on_stats:
                        self.on_stats(self.bytes_received, self.raw_lines, self.parsed_lines)
                    continue

                self.bytes_received += len(raw)
                line = raw.decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                self.raw_lines += 1
                self._handle_line(
                    line,
                    self.callback,
                    self.on_jammer_status,
                    self.on_stats,
                    self,
                )

            ser.close()
        except serial.SerialException as exc:
            self.error = (
                f"Nao foi possivel abrir {self.port}.\n\n"
                f"{exc}\n\n"
                "Feche o Monitor Serial do PlatformIO/Arduino antes de gravar."
            )
            if self.on_error:
                self.on_error(self.error)
        except Exception as exc:
            self.error = str(exc)
            if self.on_error:
                self.on_error(self.error)
        finally:
            self._ready_event.clear()
            self._jammer_ack.set()
            self._ser = None

    def stop(self):
        self._stop_event.set()


# ---------------------------------------------------------------------------
# Live spectrum canvas
# ---------------------------------------------------------------------------
class LiveSpectrumCanvas:
    def __init__(self, parent):
        self.fig = Figure(figsize=(9, 4), tight_layout=True)
        self.ax = self.fig.add_subplot(111)
        self._setup_axes()
        (self.line,) = self.ax.plot(
            range(N_CHANNELS), [0] * N_CHANNELS, color="#2196F3", linewidth=1.2
        )
        self.canvas = FigureCanvasTkAgg(self.fig, master=parent)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _setup_axes(self):
        self.ax.set_xlim(0, N_CHANNELS - 1)
        self.ax.set_ylim(0, 100)
        self.ax.set_xlabel("Canal NRF24  (2400 + ch MHz)")
        self.ax.set_ylabel("Atividade (%)")
        self.ax.set_title("Espectro RF ao vivo — TCC")
        self.ax.grid(True, alpha=0.3)
        draw_spectrum_markers(self.ax, vertical=True)

    def update(self, channel_values: list):
        y = channel_values[:N_CHANNELS]
        x = list(range(len(y)))
        self.line.set_xdata(x)
        self.line.set_ydata(y)
        self.canvas.draw_idle()


# ---------------------------------------------------------------------------
# Record tab
# ---------------------------------------------------------------------------
class RecordTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._samples: list = []
        self._reader: SerialReaderThread | None = None
        self._start_time: float | None = None
        self._timer_running = False
        self._port_map = {}
        self._jammer_state = tk.StringVar(value="Jammer: —")
        self._auto_jammer_var = tk.BooleanVar(value=True)
        self._auto_interval_var = tk.IntVar(value=10)
        self._jammer_auto_on: Optional[bool] = None
        self._jammer_toggle_pending = False

        self._build_controls()
        self._build_plot()

    def _build_controls(self):
        ctrl = ttk.LabelFrame(self, text="Conexao Serial (analisador)", padding=8)
        ctrl.pack(fill=tk.X, padx=10, pady=(10, 4))

        port_row = ttk.Frame(ctrl)
        port_row.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(port_row, text="Porta:").pack(side=tk.LEFT)
        self._port_cb = ttk.Combobox(port_row, state="readonly", width=18)
        self._port_cb.pack(side=tk.LEFT, padx=(4, 8))
        ttk.Button(port_row, text="Atualizar", command=self._refresh_ports).pack(side=tk.LEFT)

        fname_row = ttk.Frame(ctrl)
        fname_row.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(fname_row, text="Salvar como:").pack(side=tk.LEFT)
        self._fname_var = tk.StringVar(value="baseline")
        self._fname_entry = ttk.Entry(fname_row, textvariable=self._fname_var, width=30)
        self._fname_entry.pack(side=tk.LEFT, padx=(4, 4))
        ttk.Label(fname_row, text=".csv  ->  saved_data/", foreground="gray").pack(side=tk.LEFT)

        action_row = ttk.Frame(ctrl)
        action_row.pack(fill=tk.X)
        self._start_btn = ttk.Button(action_row, text="Iniciar", command=self._start)
        self._start_btn.pack(side=tk.LEFT, padx=(0, 6))
        self._stop_btn = ttk.Button(
            action_row, text="Parar e Salvar", command=self._stop, state=tk.DISABLED
        )
        self._stop_btn.pack(side=tk.LEFT, padx=(0, 20))
        self._time_lbl = ttk.Label(action_row, text="Tempo: 00:00:00")
        self._time_lbl.pack(side=tk.LEFT)
        self._sample_lbl = ttk.Label(action_row, text="  Amostras: 0", foreground="gray")
        self._sample_lbl.pack(side=tk.LEFT)

        jammer_row = ttk.Frame(ctrl)
        jammer_row.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(jammer_row, text="Controle jammer (validacao):").pack(side=tk.LEFT)
        self._jammer_on_btn = ttk.Button(
            jammer_row, text="Ligar jammer", command=self._jammer_on, state=tk.DISABLED
        )
        self._jammer_on_btn.pack(side=tk.LEFT, padx=(8, 4))
        self._jammer_off_btn = ttk.Button(
            jammer_row, text="Desligar jammer", command=self._jammer_off, state=tk.DISABLED
        )
        self._jammer_off_btn.pack(side=tk.LEFT, padx=(0, 12))
        self._jammer_lbl = ttk.Label(jammer_row, textvariable=self._jammer_state, foreground="gray")
        self._jammer_lbl.pack(side=tk.LEFT)

        auto_row = ttk.Frame(ctrl)
        auto_row.pack(fill=tk.X, pady=(6, 0))
        self._auto_cb = ttk.Checkbutton(
            auto_row,
            text="Ciclo automatico jammer",
            variable=self._auto_jammer_var,
            command=self._on_auto_jammer_toggle,
        )
        self._auto_cb.pack(side=tk.LEFT)
        ttk.Label(auto_row, text="Intervalo (s):").pack(side=tk.LEFT, padx=(12, 4))
        self._auto_interval_spin = ttk.Spinbox(
            auto_row,
            from_=5,
            to=120,
            increment=5,
            textvariable=self._auto_interval_var,
            width=5,
        )
        self._auto_interval_spin.pack(side=tk.LEFT)
        self._auto_phase_lbl = ttk.Label(auto_row, text="", foreground="gray")
        self._auto_phase_lbl.pack(side=tk.LEFT, padx=(12, 0))

        self._status_lbl = ttk.Label(
            ctrl,
            text=(
                "Com ciclo automatico: baseline -> jammer ON -> OFF -> ON a cada intervalo. "
                "Firmware validacao necessario."
            ),
            foreground="gray",
            wraplength=900,
        )
        self._status_lbl.pack(fill=tk.X, pady=(6, 0))

        self._refresh_ports()

    def _build_plot(self):
        plot_frame = ttk.LabelFrame(self, text="Sinal ao vivo", padding=4)
        plot_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=(4, 10))
        self._live_canvas = LiveSpectrumCanvas(plot_frame)

    def _refresh_ports(self):
        entries = []
        for info in serial.tools.list_ports.comports():
            label = f"{info.device} — {info.description}"
            entries.append((info.device, label))
        self._port_map = {label: dev for dev, label in entries}
        labels = [label for _, label in entries]
        self._port_cb["values"] = labels
        if not labels:
            return
        preferred = guess_esp32_port()
        for label, dev in self._port_map.items():
            if dev == preferred:
                self._port_cb.set(label)
                return
        self._port_cb.current(0)

    def _selected_port(self) -> str:
        label = self._port_cb.get()
        return self._port_map.get(label, label.split(" — ")[0])

    def _start(self):
        port = self._selected_port()
        if not port:
            messagebox.showwarning("Sem porta", "Selecione a porta serial do analisador.")
            return
        fname = self._fname_var.get().strip()
        if not fname:
            messagebox.showwarning("Sem nome", "Informe um nome para o arquivo.")
            return
        fname = re.sub(r'[<>:"/\\|?*]', "_", fname)
        self._fname_var.set(fname)

        self._samples = []
        self._start_time = time.time()
        self._timer_running = True
        self._jammer_auto_on = False
        self._jammer_toggle_pending = False
        self._reader = SerialReaderThread(
            port,
            callback=self._on_sample,
            on_error=self._on_serial_error,
            on_stats=self._on_serial_stats,
            on_jammer_status=self._on_jammer_status,
        )
        self._reader.start()
        self._jammer_state.set("Jammer: OFF (baseline)")
        self._update_auto_phase_label(0)
        self._status_lbl.config(
            text=f"Lendo {port}... aguarde scans CSV (feche o Monitor Serial antes).",
            foreground="blue",
        )

        self._start_btn.config(state=tk.DISABLED)
        self._stop_btn.config(state=tk.NORMAL)
        self._port_cb.config(state=tk.DISABLED)
        self._fname_entry.config(state=tk.DISABLED)
        self._auto_cb.config(state=tk.DISABLED)
        self._auto_interval_spin.config(state=tk.DISABLED)
        self._sync_jammer_manual_buttons()
        self._tick_timer()
        self.after(500, self._check_reader_alive)

    def _stop(self):
        self._timer_running = False
        self._jammer_auto_on = None
        self._jammer_toggle_pending = False
        if self._reader:
            if self._reader._ready_event.is_set():
                self._reader.send_command("jammer off", wait_prefix="[JAMMER] OFF", wait_timeout=2.0)
            self._reader.stop()
            self._reader.join(timeout=5)
            self._reader = None

        self._start_btn.config(state=tk.NORMAL)
        self._stop_btn.config(state=tk.DISABLED)
        self._jammer_on_btn.config(state=tk.DISABLED)
        self._jammer_off_btn.config(state=tk.DISABLED)
        self._jammer_state.set("Jammer: —")
        self._auto_phase_lbl.config(text="")
        self._port_cb.config(state=tk.NORMAL)
        self._fname_entry.config(state=tk.NORMAL)
        self._auto_cb.config(state=tk.NORMAL)
        self._auto_interval_spin.config(state=tk.NORMAL)
        self._time_lbl.config(text="Tempo: 00:00:00")

        self._save_data()

    def _on_serial_error(self, message: str):
        self.after(0, lambda: self._handle_serial_error(message))

    def _handle_serial_error(self, message: str):
        self._timer_running = False
        self._jammer_auto_on = None
        self._jammer_toggle_pending = False
        if self._reader:
            self._reader.stop()
            self._reader = None
        self._start_btn.config(state=tk.NORMAL)
        self._stop_btn.config(state=tk.DISABLED)
        self._jammer_on_btn.config(state=tk.DISABLED)
        self._jammer_off_btn.config(state=tk.DISABLED)
        self._port_cb.config(state=tk.NORMAL)
        self._fname_entry.config(state=tk.NORMAL)
        self._auto_cb.config(state=tk.NORMAL)
        self._auto_interval_spin.config(state=tk.NORMAL)
        self._auto_phase_lbl.config(text="")
        self._status_lbl.config(text="Erro na conexao serial.", foreground="red")
        messagebox.showerror("Erro serial", message)

    def _on_auto_jammer_toggle(self):
        self._sync_jammer_manual_buttons()
        if not self._timer_running:
            self._auto_phase_lbl.config(text="")

    def _sync_jammer_manual_buttons(self):
        manual = self._timer_running and not self._auto_jammer_var.get()
        state = tk.NORMAL if manual else tk.DISABLED
        self._jammer_on_btn.config(state=state)
        self._jammer_off_btn.config(state=state)

    def _auto_interval_seconds(self) -> int:
        try:
            return max(5, int(self._auto_interval_var.get()))
        except (tk.TclError, ValueError):
            return 10

    def _desired_jammer_on(self, elapsed: int) -> bool:
        interval = self._auto_interval_seconds()
        if elapsed < interval:
            return False
        return ((elapsed - interval) // interval) % 2 == 0

    def _seconds_until_auto_change(self, elapsed: int) -> int:
        interval = self._auto_interval_seconds()
        if elapsed < interval:
            return interval - elapsed
        return interval - ((elapsed - interval) % interval)

    def _update_auto_phase_label(self, elapsed: int):
        if not self._auto_jammer_var.get() or not self._timer_running:
            self._auto_phase_lbl.config(text="")
            return
        interval = self._auto_interval_seconds()
        remaining = self._seconds_until_auto_change(elapsed)
        if elapsed < interval:
            phase = f"baseline — jammer ON em {remaining}s"
        elif self._desired_jammer_on(elapsed):
            phase = f"jammer ON — OFF em {remaining}s"
        else:
            phase = f"baseline — ON em {remaining}s"
        self._auto_phase_lbl.config(text=phase)

    def _apply_auto_jammer(self, elapsed: int):
        if not self._auto_jammer_var.get() or not self._reader:
            return
        desired = self._desired_jammer_on(elapsed)
        if self._jammer_auto_on == desired or self._jammer_toggle_pending:
            return

        command = "jammer on" if desired else "jammer off"
        self._jammer_toggle_pending = True

        def worker():
            ok = self._send_jammer_command(command, auto_mode=True)

            def finish():
                self._jammer_toggle_pending = False
                if ok:
                    self._jammer_auto_on = desired

            self.after(0, finish)

        threading.Thread(target=worker, daemon=True).start()

    def _jammer_on(self):
        self._send_jammer_command("jammer on")

    def _jammer_off(self):
        self._send_jammer_command("jammer off")

    def _send_jammer_command(self, command: str, auto_mode: bool = False):
        if not self._reader:
            return False
        expect = "[JAMMER] ON" if "on" in command else "[JAMMER] OFF"
        ok = self._reader.send_command(command, wait_prefix=expect, wait_timeout=3.0)
        if not ok:
            if not auto_mode:
                messagebox.showwarning(
                    "Serial",
                    f"Jammer nao respondeu a '{command}'.\n"
                    "Verifique se o firmware validacao esta gravado e se a porta COM esta livre.",
                )
            else:
                self.after(
                    0,
                    lambda: self._status_lbl.config(
                        text=f"Auto: falha ao enviar '{command}' (tentando de novo no proximo ciclo)",
                        foreground="orange",
                    ),
                )
            return False
        if not auto_mode:
            self._status_lbl.config(
                text=f"Comando confirmado: {command}",
                foreground="red" if "on" in command else "gray",
            )
        return True

    def _on_jammer_status(self, line: str):
        self.after(0, lambda msg=line: self._update_jammer_status(msg))

    def _update_jammer_status(self, line: str):
        upper = line.upper()
        if upper.startswith("[JAMMER] ON"):
            self._jammer_state.set("Jammer: ON (hop aleatorio)")
            self._jammer_lbl.config(foreground="red")
            self._status_lbl.config(text=line, foreground="red")
        elif upper.startswith("[JAMMER] OFF"):
            self._jammer_state.set("Jammer: OFF (baseline)")
            self._jammer_lbl.config(foreground="gray")
            self._status_lbl.config(text=line, foreground="gray")
        elif upper.startswith("[JAMMER] ERRO"):
            self._jammer_state.set("Jammer: ERRO")
            self._jammer_lbl.config(foreground="orange")
            self._status_lbl.config(text=line, foreground="orange")
        elif line.startswith("[STATUS]"):
            self._jammer_state.set(line.replace("[STATUS]", "Status:").strip())

    def _check_reader_alive(self):
        if not self._timer_running or not self._reader:
            return
        if self._reader.error:
            return
        if not self._reader.is_alive():
            self._handle_serial_error(
                "A leitura serial encerrou inesperadamente.\n"
                "Verifique a porta COM e se o ESP32 esta conectado."
            )
            return

        elapsed = time.time() - self._start_time
        if self._reader.parsed_lines == 0:
            if self._reader.raw_lines > 0:
                self._status_lbl.config(
                    text=(
                        f"Serial ativa ({self._reader.raw_lines} linhas), "
                        "mas nenhum scan CSV com 126 canais reconhecido."
                    ),
                    foreground="orange",
                )
            elif elapsed >= 8:
                self._status_lbl.config(
                    text=(
                        f"Sem dados em {self._selected_port()} apos {int(elapsed)}s. "
                        "Confira a porta COM, feche o Monitor Serial e reinicie a gravacao."
                    ),
                    foreground="orange",
                )

        self.after(2000, self._check_reader_alive)

    def _on_sample(self, values: list):
        self.after(0, lambda v=values: self._append_sample(v))

    def _on_serial_stats(self, nbytes: int, raw_lines: int, parsed_lines: int):
        def update():
            if parsed_lines:
                color = "green"
            elif raw_lines:
                color = "orange"
            else:
                color = "blue"
            self._status_lbl.config(
                text=(
                    f"Serial: {nbytes} bytes | {raw_lines} linhas | "
                    f"{parsed_lines} scans validos | amostras={len(self._samples)}"
                ),
                foreground=color,
            )

        self.after(0, update)

    def _append_sample(self, values: list):
        self._samples.append(values)
        self._live_canvas.update(values[1:])
        self._sample_lbl.config(text=f"  Amostras: {len(self._samples)}")
        self._status_lbl.config(
            text=f"Recebendo dados ({len(self._samples)} scans)...",
            foreground="green",
        )

    def _tick_timer(self):
        if not self._timer_running:
            return
        elapsed = int(time.time() - self._start_time)
        h, m, s = elapsed // 3600, (elapsed % 3600) // 60, elapsed % 60
        self._time_lbl.config(text=f"Tempo: {h:02d}:{m:02d}:{s:02d}")
        self._update_auto_phase_label(elapsed)
        self._apply_auto_jammer(elapsed)
        self.after(1000, self._tick_timer)

    def _save_data(self):
        if not self._samples:
            messagebox.showinfo("Sem dados", "Nenhuma amostra foi coletada.")
            return
        os.makedirs(SAVED_DATA_DIR, exist_ok=True)
        fname = self._fname_var.get().strip() or "recording"
        path = os.path.join(SAVED_DATA_DIR, f"{fname}.csv")
        if os.path.exists(path):
            base, ext = os.path.splitext(path)
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = f"{base}_{stamp}{ext}"

        n_ch = len(self._samples[0]) - 1
        columns = ["timestamp"] + [f"Ch{i+1}" for i in range(n_ch)]
        df = pd.DataFrame(self._samples, columns=columns[: len(self._samples[0])])
        df.to_csv(path, index=False)
        messagebox.showinfo("Salvo", f"Dados gravados em:\n{path}")


# ---------------------------------------------------------------------------
# View tab
# ---------------------------------------------------------------------------
class ViewTab(ttk.Frame):
    def __init__(self, parent):
        super().__init__(parent)
        self._current_fig: Figure | None = None
        self._current_canvas: FigureCanvasTkAgg | None = None
        self._current_toolbar = None
        self._loaded_df: pd.DataFrame | None = None
        self._loaded_path: str = ""

        self._build_controls()
        self._build_plot_area()
        self._refresh_file_list()

    def _build_controls(self):
        ctrl = ttk.LabelFrame(self, text="Dados gravados", padding=8)
        ctrl.pack(fill=tk.X, padx=10, pady=(10, 4))

        file_row = ttk.Frame(ctrl)
        file_row.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(file_row, text="Arquivo:").pack(side=tk.LEFT)
        self._file_cb = ttk.Combobox(file_row, state="readonly", width=50)
        self._file_cb.pack(side=tk.LEFT, padx=(4, 8))
        ttk.Button(file_row, text="Atualizar", command=self._refresh_file_list).pack(
            side=tk.LEFT, padx=(0, 4)
        )
        ttk.Button(file_row, text="Procurar...", command=self._browse_file).pack(side=tk.LEFT)

        action_row = ttk.Frame(ctrl)
        action_row.pack(fill=tk.X)
        ttk.Button(action_row, text="Abrir e plotar", command=self._load_and_plot).pack(
            side=tk.LEFT, padx=(0, 8)
        )
        self._save_img_btn = ttk.Button(
            action_row, text="Salvar imagem...", command=self._save_image, state=tk.DISABLED
        )
        self._save_img_btn.pack(side=tk.LEFT, padx=(0, 20))
        self._info_lbl = ttk.Label(action_row, text="", foreground="gray")
        self._info_lbl.pack(side=tk.LEFT)

    def _build_plot_area(self):
        self._plot_frame = ttk.LabelFrame(self, text="Heatmap do espectro", padding=4)
        self._plot_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=(4, 10))

    def _refresh_file_list(self):
        os.makedirs(SAVED_DATA_DIR, exist_ok=True)
        files = sorted(glob.glob(os.path.join(SAVED_DATA_DIR, "*.csv")))
        display = [os.path.basename(f) for f in files]
        self._file_cb["values"] = display
        self._file_cb._full_paths = files
        if display:
            self._file_cb.current(len(display) - 1)

    def _browse_file(self):
        path = filedialog.askopenfilename(
            title="Selecionar CSV do espectro",
            initialdir=SAVED_DATA_DIR,
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        )
        if path:
            self._load_file(path)

    def _load_and_plot(self):
        idx = self._file_cb.current()
        paths = getattr(self._file_cb, "_full_paths", [])
        if idx < 0 or idx >= len(paths):
            messagebox.showwarning("Sem arquivo", "Selecione um arquivo da lista.")
            return
        self._load_file(paths[idx])

    def _load_file(self, path: str):
        try:
            df = pd.read_csv(path)
        except Exception as e:
            messagebox.showerror("Erro", f"Nao foi possivel ler o arquivo:\n{e}")
            return

        self._loaded_df = df
        self._loaded_path = path
        n_rec = len(df)
        n_ch = len([c for c in df.columns if c.lower().startswith("ch")])
        fname = os.path.basename(path)
        self._info_lbl.config(text=f"{fname}  |  {n_rec} registros  |  {n_ch} canais")
        self._plot_heatmap(df, title=fname)
        self._save_img_btn.config(state=tk.NORMAL)

    def _clear_plot(self):
        if self._current_toolbar:
            self._current_toolbar.destroy()
        if self._current_canvas:
            self._current_canvas.get_tk_widget().destroy()
        self._current_canvas = None
        self._current_toolbar = None
        self._current_fig = None

    def _plot_heatmap(self, df: pd.DataFrame, title: str = ""):
        self._clear_plot()

        ch_cols = [c for c in df.columns if re.match(r"(?i)^ch\d+$", c)]
        if not ch_cols:
            messagebox.showerror(
                "Formato invalido",
                "Colunas de canal nao encontradas (esperado Ch1, Ch2, ...).",
            )
            return

        channel_data = df[ch_cols].values
        ts_col = next(
            (c for c in df.columns if c.lower() == "timestamp"), df.columns[0]
        )
        timestamps = df[ts_col].values
        t_sec = timestamps.astype(float)
        t_sec = t_sec - t_sec[0]

        fig = Figure(figsize=(10, 5), tight_layout=True)
        ax = fig.add_subplot(111)

        im = ax.imshow(
            channel_data.T,
            aspect="auto",
            cmap=SPECTRUM_CMAP,
            interpolation="none",
            vmin=0,
            vmax=100,
            origin="lower",
        )

        draw_spectrum_markers(ax, vertical=False, show_labels=True)

        ax.set_title(title, fontsize=12, fontweight="bold")
        ax.set_xlabel("Tempo (s)", fontsize=11)
        ax.set_ylabel("Canal NRF24  (2400 + ch MHz)", fontsize=11)

        n_ticks = min(8, len(t_sec))
        idx = np.linspace(0, len(t_sec) - 1, n_ticks, dtype=int)
        ax.set_xticks(idx)
        ax.set_xticklabels([f"{t_sec[i]:.1f}" for i in idx], rotation=30, ha="right")

        y_step = max(1, len(ch_cols) // 10)
        y_ticks = list(range(0, len(ch_cols), y_step))
        ax.set_yticks(y_ticks)
        ax.set_yticklabels([f"{t} ({2400 + t} MHz)" for t in y_ticks], fontsize=8)

        cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.02)
        cbar.set_label("Atividade (%)", fontsize=10)

        self._current_fig = fig
        self._current_canvas = FigureCanvasTkAgg(fig, self._plot_frame)
        self._current_toolbar = NavigationToolbar2Tk(
            self._current_canvas, self._plot_frame
        )
        self._current_toolbar.update()
        self._current_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        self._current_canvas.draw()

    def _save_image(self):
        if self._current_fig is None:
            return
        default_name = os.path.splitext(os.path.basename(self._loaded_path))[0] + ".png"
        path = filedialog.asksaveasfilename(
            title="Salvar imagem",
            initialdir=SAVED_DATA_DIR,
            initialfile=default_name,
            defaultextension=".png",
            filetypes=[
                ("PNG image", "*.png"),
                ("JPEG image", "*.jpg"),
                ("PDF document", "*.pdf"),
                ("All files", "*.*"),
            ],
        )
        if path:
            self._current_fig.savefig(path, dpi=150, bbox_inches="tight")
            messagebox.showinfo("Salvo", f"Imagem salva em:\n{path}")


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TCC — Analisador de Espectro RF")
        self.geometry("1000x680")
        self.resizable(True, True)

        notebook = ttk.Notebook(self)
        notebook.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        self._record_tab = RecordTab(notebook)
        self._view_tab = ViewTab(notebook)

        notebook.add(self._record_tab, text="  Gravar  ")
        notebook.add(self._view_tab, text="  Visualizar  ")

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _on_close(self):
        reader = getattr(self._record_tab, "_reader", None)
        if reader:
            reader.stop()
        self.destroy()


if __name__ == "__main__":
    app = App()
    app.mainloop()
