"""Execute the actual patched Thumb code offline. Never opens a USB device."""
import binascii
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_IPSR

ROOT = Path(__file__).resolve().parent
M = json.loads((ROOT/'manifest-rev9.json').read_text())
u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
u.mem_map(0x1000000, 0x80000)
u.mem_write(0x1000200, (ROOT/'919_v104-aw60-rev9.bin').read_bytes())
u.mem_map(0x20000, 0x10000)
u.mem_map(0xf8000, 0x10000); u.mem_map(0xe000e000, 0x1000)
u.mem_write(0xf8560, struct.pack('<I', 0)); u.mem_write(0xe000e014, struct.pack('<I', 9599999)); u.mem_write(0xe000e018, struct.pack('<I', 9599999))
STOP = 0x107f000
PACKET, OUTPUT, STATE = 0x21ad8, 0x21681, 0x2cee0
ROOT_DIR = ROOT
CFG_LIGHT, LIGHT_DIRTY, PRESET_BASE = 0x219d8, 0x2047a, 0x1041100
writes = []
u.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, addr, size, value, data: writes.append((addr,size)))
masked_run=masked_max=0
masked_pc=0
def timing(uc,addr,size,data):
    global masked_run,masked_max,masked_pc
    if uc.reg_read(UC_ARM_REG_PRIMASK):
        masked_run+=1
        if masked_run>masked_max:masked_max=masked_run;masked_pc=addr
    else:masked_run=0
u.hook_add(UC_HOOK_CODE,timing)

def call(name, arg=0):
    u.reg_write(UC_ARM_REG_SP, 0x2f000)
    u.reg_write(UC_ARM_REG_LR, STOP | 1)
    u.reg_write(UC_ARM_REG_R0, arg)
    address=M['symbols'][name] if isinstance(name,str) else name
    u.emu_start(address | 1, STOP, count=3000000)
    assert u.reg_read(UC_ARM_REG_PC) == STOP, 'Instruction budget exhausted'
    assert u.reg_read(UC_ARM_REG_SP) == 0x2f000, 'Stack imbalance'
    return u.reg_read(UC_ARM_REG_R0)

def packet(p):
    p=bytearray(p.ljust(64,b'\0'))
    p[7]=(255-sum(p[:7]))&255
    u.mem_write(PACKET,bytes(p))
    return call('checksum_hook',PACKET)

def frame(rgb, seq=1, corrupt=False, service=True):
    for page in range(5):
        data=rgb[page*56:(page+1)*56]
        p=bytearray(struct.pack('<BBHBBBB',0xe8,1,seq,page,5,len(data),0)+data)
        if page==4:
            p+=struct.pack('<H',binascii.crc_hqx(rgb,0xffff)^int(corrupt))
        assert packet(p)==0
    if service:call('service_pending')

def output(): return bytes(u.mem_read(OUTPUT,588))
def status():
    packet(b'\xe9')
    p=bytes(u.mem_read(PACKET,64))
    assert p[:6]==b'\xe9AW60\x01' and (sum(p[:8])&255)==255
    return p

def write_preset(slot, mode, color1, color2, color3, speed=35, brightness=180, fps=60, scale=50, amount=60):
    blob=bytearray(256)
    blob[:4]=b'AWP5'
    blob[4]=1
    blob[5]=mode
    blob[6]=speed
    blob[7]=brightness
    blob[8]=fps
    blob[9]=scale
    blob[10]=amount
    blob[12:15]=bytes(color1)
    blob[15:18]=bytes(color2)
    blob[18:21]=bytes(color3)
    u.mem_write(PRESET_BASE+slot*0x100,bytes(blob))

