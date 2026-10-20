"""Autonomous post-flash health check for the wired ARDOR Warlock (read-mostly; one temporary effect).

Checks: identity/firmware, every persistent setting against the last pre-flash snapshot, AW60 status,
idle key matrix, a 60 FPS autonomous soak, error counters, and (rev8) the performance report.

    py -3.12 firmware/aw60/verify_device.py [--soak 20] [--baseline custom-....json]
"""
import argparse
import json
import sys
import time
from pathlib import Path

from client import AW60, raw_get
from radio import Radio


def read(d):
    return d.read() if isinstance(d, Radio) else raw_get(d.h, d.dev)

ROOT = Path(__file__).resolve().parent
SETTINGS = (0x85, 0x86, 0x91, 0x92, 0x97)     # profile, options, debounce, sleep, misc (0x87 follows the live effect)
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(('PASS ' if ok else 'FAIL ') + name + (' - ' + str(detail) if detail else ''), flush=True)
    return ok


def get(d, payload, wait=.15):
    if isinstance(d, Radio):
        return d.query(bytes(payload))
    d.send(bytes(payload))
    time.sleep(wait)
    return read(d)


def u32(p, off):
    return int.from_bytes(p[off:off + 4], 'little')


def baseline_settings(path):
    if path:
        records = [Path(path)]
    else:
        records = sorted(ROOT.glob('custom-*.json'), reverse=True)
    for record in records:
        data = json.loads(record.read_text(encoding='utf8'))
        for event in data.get('events', []):
            if event.get('phase') == 'settings-saved':
                return record.name, event['settings']
    raise SystemExit('no pre-flash settings snapshot found')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--soak', type=float, default=20)
    ap.add_argument('--baseline')
    ap.add_argument('--radio', action='store_true')
    a = ap.parse_args()
    name, base = baseline_settings(a.baseline)
    print('baseline:', name)
    d = Radio() if a.radio else AW60()
    try:
        ident = get(d, [0x8f])
        check('identity model 919', ident[:1] == b'\x8f' and int.from_bytes(ident[1:3], 'little') == 919, ident[:8].hex())
        rev = get(d, [0x80])
        check('stock firmware revision 1.04', rev[:3] == bytes.fromhex('800401'), rev[:4].hex())
        for command in SETTINGS:
            now = get(d, [command]).hex()
            was = base[hex(command)]
            check('setting 0x%02x unchanged' % command, now[:16] == was[:16], 'now %s was %s' % (now[:16], was[:16]))
        profile = get(d, [0x85])[1]
        for command, pages in ((0x89, 8), (0x90, 8)):
            now = [get(d, [command, profile, page]).hex() for page in range(pages)]
            was = base[hex(command)]
            # The last 8 bytes of a layer are firmware-owned (see Studio README); compare the first 504.
            now_bytes = bytes.fromhex(''.join(p[:128] for p in now))[:504]
            was_bytes = bytes.fromhex(''.join(p[:128] for p in was))[:504]
            check('%s layer unchanged (504 bytes)' % ('base' if command == 0x89 else 'Fn'), now_bytes == was_bytes)
        d.send(b'\xe9')
        status = read(d)
        check('AW60 status signature', status[:6] == b'\xe9AW60\x01', status[:6])
        revision = status[23]
        check('AW60 revision >= 9 (isolated layout staging)', revision >= 9, revision)
        check('key state RAM self-test', status[56] == 1)
        errors_before = int.from_bytes(status[12:14], 'little')
        d.send(b'\xec')
        keys = read(d)
        idle = keys[:4] == b'\xecKEY' and keys[8:23] == bytes([255] * 15)
        check('key matrix idle (all released; press nothing during the test)', idle, keys[8:23].hex())

        if revision >= 8:
            d.send(bytes([0xed, 0, 1]))
            page0 = read(d)
            check('perf page 0 reply', page0[:4] == b'\xedPF\x00', page0[:4].hex())
            print('   hclk select %d, core %d kHz, SysTick reload %d, NVIC prio IRQ12..15 %08x' % (
                u32(page0, 8), u32(page0, 36), u32(page0, 12), u32(page0, 24)))
        # Autonomous soak: one command, then no host traffic while the MCU renders on its own.
        p = bytearray(64)
        p[:7] = bytes([0xeb, 0, 35, 180, 60, 0, 170])
        p[8:11] = bytes([255, 7, 0])
        p[19] = 1
        d.send(bytes(p))
        time.sleep(.3)
        d.send(b'\xe9'); before = read(d)
        if revision >= 8:
            d.send(bytes([0xed, 1, 1])); read(d)       # reset counters
        t0 = time.perf_counter()
        time.sleep(a.soak)
        d.send(b'\xe9'); after = read(d)
        elapsed = time.perf_counter() - t0
        frames = (u32(after, 8) - u32(before, 8)) & 0xffffffff
        check('autonomous effect running', after[6] == 2, after[6])
        check('60 FPS for %.0f s' % a.soak, 57 <= frames / elapsed <= 63, '%.2f FPS' % (frames / elapsed))
        check('no new errors', int.from_bytes(after[12:14], 'little') == errors_before)
        if revision >= 8:
            d.send(bytes([0xed, 1, 0])); page1 = read(d)
            d.send(bytes([0xed, 2, 0])); page2 = read(d)
            cpm = max(1, u32(page0, 36))
            hist = [int.from_bytes(page1[40 + 2 * i:42 + 2 * i], 'little') for i in range(8)]
            print('   hook entries: %d (poll %d + scan %d), longest gap %.3f ms, histogram(<0.1,.25,.5,1,2,5,10,>=10 ms) %s' % (
                u32(page1, 12) + u32(page1, 36), u32(page1, 12), u32(page1, 36), u32(page1, 8) / cpm, hist))
            print('   render: %d frames in %d slices, longest slice %.0f us; converter calls %d, longest %.0f us' % (
                u32(page1, 24), u32(page1, 20), u32(page1, 16) * 1000 / cpm, u32(page1, 32), u32(page1, 28) * 1000 / cpm))
            for index, label in enumerate(('USB', 'timer1', 'timer2 (key scan)', 'timer3')):
                count, longest, total = u32(page2, 12 + 12 * index), u32(page2, 8 + 12 * index), u32(page2, 16 + 12 * index)
                print('   IRQ %-18s %7d calls, longest %5.0f us, CPU load %.2f %%' % (
                    label, count, longest * 1000 / cpm, total * 16 * 100 / (elapsed * 1000 * cpm)))
            check('no gap between rendering opportunities >= 50 ms', u32(page1, 8) / cpm < 50, '%.2f ms' % (u32(page1, 8) / cpm))
        # The rev<=8 bug was persistent corruption AFTER the initial checks.
        # Read both layers again while the effect is active, and repeat stock
        # reads which exercise the staging/commit path before the final read.
        for _ in range(2):
            for command in SETTINGS:
                get(d, [command])
        for command in (0x89, 0x90):
            now = b''.join(get(d, [command, profile, page]) for page in range(8))
            wanted = bytes.fromhex(''.join(base[hex(command)]))
            check('%s layer intact AFTER autonomous rendering and stock reads' % ('base' if command == 0x89 else 'Fn'), now[:504] == wanted[:504])
    finally:
        d.send(b'\xe8\x00')              # leave the keyboard on its normal lighting
        d.close()
    failed = [r for r in results if not r[1]]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
