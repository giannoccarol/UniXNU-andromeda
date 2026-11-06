#!/usr/bin/env python3
"""Readable status of the andromeda drivers from the live USB kernel log.

  scripts/andstatus.py LOGFILE            summary of what the log shows so far
  scripts/andstatus.py LOGFILE --follow   refresh every 2 s while the log grows

LOGFILE is what scripts/udplog-ts.py writes (phone/logs/*.log). Read only:
the phone receives nothing; it only parses the lines the drivers already
print (ANDROMEDA CPU/GPU/BT/BL/PROBE, SWEET TOUCH/HID/FB/BATTERY, ...).
"""
import re
import sys
import time
from pathlib import Path

# (section, label, regex): the last match wins and is shown.
RULES = [
    ("Avvio", "root montata", r"hfs: mounted (\S+) on device"),
    ("Avvio", "SpringBoard", r"(exec SpringBoard)"),
    ("Avvio", "schermo", r"SWEET FB: (display (?:on|off))"),
    ("Avvio", "display fit", r"SWEET FB: fit (\S+ -> \S+)"),
    ("Touch", "chip", r"SWEET TOUCH \(cepheus\): probe 0x49 err=(\S+) chip-id=(\S+)"),
    ("Touch", "scansione", r"SWEET TOUCH \(cepheus\): scan on \(([^)]*)\) settings=(\S+) err=(\S+)"),
    ("Touch", "letture", r"SWEET TOUCH \(cepheus\): polls=(\d+) nonempty=(\d+)"),
    ("Touch", "esperimento", r"SWEET TOUCH EXP: step (\d+) ended, finger-enter events in it: (\d+)"),
    ("Touch", "ultimo tocco HID", r"SWEET HID: touch (\d+ (?:down|up) at \S+)"),
    ("CPU", "silver", r"ANDROMEDA CPU: gov silver[^:]*: (.*)"),
    ("CPU", "gold", r"ANDROMEDA CPU: gov gold[^:]*: (.*)"),
    ("CPU", "prime", r"ANDROMEDA CPU: gov prime[^:]*: (.*)"),
    ("CPU", "frequenza misurata", r"ANDROMEDA CPU: (\S+ \(\S+\)) \((?:full|cool|boot)\): desired=\d+ rate_khz=(\d+)"),
    ("CPU", "temperatura", r"ANDROMEDA CPU: (?:temperature )?(?:max )?(-?\d+) mC"),
    ("Batteria", "iOS", r"SWEET BATTERY: ([^\n]*)"),
    ("Batteria", "PM8150B", r"ANDROMEDA PROBE: battery (.*)"),
    ("GPU", "gradino", r"ANDROMEDA GPU: (step \d+ \([^)]*\) \S+)"),
    ("GPU", "copia a schermo", r"ANDROMEDA GPU: (present: (?:first \d+ GPU frames checked.*|DISABLED.*|\d+ GPU frames.*)|present not used: .*)"),
    ("GPU", "firmware", r"ANDROMEDA GPU: probe: firmware (\S+ \S+)"),
    ("Fotocamera", "C1", r"ANDROMEDA CAM: (C1 PASS.*|C1 FAIL.*|imx363 reg .*)"),
    ("Wi-Fi", "modem", r"ANDROMEDA WLAN: (modem READY.*|W0 done.*|.*FAIL.*|PAS .*)"),
    ("Bluetooth", "esito", r"ANDROMEDA BT: (B1 PASS|B2 PASS.*|.*FAIL.*|down: .*)"),
    ("Bluetooth", "versione", r"ANDROMEDA BT: (SoC version: .*)"),
    ("Luminosità", "esito", r"ANDROMEDA BL: (level \d+ .*|DCS 51 .*)"),
    ("Consumo", "statistiche", r"ANDROMEDA PWR: (t=.*)"),
    ("Slider", "posizione", r"ANDROMEDA SLIDER: (hall42=\d hall114=\d -> .*)"),
    ("Slider", "per iOS", r"ANDROMEDA SLIDERCAM: (.*)"),
    ("Slider", "andromedad", r"andromedad: (.*)"),
    ("Vibrazione", "prova", r"ANDROMEDA HAP: (restored: .*|pulse \d: on .*|.*nothing written.*|WARNING: .*)"),
    ("Vibrazione", "registri", r"ANDROMEDA PROBE: (PM8150B HAPTICS sid3 .*)"),
    ("Sistema", "AMFI", r"SWEET AMFI: (.*)"),
    ("Sistema", "fastboot", r"ANDROMEDA PROBE: (REBOOT .*|will reboot into fastboot .*)"),
    ("Sistema", "panic", r"(panic\(.*)"),
]
COMPILED = [(sec, label, re.compile(rx)) for sec, label, rx in RULES]
STAMP = re.compile(r"^(\d\d:\d\d:\d\d) \+\s*([\d.]+)s ")


def scan(lines, state):
    for line in lines:
        stamp = STAMP.match(line)
        when = stamp.group(1) if stamp else ""
        for sec, label, rx in COMPILED:
            m = rx.search(line)
            if m:
                state[(sec, label)] = (when, " ".join(g for g in m.groups() if g))


def render(state, path):
    out = [f"andromeda — {path}  ({time.strftime('%H:%M:%S')})"]
    section = None
    for sec, label, _ in COMPILED:
        if (sec, label) not in state:
            continue
        if sec != section:
            out.append(f"\n[{sec}]")
            section = sec
        when, value = state[(sec, label)]
        out.append(f"  {label:<20} {value}   ({when})")
    if len(out) == 1:
        out.append("  (nessuna riga dei driver nel log, per ora)")
    return "\n".join(out)


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        sys.exit(__doc__)
    path = Path(sys.argv[1])
    follow = "--follow" in sys.argv[2:]
    state, offset = {}, 0
    while True:
        with path.open("rb") as f:
            f.seek(offset)
            chunk = f.read()
            offset += len(chunk)
        scan(chunk.decode("utf-8", "replace").splitlines(), state)
        if not follow:
            print(render(state, path))
            return
        print("\033[2J\033[H" + render(state, path), flush=True)
        time.sleep(2)


if __name__ == "__main__":
    main()
