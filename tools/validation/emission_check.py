#!/usr/bin/env python3
"""
jammer-emission-check: o jammer (placa_b) irradia energia RF observável?
Compara o espectro do analisador (placa_a, logs/spectrum.csv) numa faixa
alta "limpa" (TEST_CHANNELS, fora dos canais de pareamento/hopping) com o
jammer DESLIGADO vs LIGADO em 'targeted'+'power max' -- se atividade
aparecer nos canais de teste só quando o jammer está ligado, é evidência
de emissão fora da banda de operação normal (não é só efeito do link
mudando de canal).

Este script NÃO abre nenhuma porta serial -- só escreve comandos em
logs/cmd.in (mesmo mecanismo de run_scenario.py/run_battery.py) e lê
logs/state.json / logs/latest.jsonl / logs/spectrum.csv.
tools/logger/main.py precisa estar rodando (dono exclusivo das portas)
antes de chamar isto.

Uso:
    python3 tools/logger/main.py &         # se ainda não estiver rodando
    python3 tools/validation/emission_check.py run --phase-s 120 --settle-s 10
    python3 tools/validation/emission_check.py analyze logs/emission-check-<ts>.json

O subcomando 'run' só orquestra o experimento e salva as janelas de tempo
(epoch) em logs/emission-check-<session>.json; a análise (leitura de
logs/spectrum.csv) roda automaticamente ao final e também pode ser
reexecutada depois, isoladamente, com 'analyze'.

Suposições sobre logs/spectrum.csv (verificado com head/tail em
2026-09-25, não há linha de cabeçalho):
    epoch,placa_a,ch0,ch1,...,ch125
  epoch é float (segundos, época Unix), a 2ª coluna é sempre o nome da
  placa que fez a varredura (só 'placa_a' observado -- o analisador vive
  nela), e as 126 colunas seguintes são atividade 0-100 por canal
  (HOPPING_CHANNELS + banda alta), ~1 linha/segundo (varia, não é
  estritamente periódico).
"""
import argparse
import csv
import json
import os
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(__file__))
from run_scenario import (  # noqa: E402
    LATEST_PATH,
    LOGS_DIR,
    current_session_path,
    read_state,
    require_logger_running,
    send_cmd,
)

SPECTRUM_PATH = os.path.join(LOGS_DIR, "spectrum.csv")

MESTRE_BOARD = "placa_a"
ESCRAVO_BOARD = "placa_b"  # mesma placa do jammer

# Canais de banda alta "limpos" usados como sonda -- fora de
# HOPPING_CHANNELS e fora dos candidatos de pareamento, então qualquer
# atividade ali com o link travado no canal 10 (mode fixed) só pode vir de
# ruído ambiente ou do próprio jammer.
TEST_CHANNELS = [94, 100, 105, 110, 115, 120, 125]
# Canais candidatos a pareamento -- fora do escopo da sonda, mas
# documentados aqui pra quem for escolher outra faixa de teste no futuro.
PAIRING_CANDIDATES = {76, 90, 96, 101, 108, 114, 119, 124}

# msg_done vem de "mestre" ou "escravo" (ver mestre/src/node.h,
# escravo/src/node.h) -- qualquer um dos dois conta como "link com
# tráfego de verdade".
LINK_TRAFFIC_EVENTS = {"msg_done"}


def load_events(session_path, t_start, t_end, pad=5.0):
    events = []
    try:
        f = open(session_path)
    except FileNotFoundError:
        print(f"[emission] AVISO: session file {session_path} não existe mais.", file=sys.stderr)
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


def link_is_paired(window_s=15.0):
    """Enlace pareado e com tráfego recente: msg_done (mestre OU escravo)
    nos últimos `window_s` segundos, lendo direto do latest.jsonl -- mais
    barato que abrir o arquivo inteiro pra checar só isto."""
    try:
        path = current_session_path()
    except OSError:
        return False
    now = time.time()
    found = False
    try:
        with open(path) as f:
            # arquivo pode ser grande; olhamos só a cauda.
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 200_000))
            if f.tell() != 0:
                f.readline()  # descarta linha parcial
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if d.get("event") not in LINK_TRAFFIC_EVENTS:
                    continue
                ep = d.get("epoch")
                if ep is not None and now - ep < window_s:
                    found = True
    except FileNotFoundError:
        return False
    return found


