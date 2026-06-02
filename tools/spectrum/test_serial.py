"""Leitura rapida da serial para diagnostico. Uso: python test_serial.py COM4"""
import sys
import time
import serial

from main import parse_spectrum_line, N_CHANNELS

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 2
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.3)
print(f"Abrindo {port}, aguardando linhas por 30 s...\n")

deadline = time.time() + 30
raw_count = 0
parsed_count = 0

while time.time() < deadline:
    line = ser.readline().decode("utf-8", errors="ignore").strip()
    if not line:
        continue
    raw_count += 1
    parts = line.split(",")
    parsed = parse_spectrum_line(line)
    if parsed:
        parsed_count += 1
        print(f"[OK #{parsed_count}] {len(parts)} campos, max={max(parsed)}")
    else:
        print(f"[-- #{raw_count}] {len(parts)} campos | {line[:100]!r}")

ser.close()
print(f"\nTotal: {raw_count} linhas brutas, {parsed_count} scans validos (126 canais)")
