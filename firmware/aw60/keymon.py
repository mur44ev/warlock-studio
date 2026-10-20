"""Live key monitor (AW60 rev6 EC command). Read-only; use it right after flashing to confirm
that the reactive-effect hook sees the real keys: press a key and its name must appear.

    py -3.12 firmware/aw60/keymon.py --seconds 20
"""
import argparse
import re
import time
from pathlib import Path

from client import AW60, raw_get

ROOT = Path(__file__).resolve().parents[2]
NAMES = re.findall(r'\{"([^"]+)",\s*\d+\}', (ROOT / 'WarlockLayout.h').read_text())


def read(d):
    for _ in range(20):
        d.send(b'\xec')
        p = raw_get(d.h, d.dev)
        if p[:4] == b'\xecKEY':
            return p
        time.sleep(.005)
    raise RuntimeError('EC reply missing; flash AW60 rev6 first')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=20)
    a = ap.parse_args()
    d = AW60()
    try:
        end, last = time.monotonic() + a.seconds, None
        while time.monotonic() < end:
            p = read(d)
            held = [NAMES[k] for k in range(80) if p[24 + k // 8] & (1 << (k % 8))]
            line = 'active=%d tracking=%d presses=%d raw=%s held=%s' % (
                p[6], p[5], int.from_bytes(p[34:38], 'little'), p[8:23].hex(), ','.join(held) or '-')
            if line != last:
                print(line, flush=True)
                last = line
            time.sleep(.02)
    finally:
        d.close()


if __name__ == '__main__':
    main()
