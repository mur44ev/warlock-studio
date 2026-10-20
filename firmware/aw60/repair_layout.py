"""Restore known-good key layers after the rev<=8 RGB/staging alias bug.

Read-only unless --apply. Writes require rev9 or newer and the same profile.
Always saves the current bytes before any writes; reserved tail is stock-owned.
"""
import argparse
import json
import time
from pathlib import Path
from client import AW60, raw_get
from verify_device import baseline_settings

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--force', action='store_true', help='also re-commit matching layers to request native layout sync')
    a = ap.parse_args()
    name, baseline = baseline_settings(a.baseline)
    d = AW60()
    def query(p):
        d.send(bytes(p)); time.sleep(.15)
        return raw_get(d.h, d.dev)
    def layer(command, profile):
        return b''.join(query([command, profile, page]) for page in range(8))
    record = {'baseline': name, 'applied': a.apply, 'layers': {}}
    path = Path(__file__).with_name('layout-repair-'+time.strftime('%Y%m%d-%H%M%S')+'.json')
    def save():
        path.write_text(json.dumps(record, indent=2), encoding='utf8')
    try:
        status = query([0xe9])
        if status[:6] != b'\xe9AW60\x01':
            raise RuntimeError('Unknown firmware')
        if a.apply and status[23] < 9:
            raise RuntimeError('Install rev9 first: older firmware clobbers layout staging')
        profile = query([0x85])[1]
        if profile != bytes.fromhex(baseline['0x85'])[1]:
            raise RuntimeError('Baseline profile differs from the current profile')
        record['profile'] = profile
        for command in (0x89, 0x90):
            now = layer(command, profile)
            wanted = bytes.fromhex(''.join(baseline[hex(command)]))
            if len(now) != 512 or len(wanted) != 512:
                raise RuntimeError('Short layer')
            record['layers'][hex(command)] = {'before': now.hex(), 'wanted': wanted.hex(),
                'changed_offsets': [i for i in range(504) if now[i] != wanted[i]]}
        save()  # Save BOTH original layers before the first write.
        for command in (0x89, 0x90):
            item = record['layers'][hex(command)]
            if a.apply and (item['changed_offsets'] or a.force):
                blob = bytes.fromhex(item['wanted'])
                for page in range(9):
                    d.send(bytes([9 if command == 0x89 else 0x10, profile, 248, 1, page, 0, 0, 0]) + blob[page*56:(page+1)*56])
                    time.sleep(.15)
                time.sleep(2)
            item['verified'] = layer(command, profile)[:504] == bytes.fromhex(item['wanted'])[:504]
            save()
            print(hex(command), 'differing bytes:', len(item['changed_offsets']), 'verified:', item['verified'], flush=True)
            if a.apply and not item['verified']:
                raise RuntimeError('Readback mismatch; original saved in '+str(path))
        print('Record:', path, flush=True)
    finally:
        d.close()

if __name__ == '__main__':
    main()
