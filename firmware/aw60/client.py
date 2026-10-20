"""AW60 RAM frame transport and device-side acceptance benchmark."""
import argparse
import binascii
import colorsys
import json
import struct
import sys
import time
from pathlib import Path
from hid_transport import library, enumerate_hid, raw_send, raw_get

class AW60:
    def __init__(self):
        self.h=library()
        ds=[d for d in enumerate_hid(self.h) if (d['vid'],d['pid'],d['page'],d['usage'])==(0x3151,0x4015,0xffff,2)]
        if len(ds)!=1: raise RuntimeError('Expected exactly one wired GP75')
        self.dev=self.h.hid_open_path(ds[0]['path'].encode())
        if not self.dev: raise OSError(self.h.hid_error(None))
        self.sequence=0
    def close(self):
        if self.dev: self.h.hid_close(self.dev);self.dev=None
    def send(self,p):
        p=bytearray(p.ljust(64,b'\0'))
        p[7]=(255-sum(p[:7]))&255
        raw_send(self.h,self.dev,bytes(p))
    def status(self):
        # A concurrent streamer can replace the shared vendor reply before GET.
        # Do not insert the stock 150ms sleep between SET and GET here.
        for attempt in range(20):
            self.send(b'\xe9');p=raw_get(self.h,self.dev)
            if p[:6]==b'\xe9AW60\x01':break
            time.sleep(.005)
        else:raise RuntimeError('AW60 signature missing: '+p.hex())
        return dict(active=p[6],frames=int.from_bytes(p[8:12],'little'),errors=int.from_bytes(p[12:14],'little'),
                    ticks=int.from_bytes(p[14:16],'little'),crc=int.from_bytes(p[16:18],'little'),seq=int.from_bytes(p[18:20],'little'),pending=p[22])
    def frame(self,rgb):
        if len(rgb)!=240: raise ValueError('Need 80 RGB pixels')
        self.sequence=(self.sequence+1)&65535
        for n in range(5):
            chunk=rgb[n*56:(n+1)*56]
            p=struct.pack('<BBHBBBB',0xe8,1,self.sequence,n,5,len(chunk),0)+chunk
            if n==4:p+=struct.pack('<H',binascii.crc_hqx(rgb,0xffff))
            self.send(p)
    def stop(self):self.send(b'\xe8\0')

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--seconds',type=float,default=0)
    ap.add_argument('--fps',type=float,default=60)
    ap.add_argument('--solid',choices=['red','green','blue','white'])
    ap.add_argument('--stop',action='store_true')
    a=ap.parse_args();d=AW60()
    try:
        before=d.status();print('before',json.dumps(before),flush=True)
        if a.stop:d.stop();return
        if a.solid:
            rgb=dict(red=b'\xff\0\0',green=b'\0\xff\0',blue=b'\0\0\xff',white=b'\xff\xff\xff')[a.solid]*80
            d.frame(rgb);print('after',json.dumps(d.status()),flush=True);return
        if a.seconds:
            start=time.perf_counter();sent=0;latencies=[]
            while time.perf_counter()-start<a.seconds:
                rgb=bytes(c for i in range(80) for c in [int(v*180) for v in colorsys.hsv_to_rgb((i/80+sent/180)%1,1,1)])
                t=time.perf_counter();d.frame(rgb);latencies.append(time.perf_counter()-t);sent+=1
                delay=start+sent/a.fps-time.perf_counter()
                if delay>0:time.sleep(delay)
            elapsed=time.perf_counter()-start
            time.sleep(.05) # Let thread-mode renderer consume the final queued frame.
            after=d.status()
            result=dict(sent=sent,elapsed=elapsed,host_fps=sent/elapsed,accepted=after['frames']-before['frames'],
                        before=before,after=after,max_send_ms=max(latencies)*1000)
            name=Path(__file__).with_name('benchmark-'+time.strftime('%Y%m%d-%H%M%S')+'.json')
            name.write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
    finally:d.close()
if __name__=='__main__':main()
