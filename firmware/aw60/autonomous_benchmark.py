"""Send one EB configuration, then measure autonomous MCU rendering without traffic."""
import argparse, json, time
from pathlib import Path
from client import AW60, raw_get
from radio import Radio

def status(d):
    if isinstance(d, Radio):
        p = d.query(b'\xe9')
    else:
        d.send(b'\xe9')
        p = raw_get(d.h, d.dev)
    if p[:6] != b'\xe9AW60\x01' or p[23] < 4:
        raise RuntimeError('Autonomous AW60 rev4+ missing: ' + p.hex())
    return dict(active=p[6], frames=int.from_bytes(p[8:12], 'little'),
                errors=int.from_bytes(p[12:14], 'little'),
                stock_tick=int.from_bytes(p[24:28], 'little'), revision=p[23])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--radio', action='store_true')
    ap.add_argument('--seconds', type=float, default=10)
    a = ap.parse_args()
    d = Radio() if a.radio else AW60()
    try:
        status(d)
        p = bytearray(64)
        p[:7] = bytes([0xeb, 0, 35, 180, 60, 0, 170])
        p[8:11] = bytes([255, 1, 0])
        d.send(p)
        time.sleep(.1)
        before = status(d)
        start = time.perf_counter()
        time.sleep(a.seconds)  # Absolutely no HID calls during this interval.
        after = status(d)
        elapsed = time.perf_counter() - start
        accepted = (after['frames'] - before['frames']) & 0xffffffff
        result = dict(radio=a.radio, effect_commands=1, seconds=elapsed,
                      rendered=accepted, measured_fps=accepted/elapsed,
                      before=before, after=after)
        print(json.dumps(result), flush=True)
        Path(__file__).with_name('autonomous-'+time.strftime('%Y%m%d-%H%M%S')+'.json').write_text(json.dumps(result, indent=2))
        if after['active'] != 2 or after['errors'] != before['errors'] or not 57 <= accepted/elapsed <= 63:
            raise RuntimeError('Autonomous rate or status failed validation')
    finally:
        d.close()  # Keep animation running after the host closes its handle.

if __name__ == '__main__':
    main()