assert status()[6]==0
# Execute the entire real USB dispatcher reject/cleanup path as well.
p=bytearray(64);p[0]=0xe9;p[7]=(255-0xe9)&255
u.mem_write(PACKET,bytes(p))
call('dispatcher_hook')
assert bytes(u.mem_read(PACKET,6))==b'\xe9AW60\x01'
assert bytes(u.mem_read(0x2048d,1))==b'\x01'
assert call(0x1022d3c,0x22158)==1
assert bytes(u.mem_read(0x22158,6))==b'\xe9AW60\x01'
rgb=bytes((i*73+11)&255 for i in range(240))
u.mem_write(OUTPUT,b'\xa5'*588)
frame(rgb)
expected=bytearray(504)+bytearray(b'\xa5'*84)
for k,offset in enumerate(M['offsets']):
    expected[offset+1:offset+4]=bytes((rgb[k*3]*70//100,rgb[k*3+1],rgb[k*3+2]))
assert output()==expected, 'Frame mapping/scaling mismatch'
assert struct.unpack_from('<I',status(),8)[0]==1
for _ in range(3): call('scan_copy_hook')
assert output()==expected and struct.unpack_from('<H',status(),14)[0]==3
frame(bytes(reversed(rgb)),2,corrupt=True)
assert output()==expected and struct.unpack_from('<I',status(),8)[0]==1
assert struct.unpack_from('<H',status(),12)[0]==1
# Page one without a preceding page zero must never commit.
packet(struct.pack('<BBHBBBB',0xe8,1,3,1,5,56,0)+bytes(56))
assert output()==expected and struct.unpack_from('<H',status(),12)[0]==2
frame(rgb,4)
assert struct.unpack_from('<I',status(),8)[0]==2
packet(b'\xe8\0')
assert status()[6]==0
# Inactive hook must match stock work->scan conversion, including indicators.
work=bytes((i*37+5)&255 for i in range(588))
u.mem_write(0x213e1,work)
before=output()
call('scan_copy_hook')
stock_expected=bytearray(before)
for i in range(147):
    stock_expected[i*4+1:i*4+4]=bytes((work[i*4+1]*70//100,work[i*4+2],work[i*4+3]))
assert output()==stock_expected, 'Original converter behavior changed'
frame(rgb,5)
assert packet(b'\x8f')!=0
assert status()[6]==1, 'Read-only discovery interrupted Direct mode'
# Ordinary hardware lighting command has the stock eight-byte checksum.
p=bytearray(64);p[:8]=bytes([7,1,4,4,7,255,0,0]);p[8]=(255-sum(p[:8]))&255
u.mem_write(PACKET,bytes(p));assert call('checksum_hook',PACKET)!=0
assert status()[6]==0, 'Normal stock command did not release ownership'
# Reject bad checksum without accepting a frame.
p=bytearray(64);p[0]=0xe8;p[1]=1
u.mem_write(PACKET,bytes(p))
assert call('checksum_hook',PACKET)==0
assert status()[6]==0
before_frames=struct.unpack_from('<I',status(),8)[0]
before_errors=struct.unpack_from('<H',status(),12)[0]
frame(bytes(reversed(rgb)),100,service=False)
frame(rgb,101,service=False)
frame(rgb,102,service=False) # Both queued buffers must survive overflow.
assert struct.unpack_from('<I',status(),8)[0]==before_frames, 'USB committed instead of deferring'
u.reg_write(UC_ARM_REG_IPSR,16)
call('service_pending')
assert struct.unpack_from('<I',status(),8)[0]==before_frames, 'Deferred work ran in ISR context'
u.reg_write(UC_ARM_REG_IPSR,0)
call('service_pending')
assert struct.unpack_from('<I',status(),8)[0]==before_frames+1
assert struct.unpack_from('<H',status(),18)[0]==100
call('service_pending')
assert struct.unpack_from('<I',status(),8)[0]==before_frames+2
assert struct.unpack_from('<H',status(),18)[0]==101
assert struct.unpack_from('<H',status(),12)[0]>=before_errors+1
# Compact effect jobs remain deferred and share FIFO ownership with full frames.
for effect in range(6):
    before_frames=struct.unpack_from('<I',status(),8)[0]
    p=bytearray(64);p[0]=0xea;p[1]=effect;p[2:4]=struct.pack('<H',200+effect)
    p[6]=255;p[8:11]=bytes([255,64,16])
    packet(p)
    assert struct.unpack_from('<I',status(),8)[0]==before_frames
    call('service_pending')
    assert struct.unpack_from('<I',status(),8)[0]==before_frames+1
    if effect==0:assert output()[1:4]==bytes([178,0,0])
# Zero brightness must clear all key channels, retaining the indicator row.
p[1]=0;p[6]=0;packet(p);indicators=output()[504:];call('service_pending')
assert output()[:504]==bytes(504) and output()[504:]==indicators
before=output();p[1]=6;packet(p);call('service_pending');assert output()==before
# Simulate the hardware's coarse 100ms tick plus its running downcounter.
u.mem_write(0x20718,struct.pack('<I',0))
before_frames=struct.unpack_from('<I',status(),8)[0]
p=bytearray(64);p[:7]=bytes([0xeb,0,35,180,60,0,180]);p[8]=255;p[9:11]=struct.pack('<H',1234)
packet(p);assert status()[6]==2
for ms in range(1001):
    u.mem_write(0x20718,struct.pack('<I',ms//100))
    u.mem_write(0xe000e018,struct.pack('<I',9599999-(ms%100)*96000));call('service_pending')
assert 60<=struct.unpack_from('<I',status(),8)[0]-before_frames<=61, 'Autonomous clock did not produce60FPS'
packet(bytes([0xeb,255]));before_frames=struct.unpack_from('<I',status(),8)[0]
u.mem_write(0x20718,struct.pack('<I',1100));call('service_pending')
assert struct.unpack_from('<I',status(),8)[0]==before_frames and status()[6]==1
write_preset(1,6,(0,170,255),(255,80,170),(96,255,220),speed=40,scale=58,amount=74)
write_preset(3,10,(255,96,24),(255,180,48),(255,255,200),speed=55,scale=46,amount=88)
u.mem_write(STATE,bytes(32))
u.mem_write(CFG_LIGHT+26,b'\x00')
u.mem_write(LIGHT_DIRTY,b'\x00')
call('scan_copy_hook')
u.mem_write(CFG_LIGHT+26,b'\x01')
u.mem_write(LIGHT_DIRTY,b'\x01')
before_frames=struct.unpack_from('<I',status(),8)[0]
call('scan_copy_hook')
s=status()
assert s[6]==2 and s[23]==9 and s[43]==2 and s[44]==1 and s[45]==0
assert struct.unpack_from('<I',status(),8)[0]>before_frames
u.mem_write(CFG_LIGHT+26,b'\x02')
u.mem_write(LIGHT_DIRTY,b'\x01')
call('scan_copy_hook')
s=status()
assert s[44]==3 and s[45]==1 and s[28]==10 and bytes(u.mem_read(LIGHT_DIRTY,1))==b'\x00'
# ---- Presets: the keyboard's own shortcut cycles saved presets, then shows the stock effect once.
u.mem_write(STATE,bytes(32))
u.mem_write(CFG_LIGHT+26,b'\x04')
u.mem_write(LIGHT_DIRTY,b'\x00')
call('scan_copy_hook')
u.mem_write(0x20718,struct.pack('<I',5000))
u.mem_write(CFG_LIGHT+26,b'\x05');u.mem_write(LIGHT_DIRTY,b'\x01')
frames_before=struct.unpack_from('<I',status(),8)[0]
call('scan_copy_hook');s=status()
assert s[6]==2 and s[44]==1 and s[45]==0, 'first shortcut press must load preset 0'
assert struct.unpack_from('<I',s,8)[0]>frames_before,'preset must render its first frame immediately'
u.mem_write(CFG_LIGHT+26,b'\x06');u.mem_write(LIGHT_DIRTY,b'\x01')
call('scan_copy_hook');s=status()
assert s[44]==3 and s[45]==1 and s[6]==2
u.mem_write(CFG_LIGHT+26,b'\x07');u.mem_write(LIGHT_DIRTY,b'\x01')
call('scan_copy_hook');s=status()
assert s[6]==0 and s[44]==255 and s[46]==7, 'cycle must wrap through the stock effect'
assert bytes(u.mem_read(LIGHT_DIRTY,1))==b'\x01', 'stock effect change must still be applied'
u.mem_write(CFG_LIGHT+26,b'\x08');u.mem_write(LIGHT_DIRTY,b'\x01')
call('scan_copy_hook');s=status()
assert s[6]==2 and s[44]==1 and s[45]==0, 'cycle must wrap back to preset 0'

# ---- A host lighting command (Studio hardware mode) must win over the preset cycle.
p=bytearray(64);p[:8]=bytes([7,5,0,4,7,255,0,0]);p[8]=(255-sum(p[:8]))&255
u.mem_write(0x20718,struct.pack('<I',6000))
u.mem_write(PACKET,bytes(p));call('dispatcher_hook')
call('poll_hook');call('scan_copy_hook');s=status()
assert s[6]==0 and s[46]==5 and s[47]==5, ('host effect was hijacked by a preset',s[6],s[46],s[47])
u.mem_write(0x20718,struct.pack('<I',6020))
u.mem_write(CFG_LIGHT+26,b'\x06');u.mem_write(LIGHT_DIRTY,b'\x01')
call('scan_copy_hook');s=status()
assert s[6]==2, 'after the host window the keyboard shortcut works again'

# ---- Key-reactive effects --------------------------------------------------------------------
import re
names=re.findall(r'\{"([^"]+)",\s*\d+\}',(ROOT.parents[1]/'WarlockLayout.h').read_text())
slots=M['slots'];offsets=M['offsets']
def key_of(name):return names.index(name)
def rowcol(name):
    slot=slots[key_of(name)];return slot%6,slot//6
def px(name):
    o=offsets[key_of(name)];out=output();return out[o+1],out[o+2],out[o+3]
def lit(name):return sum(px(name))
def run_to(t_ms):
    u.mem_write(0x20718,struct.pack('<I',t_ms//100))
    u.mem_write(0xe000e018,struct.pack('<I',9599999-(t_ms%100)*96000))
def advance(t0,span,step=4):
    for t in range(t0,t0+span+1,step):
        run_to(t)
        for _ in range(12):call('service_pending')   # the real main loop spins far faster than 1 ms
    return t0+span
def key_event(name,pressed):
    row,col=rowcol(name)
    u.reg_write(UC_ARM_REG_SP,0x2f000);u.reg_write(UC_ARM_REG_LR,STOP|1)
    u.reg_write(UC_ARM_REG_R0,row);u.reg_write(UC_ARM_REG_R1,col);u.reg_write(UC_ARM_REG_R2,pressed)
    cell=bytearray(u.mem_read(0x20f42+col,1))[0]
    u.mem_write(0x20f42+col,bytes([cell&~(1<<row) if pressed else cell|(1<<row)]))  # stock: clear bit = pressed
    u.emu_start(M['symbols']['key_event_hook']|1,STOP,count=300000)
    assert u.reg_read(UC_ARM_REG_PC)==STOP and u.reg_read(UC_ARM_REG_SP)==0x2f000
def start(mode,c1=(0,90,200),c2=(255,0,160),c3=(255,255,255),speed=40,scale=50,amount=60,flags=1):
    run_to(10000)
    p=bytearray(64);p[:7]=bytes([0xeb,mode,speed,255,60,c1[0],c1[1]]);p[8]=c1[2];p[9:11]=struct.pack('<H',77+mode)
    p[11]=scale;p[12]=amount;p[13:16]=bytes(c2);p[16:19]=bytes(c3);p[19]=flags
    packet(p);s=status();assert s[6]==2 and s[28]==mode,('effect did not start',s[6],s[28])
    return advance(10000,200)
def rows_lit(threshold,exclude=()):
    return [n for n in names if n not in exclude and lit(n)>threshold]

assert M['symbols'].get('key_event_hook'),'key hook missing from image'
u.mem_write(0x20f42,bytes([255]*15))
# EC reports the stock matrix bytes and our held bits.
t=start(14)
assert status()[56]==1, 'key state RAM self-check failed'
packet(b'\xec');k=bytes(u.mem_read(PACKET,64))
assert k[:4]==b'\xecKEY' and k[5]==1 and k[8:23]==bytes([255]*15) and (sum(k[:8])&255)==255
assert k[24:34]==bytes(10) and struct.unpack_from('<I',k,34)[0]==0
key_event('A',1);packet(b'\xec');k=bytes(u.mem_read(PACKET,64))
assert k[24+key_of('A')//8]&(1<<(key_of('A')%8)) and struct.unpack_from('<I',k,34)[0]==1
key_event('A',0);packet(b'\xec');k=bytes(u.mem_read(PACKET,64))
assert k[24:34]==bytes(10) and struct.unpack_from('<I',k,34)[0]==1
# Out-of-range scanner events must be ignored.
key_event('A',1);key_event('A',0)
u.reg_write(UC_ARM_REG_SP,0x2f000);u.reg_write(UC_ARM_REG_LR,STOP|1)
u.reg_write(UC_ARM_REG_R0,9);u.reg_write(UC_ARM_REG_R1,40);u.reg_write(UC_ARM_REG_R2,1)
u.emu_start(M['symbols']['key_event_hook']|1,STOP,count=300000)

# Glow: idle = dim background only; pressed key flashes to colour C and decays back.
t=start(14)
idle=lit('A');assert 0<idle<200,('background should be dim but visible',idle)
assert abs(lit('A')-lit('Space'))<60
key_event('A',1);t=advance(t,40)
assert lit('A')>600 and lit('A')>idle*3,'pressed key must flash'
assert lit('S')<idle*2,'neighbours must stay at background'
key_event('A',0);t=advance(t,3500)
assert lit('A')<idle*2,('glow must fade out',lit('A'),idle)

# Ripple: ring leaves the pressed key and travels outward; far keys stay dark; ends after 1.5 s.
t=start(15);idle=lit('A')
key_event('G',1);key_event('G',0);t=advance(t,60)
near=lit('G');assert near>400
t=advance(t,100)
ring=[n for n in names if n not in('G',) and lit(n)>idle*3]
assert len(ring)>=3,('ripple ring missing',ring)
assert lit('F12')<idle*2 and lit('Right')<idle*2,'ring must not reach far keys yet'
t=advance(t,1500)
assert all(lit(n)<idle*2 for n in names),'ripple must fully expire'

# Laser cross: beams along the pressed key's row and column only.
t=start(16);idle=lit('A')
key_event('G',1);key_event('G',0);t=advance(t,100)
assert lit('J')>idle*3 and lit('F')>idle*3, 'row beam'
assert lit('T')>idle*3 or lit('B')>idle*3, 'column beam'
assert lit('F9')<idle*2 and lit('Right')<idle*2, 'off-axis keys must stay dark'
t=advance(t,1200)
assert all(lit(n)<idle*2 for n in names)

# Heat: repeated presses accumulate and cool slowly.
t=start(17);idle=lit('A')
for _ in range(4):
    key_event('A',1);t=advance(t,40);key_event('A',0);t=advance(t,120)
hot=lit('A');assert hot>500,('heat should accumulate',hot)
t=advance(t,3000);warm=lit('A')
assert idle*2<warm<hot,('heat should cool gradually',idle,warm,hot)
key_event('K',1);key_event('K',0);t=advance(t,100)
assert hot>lit('K') or lit('K')<hot*2
assert lit('A')>idle*2,'heat is sticky'

# Fireworks: sparks scatter around the press and burn out.
t=start(18);idle=lit('A')
key_event('G',1);key_event('G',0);t=advance(t,350)
spark=[n for n in names if lit(n)>idle*3]
assert len(spark)>=4,('fireworks should light several keys',spark)
t=advance(t,1500)
assert all(lit(n)<idle*2 for n in names)

# Explicit palette keeps black; legacy packets still derive missing colours.
start(14,c2=(0,0,0),c3=(0,0,0),flags=1);s=status()
assert s[37:43]==bytes(6),'explicit black palette must be preserved'
start(14,c1=(10,20,30),c2=(0,0,0),c3=(0,0,0),flags=0);s=status()
assert s[37:43]!=bytes(6),'legacy packet must still get derived colours'

# Time stepping backwards (pending tick ISR at a counter wrap) must not corrupt rendering.
t=start(15);before=struct.unpack_from('<I',status(),8)[0]
run_to(t-130);call('service_pending')
assert struct.unpack_from('<I',status(),8)[0]==before,'non-monotonic time must be ignored'
t=advance(t,100);assert struct.unpack_from('<I',status(),8)[0]>before

# Every reactive effect must run well inside the 60 FPS budget (worst case: full ring of hits).
worst=0
for mode in range(0,19):
    t=start(mode)
    for i,name in enumerate(['Q','W','E','R','T','Y','U','I','O','P','A','S']):
        key_event(name,1);key_event(name,0);t=advance(t,8,4)
    run_to(t+40)
    calls=[]
    frame_before=struct.unpack_from('<I',status(),8)[0]
    while struct.unpack_from('<I',status(),8)[0]==frame_before:
        n=[0]
        def counter(uc,addr,size,data):n[0]+=1
        u.reg_write(UC_ARM_REG_SP,0x2f000);u.reg_write(UC_ARM_REG_LR,STOP|1)
        h=u.hook_add(UC_HOOK_CODE,counter)
        u.emu_start(M['symbols']['service_pending']|1,STOP,count=3000000)
        u.hook_del(h);calls.append(n[0])
        assert len(calls)<70,'frame never completed'
    count=[max(calls)]
    print('  mode',mode,'slices',len(calls),'worst slice',count[0],'frame total',sum(calls))
    worst=max(worst,count[0])
assert worst<260000,('a frame must stay within budget',worst)
print('worst frame (instructions, one call):',worst)

# ---- rev9: system-wide fast divider, fast converter, ED report, ISR wrappers, idle cost ----------
import random
stock_image = (ROOT.parents[1]/'firmware/stock/919_v104.bin').read_bytes()
# The patched divider must equal the stock bit-serial one for every input class (run both images).
s_stock = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
s_stock.mem_map(0x1000000, 0x80000); s_stock.mem_write(0x1000200, stock_image); s_stock.mem_map(0x20000, 0x10000)
def stock_div(a, b):
    s_stock.reg_write(UC_ARM_REG_SP, 0x2f000); s_stock.reg_write(UC_ARM_REG_LR, STOP | 1)
    s_stock.reg_write(UC_ARM_REG_R0, a); s_stock.reg_write(UC_ARM_REG_R1, b)
    s_stock.emu_start(0x1029a6c | 1, STOP, count=100000)
    return s_stock.reg_read(UC_ARM_REG_R0), s_stock.reg_read(UC_ARM_REG_R1)
def patched_div(a, b):
    u.reg_write(UC_ARM_REG_SP, 0x2f000); u.reg_write(UC_ARM_REG_LR, STOP | 1)
    u.reg_write(UC_ARM_REG_R0, a); u.reg_write(UC_ARM_REG_R1, b)
    n = [0]
    h = u.hook_add(UC_HOOK_CODE, lambda uc, addr, size, data: n.__setitem__(0, n[0] + 1))
    u.emu_start(0x1029a6c | 1, STOP, count=100000)
    u.hook_del(h)
    assert u.reg_read(UC_ARM_REG_SP) == 0x2f000, 'divider changed the stack'
    return u.reg_read(UC_ARM_REG_R0), u.reg_read(UC_ARM_REG_R1), n[0]
random.seed(8)
cases = [(0, 1), (1, 1), (100, 7), (255 * 70, 100), (5, 0), (0, 0), (0x80000000, 0), (0xffffffff, 0), (0xffffffff, 1), (0xffffffff, 0xffffffff),
         (0x80000000, 3), (7, 0xffffffff), (0x7fffffff, 2), (96000, 1000), (0x10000, 0x10000), (0xfffffffe, 0xffffffff)]
cases += [(random.getrandbits(random.choice([4, 8, 16, 24, 32])), random.getrandbits(random.choice([1, 4, 8, 16, 32]))) for _ in range(1500)]
worst_new = 0
for a, b in cases:
    q, r, n = patched_div(a, b)
    assert (q, r) == stock_div(a, b), ('divider mismatch', hex(a), hex(b), (q, r), stock_div(a, b))
    worst_new = max(worst_new, n)
print('divider: %d cases identical to stock, patched worst %d instructions (stock 268..460)' % (len(cases), worst_new))
# The signed wrapper and a real stock caller keep working through the trampoline.
u.reg_write(UC_ARM_REG_SP, 0x2f000); u.reg_write(UC_ARM_REG_LR, STOP | 1)
u.reg_write(UC_ARM_REG_R0, (-1000) & 0xffffffff); u.reg_write(UC_ARM_REG_R1, 7)
u.emu_start(0x1029a98 | 1, STOP, count=100000)
assert u.reg_read(UC_ARM_REG_R0) == (-142) & 0xffffffff and u.reg_read(UC_ARM_REG_R1) == (-6) & 0xffffffff

# The fast converter must equal the original converter byte for byte on arbitrary work buffers.
for trial in range(6):
    work = bytes(random.getrandbits(8) for _ in range(588))
    seed_out = bytes(random.getrandbits(8) for _ in range(588))
    u.mem_write(0x213e1, work); u.mem_write(OUTPUT, seed_out)
    call(0x102006c); original = output()
    u.mem_write(OUTPUT, seed_out)
    n = [0]
    h = u.hook_add(UC_HOOK_CODE, lambda uc, addr, size, data: n.__setitem__(0, n[0] + 1))
    call('fast_convert'); u.hook_del(h)
    assert output() == original, 'fast converter differs from the stock converter'
print('converter: identical to stock on random buffers, %d instructions (stock ~52000)' % n[0])

# ED report: pages, checksum, reset.
t = start(14)
packet(b'\xed\x00\x00');k = bytes(u.mem_read(PACKET, 64))
assert k[:4] == b'\xedPF\x00' and (sum(k[:8]) & 255) == 255 and k[5] == 1
assert struct.unpack_from('<I', k, 36)[0] == 96000 and struct.unpack_from('<I', k, 12)[0] == 9599999
packet(b'\xed\x01\x00');k = bytes(u.mem_read(PACKET, 64))
polls = struct.unpack_from('<I', k, 12)[0]
for _ in range(5): call('poll_hook')
packet(b'\xed\x01\x01');k2 = bytes(u.mem_read(PACKET, 64))      # read + reset
assert struct.unpack_from('<I', k2, 12)[0] >= polls and struct.unpack_from('<I', k2, 24)[0] > 0, 'frames and polls must be counted'
packet(b'\xed\x01\x00');k3 = bytes(u.mem_read(PACKET, 64))
assert struct.unpack_from('<I', k3, 12)[0] == 0 and struct.unpack_from('<I', k3, 16)[0] == 0, 'reset must clear counters'
assert sum(struct.unpack_from('<8H', k3, 40)) == 0

# ISR wrappers: with a bare `bx lr` standing in for each stock handler, the wrapper must call it, return
# cleanly, restore the stack and account the time.
for name, handler, index in (('irq0_wrap', 0x102366e, 0), ('irq14_wrap', 0x101e93c, 1), ('irq15_wrap', 0x101e9d4, 2), ('irq16_wrap', 0x101eb66, 3)):
    saved = bytes(u.mem_read(handler, 2)); u.mem_write(handler, b'\x70\x47')
    packet(b'\xed\x02\x01'); before = bytes(u.mem_read(PACKET, 64))
    for _ in range(3): call(name)
    packet(b'\xed\x02\x00'); after = bytes(u.mem_read(PACKET, 64))
    assert struct.unpack_from('<I', after, 12 + index * 12)[0] == 3, (name, 'ISR must be counted')
    u.mem_write(handler, saved)
packet(b'\xed\x02\x01')

# Idle cost: nothing due -> a few dozen instructions, not ~1400 as in rev7.
t = start(0)
frames = lambda: struct.unpack_from('<I', status(), 8)[0]
before = frames()
run_to(t + 40)                                        # a frame is due; render it with the clock frozen
while frames() == before: call('service_pending')
n = [0]
h = u.hook_add(UC_HOOK_CODE, lambda uc, addr, size, data: n.__setitem__(0, n[0] + 1))
call('service_pending'); call('service_pending'); u.hook_del(h)
print('idle service_pending: %d instructions per call (rev7: ~1400)' % (n[0] // 2))
assert n[0] // 2 < 200, ('idle path too expensive', n[0] // 2)

# RGB must NEVER write into the stock 512-byte keymap/flash staging buffer.
# All existing tests above cover init, E8/EA/EB, native preset changes and IRQs.
clobbers=[(addr,size) for addr,size in writes if addr<0x21d18 and addr+size>0x21b18]
assert not clobbers, ('RGB overwrote stock layout staging',clobbers[:20])
allowed=[(0x20000,0x30000)]
assert all(any(lo<=addr and addr+size<=hi for lo,hi in allowed) for addr,size in writes), sorted(set(writes))
assert 0<masked_max<1400, ('Excessive LED scan IRQ blackout',masked_max,hex(masked_pc),sorted((v,k) for k,v in M['symbols'].items() if v<=masked_pc)[-1])
print('PASS: actual Thumb code; frame CRC/order/mapping; presets; stock fallback; host window; key hook; 5 reactive effects; RAM-only writes; stack balance; max IRQ-masked instructions',masked_max)
stack_low=min(a for a,_ in writes if a>=0x2d000)
print('deepest stack use:',0x2f000-stack_low,'bytes; key state RAM',hex(max(a for a,_ in writes if 0x2c000<=a<0x2d000)-0x2c000+1),'bytes')
assert 0x2f000-stack_low<3072,'stack use too deep for the stock firmware'