def wait_for_paired_link(timeout_s=20.0, poll_s=1.0):
    print(f"[emission] Verificando enlace pareado (msg_done recente, timeout {timeout_s:.0f}s)...")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if link_is_paired():
            print("[emission] Enlace pareado e com tráfego -- ok.")
            return True
        time.sleep(poll_s)
    return False


def cmd_run(args):
    require_logger_running()
    session_path = current_session_path()
    print(f"[emission] Sessão ativa: {session_path}")

    if not wait_for_paired_link(timeout_s=args.pair_timeout_s):
        print(
            "[emission] ERRO: enlace não está pareado/com tráfego (sem "
            "msg_done recente) -- confirme que as placas estão ligadas, "
            "pareadas e trocando dados antes de rodar o teste de emissão.",
            file=sys.stderr,
        )
        sys.exit(1)

    print("[emission] Travando o link em modo fixo (canal 10, fora da banda de teste)...")
    send_cmd(MESTRE_BOARD, "mode fixed")
    send_cmd(ESCRAVO_BOARD, "mode fixed")
    time.sleep(args.settle_s)

    if not link_is_paired():
        print(
            "[emission] ERRO: enlace caiu (sem msg_done recente) depois de "
            "'mode fixed' -- abortando antes de tirar qualquer conclusão "
            "sobre emissão do jammer.",
            file=sys.stderr,
        )
        sys.exit(1)
    print("[emission] Link confirmado vivo em modo fixo.")

    windows = {"session_file": session_path}

    print(f"[emission] Fase OFF: jammer desligado, coletando {args.phase_s:.0f}s...")
    send_cmd(ESCRAVO_BOARD, "jam off")
    time.sleep(args.settle_s)
    off_start = time.time()
    time.sleep(args.phase_s)
    off_end = time.time()
    windows["off"] = {"start_epoch": off_start, "end_epoch": off_end}

    print(f"[emission] Fase ON: jammer 'targeted'/'power max', coletando {args.phase_s:.0f}s...")
    send_cmd(ESCRAVO_BOARD, "mode targeted")
    time.sleep(0.2)
    send_cmd(ESCRAVO_BOARD, "power max")
    time.sleep(0.2)
    send_cmd(ESCRAVO_BOARD, "jam on")

    jam_confirm_deadline = time.time() + max(args.settle_s, 5.0)
    saw_jam_on = False
    while time.time() < jam_confirm_deadline:
        evs = load_events(session_path, time.time() - 5.0, time.time(), pad=0.0)
        if any(e.get("event") == "jam_state_on" for e in evs):
            saw_jam_on = True
            break
        time.sleep(0.5)
    if not saw_jam_on:
        print(
            "[emission] AVISO: nenhum evento 'jam_state_on' visto após "
            "'jam on' -- o jammer pode não ter de fato ligado (bug de "
            "firmware/serial?). Prosseguindo mesmo assim, mas desconfie "
            "do resultado.",
            file=sys.stderr,
        )
    else:
        print("[emission] jam_state_on confirmado.")

    time.sleep(args.settle_s)
    on_start = time.time()
    time.sleep(args.phase_s)
    on_end = time.time()
    windows["on"] = {"start_epoch": on_start, "end_epoch": on_end}

    print("[emission] Restaurando estado normal da bancada...")
    send_cmd(ESCRAVO_BOARD, "jam off")
    send_cmd(ESCRAVO_BOARD, "power low")
    send_cmd(MESTRE_BOARD, "mode hopping")
    send_cmd(ESCRAVO_BOARD, "mode hopping")

    ts = datetime.fromtimestamp(off_start, tz=timezone.utc).strftime("%Y%m%d-%H%M%S")
    session_tag = os.path.splitext(os.path.basename(session_path))[0]
    out_path = os.path.join(LOGS_DIR, f"emission-check-{ts}-{session_tag}.json")
    windows["loss_of_sync_on"] = [
        e["epoch"]
        for e in load_events(session_path, on_start, on_end, pad=0.0)
        if e.get("event") == "loss_of_sync"
    ]
    windows["loss_of_sync_off"] = [
        e["epoch"]
        for e in load_events(session_path, off_start, off_end, pad=0.0)
        if e.get("event") == "loss_of_sync"
    ]
    with open(out_path, "w") as f:
        json.dump(windows, f, indent=2)
    print(f"[emission] Janelas salvas em {out_path}")

    analyze(out_path)


