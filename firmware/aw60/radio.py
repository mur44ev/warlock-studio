"""Known 3151:4011 keyboard receiver relay. No pairing or firmware writes."""
import argparse,json,time
from pathlib import Path
from client import AW60
from hid_transport import library,enumerate_hid,raw_send,raw_get

class Radio(AW60):
    def __init__(self):
        self.h=library();self.dev=None;self.sequence=0;self.selected=False
        ds=[d for d in enumerate_hid(self.h) if (d['vid'],d['pid'],d['page'],d['usage'])==(0x3151,0x4011,0xffff,2)]
        if len(ds)!=1:raise RuntimeError('Expected one 3151:4011 vendor receiver')
        self.dev=self.h.hid_open_path(ds[0]['path'].encode())
        if not self.dev:raise OSError(self.h.hid_error(None))
    def control(self,p):
        p=bytearray(p.ljust(64,b'\0'));p[7]=(255-sum(p[:7]))&255;raw_send(self.h,self.dev,bytes(p))
    def receiver_status(self):
        self.control(b'\xf7');time.sleep(.01);p=raw_get(self.h,self.dev)
        if p[6] not in (1,2,3):raise RuntimeError('Invalid receiver status '+p.hex())
        return dict(ready=p[0]==1,battery=p[1],online=p[3]==0,can_send=p[5]==1,raw=p.hex())
    def send(self,p):
        end=time.monotonic()+.5
        while True:
            s=self.receiver_status()
            if not s['online']:raise RuntimeError('Keyboard offline on radio; switch to 2.4GHz and wake it')
            if s['can_send']:break
            if time.monotonic()>end:raise TimeoutError('Receiver busy')
            time.sleep(.005)
        if not self.selected:self.control(b'\xf6\x0a');time.sleep(.01);self.selected=True
        self.control(p)
    def read(self):
        end=time.monotonic()+1
        while not self.receiver_status()['ready']:
            if time.monotonic()>end:raise TimeoutError('No radio reply')
            time.sleep(.005)
        self.control(b'\xfc');time.sleep(.01);return raw_get(self.h,self.dev)
    def query(self,p):self.send(p);return self.read()

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--identify',action='store_true');a=ap.parse_args();d=Radio()
    try:
        out=dict(receiver=d.receiver_status())
        if a.identify:
            for command in (0x8f,0x80,0xe9):out[hex(command)]=d.query(bytes([command])).hex()
        print(json.dumps(out,indent=2));Path(__file__).with_name('radio-'+time.strftime('%Y%m%d-%H%M%S')+'.json').write_text(json.dumps(out,indent=2))
    finally:d.close()
