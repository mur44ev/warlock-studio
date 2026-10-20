"""Back up and restore the AW60 onboard lighting presets (macro slots reused as AWP5 blobs).

A firmware flash rewrites the whole application image, which includes the preset area, so
flash_aw60.py calls this before and after every custom flash. It can also be used by hand:

    py -3.12 firmware/aw60/presets_io.py --backup
    py -3.12 firmware/aw60/presets_io.py --restore firmware/aw60/presets-backup-YYYYmmdd-HHMMSS.json

Backups are read-only. Restore writes only slots whose content differs and verifies each one.
"""
import argparse
import json
import time
from pathlib import Path

from client import AW60, raw_get

ROOT = Path(__file__).resolve().parent
SLOT_COUNT = 50
SLOT_BYTES = 256
MAGIC = b'AWP5'


def read_slot(d, slot):
    blob = bytearray()
    for page in range(4):
        d.send(bytes([0x8b, slot, page]))
        time.sleep(.15)
        blob += raw_get(d.h, d.dev)[:64]
    return bytes(blob)


def write_slot(d, slot, blob):
    if len(blob) != SLOT_BYTES:
        raise ValueError('preset slot must be %d bytes' % SLOT_BYTES)
    for page in range(5):
        d.send(bytes([0x16, slot, page, 56, int(page == 4), 0, 0, 0]) + blob[page * 56:(page + 1) * 56])
        time.sleep(.15)
    time.sleep(2)


def valid(blob):
    return blob[:4] == MAGIC and blob[4] == 1


def backup(d, path=None):
    slots = {}
    for slot in range(SLOT_COUNT):
        blob = read_slot(d, slot)
        if valid(blob):
            slots[str(slot)] = blob.hex()
    path = Path(path) if path else ROOT / ('presets-backup-' + time.strftime('%Y%m%d-%H%M%S') + '.json')
    path.write_text(json.dumps({'format': 'AW60-presets-1', 'slots': slots}, indent=2), encoding='utf8')
    return path, slots


def restore(d, path):
    data = json.loads(Path(path).read_text(encoding='utf8'))
    if data.get('format') != 'AW60-presets-1':
        raise RuntimeError('Not an AW60 preset backup')
    written = []
    for key, hexdata in sorted(data['slots'].items(), key=lambda item: int(item[0])):
        slot, blob = int(key), bytes.fromhex(hexdata)
        if not 0 <= slot < SLOT_COUNT or not valid(blob):
            raise RuntimeError('Backup slot %s is not a valid preset' % key)
        if read_slot(d, slot) == blob:
            continue
        write_slot(d, slot, blob)
        if read_slot(d, slot) != blob:
            raise RuntimeError('Preset slot %d readback mismatch after restore' % slot)
        written.append(slot)
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--backup', action='store_true')
    ap.add_argument('--restore', metavar='FILE')
    a = ap.parse_args()
    if not a.backup and not a.restore:
        ap.error('choose --backup or --restore FILE')
    d = AW60()
    try:
        if a.backup:
            path, slots = backup(d)
            print(json.dumps({'saved': str(path), 'slots': sorted(int(k) for k in slots)}))
        if a.restore:
            print(json.dumps({'restored_slots': restore(d, a.restore)}))
    finally:
        d.close()


if __name__ == '__main__':
    main()