def read_spectrum_rows(board_filter=MESTRE_BOARD):
    """Lê logs/spectrum.csv linha a linha (sem cabeçalho): epoch, placa,
    ch0..ch125. Filtra pela placa do analisador (default placa_a) --
    verificado com head/tail que só ela aparece na bancada atual, mas o
    filtro é mantido explícito caso isso mude."""
    rows = []
    with open(SPECTRUM_PATH, newline="") as f:
        reader = csv.reader(f)
        for parts in reader:
            if len(parts) < 3:
                continue
            try:
                epoch = float(parts[0])
            except ValueError:
                continue  # linha de cabeçalho ou corrompida -- pula
            board = parts[1]
            if board_filter is not None and board != board_filter:
                continue
            try:
                channels = [float(x) for x in parts[2:]]
            except ValueError:
                continue
            rows.append((epoch, channels))
    return rows


def slice_window(rows, start_epoch, end_epoch):
    return [ch for ep, ch in rows if start_epoch <= ep <= end_epoch]


def channel_stats(window_rows, channel):
    vals = [row[channel] for row in window_rows if channel < len(row)]
    if not vals:
        return {"mean": None, "frac_active": None, "n": 0}
    n_active = sum(1 for v in vals if v > 0)
    return {
        "mean": sum(vals) / len(vals),
        "frac_active": n_active / len(vals),
        "n": len(vals),
    }


def combined_any_active_fraction(window_rows, channels):
    if not window_rows:
        return None, 0
    n_any = sum(1 for row in window_rows if any(row[c] > 0 for c in channels if c < len(row)))
    return n_any / len(window_rows), len(window_rows)


