"""Build and verify a minimal GP75 v104 RAM-stream overlay. Never flashes."""
from pathlib import Path
import hashlib,json,re,shutil,subprocess,struct,sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
STOCK=ROOT/'firmware/stock/919_v104.bin'
BASE=0x01000200
PATCH=0x01034000
SHA='f6bfa08e0ef169d0dd66d7f628d77e67fa9a37818f62085319d58c66f58de1c4'
COPY_CALLS=[0x0101552e,0x01015daa,0x01016d78,0x01016f26,0x0101b6b0]
KEY_EVENT_CALL=0x010196aa   # stock matrix scan -> reactive-lighting key hook(row,col,pressed)
KEY_EVENT_ORIG=0x0101f506
# Flash vector table (stub trampolines index it) and the stock bit-serial divider.
VECTOR_TABLE=0x010102bc
VECTORS={0:(0x0102366f,'irq0_wrap'),14:(0x0101e93d,'irq14_wrap'),15:(0x0101e9d5,'irq15_wrap'),16:(0x0101eb67,'irq16_wrap')}
DIVIDER=0x01029a6c
DIVIDER_PROLOGUE=bytes.fromhex('30b50b4601460020')
# --stage N builds a subset for staged flashing: 1 = our code only, 2 = + system divider, 3 = + ISR wrappers (default).
STAGE=int(sys.argv[sys.argv.index('--stage')+1]) if '--stage' in sys.argv else 3
SUFFIX='' if STAGE==3 else f'-s{STAGE}'
IMAGE_NAME=f'919_v104-aw60-rev9{SUFFIX}.bin'
MANIFEST_NAME=f'manifest-rev9{SUFFIX}.json'

def bl(source,target):
    offset=target-source-4
    assert offset%2==0 and -(1<<22)<=offset<(1<<22)
    # Thumb-1 BL encoding; all patch targets are within its +/-4MiB range.
    return struct.pack('<HH',0xf000|((offset>>12)&0x7ff),0xf800|((offset>>1)&0x7ff))

def main():
    data=STOCK.read_bytes()
    assert hashlib.sha256(data).hexdigest()==SHA
    text=(ROOT/'WarlockLayout.h').read_text()
    slots=[int(x) for x in re.findall(r'\{"[^"]+",\s*(\d+)\}',text)]
    assert len(slots)==80 and len(set(slots))==80 and max(slots)<90
    offsets=[(slot%6)*84+(slot//6)*4 for slot in slots]
    # Physical geometry for key-reactive effects, from the same matrix the Studio draws.
    names=re.findall(r'\{"([^"]+)",\s*\d+\}',text)
    matrix=[int(x,16) if x.startswith('0x') else int(x) for x in re.findall(r'0x[0-9a-fA-F]+|\d+',text[text.index('matrix[]'):])]
    assert len(matrix)==192,len(matrix)
    kx=[0]*80;ky=[0]*80
    for k in range(80):
        i=matrix.index(k);row,col=divmod(i,32)
        width=6.6 if names[k]=='Space' else 1.8
        kx[k]=int(col*10+width*5);ky[k]=row*20+10
    slot_key=[255]*90
    for k,slot in enumerate(slots):slot_key[slot]=k
    def table(kind,name,values):
        return f'static const {kind} {name}[{len(values)}] = {{'+','.join(map(str,values))+'};\n'
    (HERE/'offsets.h').write_text(table('unsigned short','led_offsets',offsets)+table('unsigned short','key_x10',kx)
        +table('unsigned short','key_y10',ky)+table('unsigned char','slot_key',slot_key)
        +table('unsigned char','key_mat',[(slot//6)*8+slot%6 for slot in slots]))
    gcc=shutil.which('arm-none-eabi-gcc')
    if not gcc:raise SystemExit('Arm GNU toolchain missing')
    tool=Path(gcc).parent
    elf=HERE/'aw60.elf'
    subprocess.run([gcc,'-mcpu=cortex-m0','-mthumb','-Os','-ffreestanding','-fno-builtin',
        '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
        '-Wall','-Wextra','-Werror','-nostdlib','-Wl,--build-id=none',
        '-T',str(HERE/'patch.ld'),str(HERE/'stream.c'),'-lgcc','-o',str(elf)],check=True)
    binary=HERE/'aw60-code.bin'
    subprocess.run([str(tool/'arm-none-eabi-objcopy.exe'),'-O','binary',str(elf),str(binary)],check=True)
    symbols={}
    for line in subprocess.check_output([str(tool/'arm-none-eabi-nm.exe'),str(elf)],text=True).splitlines():
        fields=line.split()
        if len(fields)==3:symbols[fields[2]]=int(fields[0],16)&~1
    code=binary.read_bytes()
    off=PATCH-BASE
    window=0
    while off+window<len(data) and data[off+window]==0:window+=1
    assert len(code)<=window,f'Patch {len(code)} bytes exceeds zero window {window} bytes'
    assert data[off:off+len(code)]==bytes(len(code))
    patched=bytearray(data);patched[off:off+len(code)]=code
    sites=[(a,0x01022ac2,'dispatcher_hook') for a in [0x01015cde,0x010237c0]]+[(a,0x0102006c,'scan_copy_hook') for a in COPY_CALLS]+[(0x01015d04,0x01015cb4,'poll_hook'),(KEY_EVENT_CALL,KEY_EVENT_ORIG,'key_event_hook')]
    changes=[]
    for site,old,name in sites:
        i=site-BASE
        assert data[i:i+4]==bl(site,old),f'Unexpected instructions at {site:x}'
        patched[i:i+4]=bl(site,symbols[name])
        changes.append(dict(address=hex(site),file_offset=hex(i),old=data[i:i+4].hex(),new=patched[i:i+4].hex(),target=name))
    for irq,(stock,name) in (VECTORS.items() if STAGE>=3 else ()):
        i=VECTOR_TABLE-BASE+4*irq
        assert struct.unpack_from('<I',data,i)[0]==stock,f'Unexpected IRQ{irq} vector'
        struct.pack_into('<I',patched,i,symbols[name]|1)
        changes.append(dict(address=hex(VECTOR_TABLE+4*irq),file_offset=hex(i),old=data[i:i+4].hex(),new=patched[i:i+4].hex(),target=name))
    i=DIVIDER-BASE
    assert data[i:i+8]==DIVIDER_PROLOGUE,'Unexpected divider prologue'
    if STAGE>=2:
        # ldr r3,[pc,#0] ; bx r3 ; .word fast_udivmod (r2/r3 are scratch in the stock divider too)
        patched[i:i+8]=bytes.fromhex('004b1847')+struct.pack('<I',symbols['fast_udivmod']|1)
        changes.append(dict(address=hex(DIVIDER),file_offset=hex(i),old=data[i:i+8].hex(),new=patched[i:i+8].hex(),target='fast_udivmod'))
    assert data[:65536]==patched[:65536]
    path=HERE/IMAGE_NAME;path.write_bytes(patched)
    manifest=dict(stock_boot_prefix_sha256=hashlib.sha256(data[:65536]).hexdigest(),stock_sha256=SHA,patched_sha256=hashlib.sha256(patched).hexdigest(),
        file_size=len(patched),code_address=PATCH,code_bytes=len(code),code_window=window,symbols=symbols,
        hooks=changes,slots=slots,offsets=offsets)
    (HERE/MANIFEST_NAME).write_text(json.dumps(manifest,indent=2))
    print(json.dumps({k:v for k,v in manifest.items() if k not in ['slots','offsets','symbols']},indent=2))

if __name__=='__main__':main()
