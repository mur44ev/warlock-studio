"""Restore hash-checked stock GP75 application ONLY if its known USB bootloader is present.

Does not force hardware boot entry. Default lists inventory, --flash writes stock.
"""
import argparse
import json
import math
import struct
import time
from pathlib import Path
from flash_aw60 import library, enumerate_hid, raw_send, raw_get, image_data

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--flash',action='store_true');a=ap.parse_args()
    h=library();inventory=enumerate_hid(h)
    print(json.dumps(inventory,indent=2))
    if not a.flash:return
    candidates=[d for d in inventory if (d['vid'],d['pid'],d['page'],d['usage'])==(0x3151,0x4016,0xff01,1)]
    if len(candidates)!=1:raise RuntimeError('Exactly one known 3151:4016 bootloader is required')
    if any(d['pid']==0x4015 for d in inventory):raise RuntimeError('Application present; use flash_aw60.py --rollback instead')
    payload=image_data(rollback=True);count=math.ceil(len(payload)/64);checksum=sum(payload)&0xffffff
    log=[];path=Path(__file__).with_name('recovery-'+time.strftime('%Y%m%d-%H%M%S')+'.json')
    def event(phase,**data):
        log.append(dict(time=time.time(),phase=phase,**data));path.write_text(json.dumps(log,indent=2));print(phase,data,flush=True)
    dev=h.hid_open_path(candidates[0]['path'].encode())
    if not dev:raise OSError(h.hid_error(None))
    try:
        event('target',device=candidates[0],stock_only=True)
        raw_send(h,dev,b'\xba\xc0'+struct.pack('<H',count)+len(payload).to_bytes(3,'little'))
        time.sleep(.02);r=raw_get(h,dev);event('init',response=r.hex())
        if r[:4]!=b'\xab\xc0'+struct.pack('<H',count):raise RuntimeError('No init acknowledgement')
        time.sleep(1)
        for i in range(count):
            raw_send(h,dev,payload[i*64:(i+1)*64]);time.sleep(.01)
            if i%200==0:event('progress',sent=i+1,total=count)
        raw_send(h,dev,b'\xba\xc2'+struct.pack('<H',count)+checksum.to_bytes(3,'little')+len(payload).to_bytes(3,'little'))
        time.sleep(.02);r=raw_get(h,dev);event('verify',response=r.hex())
        if r[:5]!=b'\xab\xc2'+struct.pack('<H',count)+b'\x55':raise RuntimeError('Stock verification failed')
    finally:h.hid_close(dev)
    print('Stock verified. Wait for USB application 3151:4015; log:',path)
if __name__=='__main__':main()
