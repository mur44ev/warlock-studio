"""One-device GP75 v104 USB trial on the explicitly authorized Warlock.

Default: enumerate and save readable settings only. --flash performs the update.
No bootloader prefix is transmitted. No generic erase, SWD unlock or RF writes.
"""
import argparse
import ctypes as c
import hashlib
import json
import math
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

from hid_transport import library, transact, enumerate_hid, raw_send, raw_get

ROOT = Path(__file__).resolve().parent
IMAGE_HASH = 'F6BFA08E0EF169D0DD66D7F628D77E67FA9A37818F62085319D58C66F58DE1C4'

def image_data(rollback=False):
    manifest = json.loads((ROOT/f'manifest-rev9{SUFFIX}.json').read_text())
    if rollback:
        image = (ROOT.parents[0]/'stock/919_v104.bin').read_bytes()
        expected = IMAGE_HASH
    else:
        image = (ROOT/f'919_v104-aw60-rev9{SUFFIX}.bin').read_bytes()
        expected = manifest['patched_sha256'].upper()
    if hashlib.sha256(image).hexdigest().upper() != expected:
        raise ValueError('Firmware hash mismatch')
    if len(image) != 282912:
        raise ValueError('Unexpected firmware length')
    if hashlib.sha256(image[:65536]).hexdigest() != manifest['stock_boot_prefix_sha256']:
        raise ValueError('Boot prefix changed')
    return image[65536:]


SKIP_PRESET_BACKUP = False
SUFFIX = ''   # '-s1' / '-s2' select the staged rev9 images (see build_patch.py --stage)

def input_required_for_unsaved_presets():
    return not SKIP_PRESET_BACKUP

def restore_presets(event, backup_file, rollback):
    # Never let a preset problem turn a good flash into an error: report it and move on.
    if rollback or backup_file is None:
        return
    try:
        import json as _json
        if not _json.loads(Path(backup_file).read_text(encoding='utf8'))['slots']:
            return
        time.sleep(3)
        from presets_io import AW60, restore
        writer = AW60()
        try:
            event('presets-restored', slots=restore(writer, backup_file))
        finally:
            writer.close()
    except Exception as exc:
        event('presets-restore-failed', error=repr(exc), backup=str(backup_file),
              hint='run: py -3.12 firmware/aw60/presets_io.py --restore ' + str(backup_file))

