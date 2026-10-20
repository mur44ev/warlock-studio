"""Measure compact EA effect delivery using the MCU commit counter."""
import argparse,json,struct,time
from pathlib import Path
from client import AW60,raw_get
from radio import Radio
def status(d):
    if isinstance(d,Radio):p=d.query(b'\xe9')
    else:d.send(b'\xe9');p=raw_get(d.h,d.dev)
    if p[:6]!=b'\xe9AW60\x01' or p[23]<3:raise RuntimeError('AW60 Radio FX revision3 required: '+p.hex())
    return dict(frames=int.from_bytes(p[8:12],'little'),errors=int.from_bytes(p[12:14],'little'),pending=p[22],revision=p[23])
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--radio',action='store_true');ap.add_argument('--seconds',type=float,default=10);ap.add_argument('--fps',type=float,default=60);a=ap.parse_args();d=Radio() if a.radio else AW60()
    try:
        before=status(d);start=time.perf_counter();sent=0
        while time.perf_counter()-start<a.seconds:
            seq=(sent+1)&65535;phase=(sent*65536//360)&65535
            p=bytearray(64);p[0]=0xea;p[1]=0;p[2:4]=struct.pack('<H',seq);p[4:6]=struct.pack('<H',phase);p[6]=180;p[8:11]=bytes([0,180,255]);d.send(p);sent+=1
            delay=start+sent/a.fps-time.perf_counter()
            if delay>0:time.sleep(delay)
        elapsed=time.perf_counter()-start;time.sleep(.08);after=status(d)
        result=dict(radio=a.radio,sent=sent,seconds=elapsed,host_fps=sent/elapsed,accepted=after['frames']-before['frames'],before=before,after=after)
        print(json.dumps(result),flush=True);Path(__file__).with_name('compact-'+time.strftime('%Y%m%d-%H%M%S')+'.json').write_text(json.dumps(result,indent=2))
    finally:d.close()
if __name__=='__main__':main()