def analyze(windows_path):
    with open(windows_path) as f:
        windows = json.load(f)

    off = windows["off"]
    on = windows["on"]

    rows = read_spectrum_rows()
    if not rows:
        print(
            f"[emission] ERRO: nenhuma linha lida de {SPECTRUM_PATH} -- "
            "arquivo vazio, ausente, ou filtro de placa não bate com o "
            "conteúdo real (ver cabeçalho do módulo).",
            file=sys.stderr,
        )
        sys.exit(1)

    off_rows = slice_window(rows, off["start_epoch"], off["end_epoch"])
    on_rows = slice_window(rows, on["start_epoch"], on["end_epoch"])

    print("=" * 78)
    print("RESUMO: emissão RF do jammer (placa_b) -- OFF vs ON, banda de teste")
    print("=" * 78)
    print(f"Janela OFF: {off['end_epoch'] - off['start_epoch']:.0f}s ({len(off_rows)} varreduras)")
    print(f"Janela ON:  {on['end_epoch'] - on['start_epoch']:.0f}s ({len(on_rows)} varreduras)")
    print()

    print(f"{'canal':>6} {'OFF média':>10} {'ON média':>10} {'OFF frac>0':>11} {'ON frac>0':>10}")
    any_ch_on_much_higher = False
    for ch in TEST_CHANNELS:
        off_s = channel_stats(off_rows, ch)
        on_s = channel_stats(on_rows, ch)
        off_mean = off_s["mean"] if off_s["mean"] is not None else float("nan")
        on_mean = on_s["mean"] if on_s["mean"] is not None else float("nan")
        off_frac = off_s["frac_active"] if off_s["frac_active"] is not None else float("nan")
        on_frac = on_s["frac_active"] if on_s["frac_active"] is not None else float("nan")
        print(f"{ch:>6} {off_mean:>10.2f} {on_mean:>10.2f} {off_frac:>11.3f} {on_frac:>10.3f}")
        if on_s["mean"] is not None and off_s["mean"] is not None and on_s["mean"] > off_s["mean"] + 5:
            any_ch_on_much_higher = True
    print()

    off_combined, off_n = combined_any_active_fraction(off_rows, TEST_CHANNELS)
    on_combined, on_n = combined_any_active_fraction(on_rows, TEST_CHANNELS)
    print(
        f"Combinado (qualquer um dos {len(TEST_CHANNELS)} canais de teste ativo): "
        f"OFF={off_combined:.3f} (n={off_n})  ON={on_combined:.3f} (n={on_n})"
    )
    print()
    print(
        "Nota: em modo 'targeted' o jammer permanece ~60ms por canal, "
        "revezando 16 canais (HOPPING_CHANNELS) -- um ciclo completo leva "
        "~1s. O analisador varre e reporta cada canal ~1x por varredura, "
        "então um canal de teste que de fato emite tende a aparecer ativo "
        "em só ~1/16 (~6%) das varreduras (~2/16, ~12%, se a placa B tiver "
        "um segundo rádio jammer concorrente). Frações ON baixas em termos "
        "absolutos (poucos %) ainda contam como evidência, desde que "
        "claramente acima do OFF."
    )
    print()

    loss_on = windows.get("loss_of_sync_on", [])
    loss_off = windows.get("loss_of_sync_off", [])
    print(f"loss_of_sync durante ON: {len(loss_on)}   durante OFF: {len(loss_off)}")
    print()

    # Critério RELATIVO (não um piso absoluto de OFF): a banda alta tem
    # ruído ambiente esporádico (hits isolados de ~10 mesmo com jammer OFF),
    # então exigir OFF≈0 daria falso "não irradia". Em vez disso, ON precisa
    # superar OFF por uma margem clara -- em duty cycle E em fração combinada
    # -- ou o enlace co-locado precisa cair só no ON.
    radiates = False
    reasons = []
    if off_combined is not None and on_combined is not None:
        # margem: ON >= 3x OFF e pelo menos +0.03 acima do OFF (cobre o caso
        # OFF≈0, onde 3x não morde), com corroboração por-canal.
        margin_ok = on_combined >= max(0.03, 3.0 * off_combined + 0.02)
        if margin_ok and any_ch_on_much_higher:
            radiates = True
            reasons.append(
                f"fração combinada ON={on_combined:.3f} supera OFF={off_combined:.3f} "
                "por margem clara (>=3x e >=+0.03), com >=1 canal de teste de "
                "média ON notavelmente acima do OFF"
            )
        elif margin_ok and not any_ch_on_much_higher:
            reasons.append(
                f"AMBÍGUO: fração combinada ON={on_combined:.3f} > OFF={off_combined:.3f}, "
                "mas nenhum canal individual com média ON claramente acima do OFF "
                "-- inspecionar a tabela por-canal manualmente antes de concluir"
            )
    if loss_on and not loss_off:
        radiates = True
        reasons.append(f"{len(loss_on)} loss_of_sync durante ON, nenhum durante OFF (enlace caiu perto do jammer)")

    print("=" * 78)
    if radiates:
        print("VEREDITO: RADIATES -- evidência de emissão RF observável do jammer.")
        for r in reasons:
            print(f"  - {r}")
    else:
        print("VEREDITO: ABSENT/WEAK -- sem evidência clara de emissão na banda de teste.")
        print(f"  (OFF combinado={off_combined}, ON combinado={on_combined}, loss_of_sync ON={len(loss_on)})")
    print("=" * 78)


def cmd_analyze(args):
    analyze(args.windows_path)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="subcommand", required=True)

    run_p = sub.add_parser("run", help="roda o experimento OFF/ON em bancada e analisa ao final")
    run_p.add_argument(
        "--phase-s", type=float, default=120.0,
        help="segundos de coleta por fase (OFF e ON), default 120",
    )
    run_p.add_argument(
        "--settle-s", type=float, default=10.0,
        help="segundos de acomodação após cada comando antes de começar a "
        "coletar (troca de modo, jam on/off), default 10",
    )
    run_p.add_argument(
        "--pair-timeout-s", type=float, default=20.0,
        help="segundos esperando o link parear/ter tráfego antes de abortar, default 20",
    )
    run_p.set_defaults(func=cmd_run)

    analyze_p = sub.add_parser("analyze", help="reanalisa um logs/emission-check-*.json já salvo")
    analyze_p.add_argument("windows_path", help="caminho do logs/emission-check-<ts>.json")
    analyze_p.set_defaults(func=cmd_analyze)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
