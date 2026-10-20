"""Write the curated AW60 rev7 lighting preset pack into slots 0..18.

Slots 14..18 are the key-reactive effects (they respond to typing, no host needed).

Backs up every slot before touching anything and verifies readback after writes.
"""
import json
import time
from pathlib import Path

from client import AW60, raw_get

ROOT = Path(__file__).resolve().parent
MAGIC = b'AWP5'
VERSION = 1
SLOT_BYTES = 256
NAME_OFFSET = 24
NAME_BYTES = 32

PRESETS = [
    dict(slot=0, mode=0, name='Rainbow Wave', speed=34, brightness=210, fps=60, scale=54, amount=66, c1='#00aaff', c2='#ff4fd8', c3='#7bffef'),
    dict(slot=1, mode=1, name='Color Wave', speed=40, brightness=205, fps=60, scale=60, amount=74, c1='#ff00aa', c2='#00ffcc', c3='#ffe066'),
    dict(slot=2, mode=2, name='Gradient', speed=30, brightness=215, fps=60, scale=46, amount=62, c1='#5b8cff', c2='#ff66c4', c3='#fff1a8'),
    dict(slot=3, mode=3, name='Breathing', speed=24, brightness=190, fps=60, scale=42, amount=55, c1='#00c8ff', c2='#7928ff', c3='#ff6ab3'),
    dict(slot=4, mode=4, name='Twinkle', speed=36, brightness=210, fps=60, scale=50, amount=84, c1='#2a4bff', c2='#ff4fa6', c3='#ffffff'),
    dict(slot=5, mode=5, name='Scanner', speed=45, brightness=220, fps=60, scale=58, amount=72, c1='#00bfff', c2='#3fffed', c3='#ffffff'),
    dict(slot=6, mode=6, name='Aurora Drift', speed=38, brightness=200, fps=60, scale=58, amount=74, c1='#00aaff', c2='#ff4fd8', c3='#7bffef'),
    dict(slot=7, mode=7, name='Plasma Cloud', speed=44, brightness=205, fps=60, scale=72, amount=82, c1='#7a00ff', c2='#00d0ff', c3='#ff5bd1'),
    dict(slot=8, mode=8, name='Sunset Drift', speed=30, brightness=220, fps=60, scale=52, amount=68, c1='#ff6018', c2='#ffb430', c3='#fff3c8'),
    dict(slot=9, mode=9, name='Ocean Tide', speed=28, brightness=210, fps=60, scale=60, amount=66, c1='#0066ff', c2='#00d8ff', c3='#7bffef'),
    dict(slot=10, mode=10, name='Ember Rain', speed=55, brightness=225, fps=60, scale=46, amount=88, c1='#ff6018', c2='#ffb430', c3='#ffffc8'),
    dict(slot=11, mode=11, name='Comet Trail', speed=58, brightness=220, fps=60, scale=44, amount=82, c1='#14a8ff', c2='#ffffff', c3='#86ffe0'),
    dict(slot=12, mode=12, name='Kaleido Mirror', speed=34, brightness=210, fps=60, scale=66, amount=73, c1='#ff4fd8', c2='#00b4ff', c3='#d6ff4d'),
    dict(slot=13, mode=13, name='Velvet Wave', speed=26, brightness=195, fps=60, scale=64, amount=62, c1='#5022ff', c2='#ff4fd8', c3='#9af7ff'),
    dict(slot=14, mode=14, name='Keypress Glow', speed=40, brightness=230, fps=60, scale=42, amount=50, c1='#0a2a66', c2='#ff4fa6', c3='#ffffff'),
    dict(slot=15, mode=15, name='Ripple Burst', speed=45, brightness=230, fps=60, scale=40, amount=45, c1='#0b1f4a', c2='#00d0ff', c3='#ffffff'),
    dict(slot=16, mode=16, name='Laser Cross', speed=50, brightness=235, fps=60, scale=40, amount=40, c1='#1a0a2e', c2='#ff2e63', c3='#ffe9a8'),
    dict(slot=17, mode=17, name='Heat Trail', speed=40, brightness=225, fps=60, scale=45, amount=45, c1='#0a1430', c2='#ff5a1f', c3='#fff2a0'),
    dict(slot=18, mode=18, name='Fireworks', speed=45, brightness=235, fps=60, scale=50, amount=40, c1='#0a0a1e', c2='#ff4fd8', c3='#7bffef'),
]

def rgb(hex_color):
    hex_color = hex_color.lstrip('#')
    return bytes.fromhex(hex_color)

def packet_status(d):
    d.send(b'\xe9')
    p = raw_get(d.h, d.dev)
    if p[:6] != b'\xe9AW60\x01':
        raise RuntimeError('AW60 signature missing: ' + p.hex())
    return p

def read_slot(d, slot):
    def query(payload):
        d.send(payload)
        time.sleep(.15)
        return raw_get(d.h, d.dev)
    return b''.join(query(bytes([0x8b, slot, page])) for page in range(4))

def write_slot(d, slot, data):
    if len(data) != SLOT_BYTES:
        raise ValueError('slot size mismatch')
    for page in range(5):
        d.send(bytes([0x16, slot, page, 56, int(page == 4), 0, 0, 0]) + data[page*56:(page+1)*56])
        time.sleep(.15)
    time.sleep(2)

def encode(p):
    blob = bytearray(SLOT_BYTES)
    blob[:4] = MAGIC
    blob[4] = VERSION
    blob[5] = p['mode']
    blob[6] = p['speed']
    blob[7] = p['brightness']
    blob[8] = p['fps']
    blob[9] = p['scale']
    blob[10] = p['amount']
    blob[11] = 1  # explicit palette: black stays black
    blob[12:15] = rgb(p['c1'])
    blob[15:18] = rgb(p['c2'])
    blob[18:21] = rgb(p['c3'])
    name = p['name'].encode('utf8')[:NAME_BYTES-1]
    blob[NAME_OFFSET:NAME_OFFSET+len(name)] = name
    return bytes(blob)

def main():
    d = AW60()
    stamp = time.strftime('%Y%m%d-%H%M%S')
    path = ROOT / f'preset-pack-{stamp}.json'
    record = {'presets': PRESETS, 'before': {}, 'after': {}}
    try:
        status = packet_status(d)
        if status[23] < 7:
            raise RuntimeError('AW60 rev7 required, got status revision %d' % status[23])
        d.stop()
        for slot in range(50):
            record['before'][str(slot)] = read_slot(d, slot).hex()
        path.write_text(json.dumps(record, indent=2), encoding='utf8')
        for preset in PRESETS:
            blob = encode(preset)
            write_slot(d, preset['slot'], blob)
            actual = read_slot(d, preset['slot'])
            if actual != blob:
                raise RuntimeError(f"Preset slot {preset['slot']} readback mismatch")
            record['after'][str(preset['slot'])] = actual.hex()
            path.write_text(json.dumps(record, indent=2), encoding='utf8')
        print(json.dumps({'revision': status[23], 'written_slots': [p['slot'] for p in PRESETS], 'record': str(path)}, ensure_ascii=False), flush=True)
    finally:
        d.close()
        print('Record:', path, flush=True)

if __name__ == '__main__':
    main()