def main():
    global SKIP_PRESET_BACKUP,SUFFIX
    ap = argparse.ArgumentParser()
    ap.add_argument('--skip-preset-backup', action='store_true', help='flash even if the onboard presets cannot be read first')
    ap.add_argument('--stage', type=int, choices=[1,2,3], default=3, help='rev9 staged image: 1 code only, 2 + divider, 3 full')
    ap.add_argument('--flash', action='store_true')
    ap.add_argument('--rollback', action='store_true', help='Restore unmodified, hash-checked GP75 v104 application')
    args = ap.parse_args()
    SKIP_PRESET_BACKUP = args.skip_preset_backup
    SUFFIX = '' if args.stage==3 else f'-s{args.stage}'
    payload = image_data(args.rollback)
    count = math.ceil(len(payload)/64)
    checksum = sum(payload) & 0xffffff
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    record_path = ROOT/f'{"rollback" if args.rollback else "custom"}-{stamp}.json'
    record = dict(image='stock' if args.rollback else f'AW60 rev9{SUFFIX}', sha256=hashlib.sha256(payload).hexdigest(),
        payload_bytes=len(payload), chunks=count, checksum=checksum,
        requested_flash=args.flash, events=[])
    def event(phase, **data):
        record['events'].append(dict(time=datetime.now().isoformat(),phase=phase,**data))
        record_path.write_text(json.dumps(record,indent=2),encoding='utf8')
        print(phase, json.dumps(data), flush=True)
    h = library()
    dev = None
    try:
        before = enumerate_hid(h)
        event('inventory-before', devices=before)
        candidates = [d for d in before if (d['vid'],d['pid'],d['page'],d['usage']) == (0x3151,0x4015,0xffff,2)]
        if len(candidates) != 1: raise RuntimeError(f'Expected one wired keyboard, got {len(candidates)}')
        selected = candidates[0]
        dev = h.hid_open_path(selected['path'].encode())
        if not dev: raise OSError(h.hid_error(None))
        identity = transact(h,dev,b'\x8f')
        revision = transact(h,dev,b'\x80')
        if identity[0] != 0x8f or struct.unpack_from('<H',identity,1)[0] != 919:
            raise RuntimeError('Target is not the authorized GP75-converted Warlock model 919')
        if revision[:3] != bytes.fromhex('800401'):
            raise RuntimeError('Unexpected starting firmware revision')
        settings = {'identity':identity.hex(),'revision':revision.hex()}
        for command in (0x85,0x86,0x87,0x91,0x92,0x97):
            settings[hex(command)] = transact(h,dev,bytes([command])).hex()
        profile = bytes.fromhex(settings['0x85'])[1]
        if profile > 4: raise RuntimeError('Unexpected profile value')
        for command, pages in ((0x89,8),(0x90,8),(0x8c,6)):
            settings[hex(command)] = [transact(h,dev,bytes([command,profile if command!=0x8c else 0,page])).hex() for page in range(pages)]
        event('settings-saved',settings=settings,note='Settings only, not a firmware dump')
        event('image-validated',payload_bytes=len(payload),chunks=count,checksum=checksum)
        if not args.flash:
            event('preflight-complete'); return
        # The flash rewrites the preset area too: keep a copy and put it back afterwards.
        preset_backup = None
        try:
            from presets_io import AW60, backup as backup_presets
            reader = AW60()
            try:
                preset_backup, slots = backup_presets(reader)
            finally:
                reader.close()
            event('presets-backed-up', file=str(preset_backup), slots=sorted(int(k) for k in slots))
        except Exception as exc:
            event('presets-backup-failed', error=repr(exc))
            if input_required_for_unsaved_presets():
                raise RuntimeError('Preset backup failed; refusing to flash. Use --skip-preset-backup to override.')
        # Vendor yc500 boot entry, Bit7 complement in payload byte 7.
        raw_send(h,dev,bytes.fromhex('7f55aa55aa000082'))
        event('boot-entry-sent')
        h.hid_close(dev); dev = None
        before_paths = {d['path'] for d in before}
        deadline = time.monotonic()+25
        boot = None
        while time.monotonic() < deadline:
            now = enumerate_hid(h)
            new = [d for d in now if d['path'] not in before_paths and
                d['pid'] not in (0x4011,0x4015) and
                (d['page'],d['usage']) in ((0xff01,1),(0xffff,2),(1,6))]
            # Several HID collections can belong to the same boot device.
            if new:
                groups = {(d['vid'],d['pid']) for d in new}
                if len(groups) != 1: raise RuntimeError('Ambiguous newly attached boot devices')
                new.sort(key=lambda d: ((d['page'],d['usage'])!=(0xff01,1),d['path']))
                boot = new[0]; break
            time.sleep(.25)
        if boot is None:
            event('inventory-after-entry',devices=enumerate_hid(h))
            raise RuntimeError('Boot device did not enumerate; no image data sent')
        event('boot-device-found',device=boot)
        dev = h.hid_open_path(boot['path'].encode())
        if not dev: raise OSError(h.hid_error(None))
        time.sleep(1)
        init = b'\xba\xc0'+struct.pack('<H',count)+len(payload).to_bytes(3,'little')
        raw_send(h,dev,init)
        time.sleep(.02)
        response = raw_get(h,dev)
        event('init-response',payload=response.hex())
        if response[:4] != b'\xab\xc0'+struct.pack('<H',count):
            raise RuntimeError('Boot initialization was not acknowledged; stopped before data')
        time.sleep(1)
        for index in range(count):
            raw_send(h,dev,payload[index*64:(index+1)*64])
            # Host driver sends reports synchronously; allow flash commit time.
            time.sleep(.01)
            if index % 200 == 0 or index == count-1:
                event('write-progress',chunks_sent=index+1,total=count)
        # Match the local yc500 driver's 24-bit checksum and 24-bit length.
        verify = b'\xba\xc2'+struct.pack('<H',count)+checksum.to_bytes(3,'little')+len(payload).to_bytes(3,'little')
        raw_send(h,dev,verify)
        time.sleep(.02)
        response = raw_get(h,dev)
        event('verify-response',payload=response.hex())
        if response[:4] != b'\xab\xc2'+struct.pack('<H',count) or response[4] != 0x55:
            raise RuntimeError('Bootloader did not confirm successful verification')
        h.hid_close(dev); dev = None
        deadline = time.monotonic()+25
        while time.monotonic() < deadline:
            now = enumerate_hid(h)
            apps = [d for d in now if (d['vid'],d['pid'],d['page'],d['usage']) == (0x3151,0x4015,0xffff,2)]
            if len(apps)==1:
                dev = h.hid_open_path(apps[0]['path'].encode())
                if dev:
                    try:
                        identity=transact(h,dev,b'\x8f');revision=transact(h,dev,b'\x80')
                        if identity[0]==0x8f and revision[0]==0x80:
                            event('application-returned',identity=identity.hex(),revision=revision.hex(),device=apps[0])
                            restore_presets(event, preset_backup, args.rollback)
                            return
                    except OSError: pass
                    h.hid_close(dev);dev=None
            time.sleep(.5)
        event('inventory-after-verify',devices=enumerate_hid(h))
        raise RuntimeError('Image verified but application did not return within timeout')
    except Exception as exc:
        event('failed',error=repr(exc))
        raise
    finally:
        if dev: h.hid_close(dev)
        print('Record:',record_path,flush=True)

if __name__=='__main__': main()
