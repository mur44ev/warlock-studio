// SPDX-License-Identifier: GPL-2.0-or-later
// GP75 919 v104 only. RAM overlay; no startup, LED scanner or flash-write changes.
#include <stdint.h>
#include <stddef.h>
#include "../../PresetFormat.h"
#include "offsets.h"

void *memcpy(void *dst,const void *src,size_t size) {
    volatile uint8_t *d=dst;volatile const uint8_t *s=src;
    for(size_t i=0;i<size;++i) d[i]=s[i];
    return dst;
}
void *memset(void *dst,int value,size_t size) {
    volatile uint8_t *d=dst;
    for(size_t i=0;i<size;++i) d[i]=(uint8_t)value;
    return dst;
}

typedef struct { uint8_t r,g,b; } Color;

#define MAGIC 0x36305741u
// 0x21b18 is STOCK profile/flash staging, also consumed by wireless keymap
// sync. Sharing it corrupts the persistent layout (including Enter and Fn).
// Keep every RGB bank/state byte in our reserved high SRAM instead.
#define SCRATCH ((volatile uint8_t *)0x0002cd00u)
#define OUTPUT ((volatile uint8_t *)0x00021681u)
#define STATE ((volatile State *)0x0002cee0u)
#define AUTO ((volatile Auto *)(0x2cd00u+256u))
#define STOCK_LIGHT ((volatile uint8_t *)0x000219d8u)
#define STOCK_EFFECT (STOCK_LIGHT[26])
#define STOCK_LIGHT_DIRTY (*(volatile uint8_t *)0x0002047au)
#define PRESET_FLASH ((const uint8_t *)AW60_PRESET_FLASH_BASE)
#define PRESET_NONE 0xffu

typedef struct {
    uint32_t magic, inverse, frames;
    uint16_t errors, ticks, crc, sequence;
    uint16_t expected[2], bank_sequence[2];
    uint8_t next_page, active, write_bank, ready;
} State;

typedef struct {
    uint32_t host_tick;
    uint16_t generation;
    uint8_t mode,speed,brightness,fps;
    uint8_t scale,amount,flags,current_slot;
    uint8_t preset_count,preset_index,native_effect,reserved;
    uint8_t r1,g1,b1,r2,g2,b2,r3,g3,b3;
    uint8_t slots[AW60_PRESET_SLOT_COUNT];
} Auto;

// Everything below lives in SRAM the stock firmware never touches (its highest referenced data is
// near 0x234ac and the stack grows down from 0x30000): 0x2c000 key state, 0x2c400 frame in flight,
// 0x2cc00 performance counters.
#define KS_BASE 0x0002c000u
#define FRAME_BASE 0x0002c400u
#define PERF_BASE 0x0002cc00u
#define KS ((volatile KeyState *)KS_BASE)
#define FRAME ((Frame *)FRAME_BASE)
#define PERF ((volatile Perf *)PERF_BASE)
#define KS_MAGIC 0x3159454bu
#define PERF_MAGIC 0x50524633u
#define RING 12
#define KEYS 80
#define HEAT_STEP 64u
#define SLICE_KEYS 8u
typedef struct { uint32_t t; uint8_t key,seed,pad0,pad1; } Press;
typedef struct {
    uint32_t magic, inverse, now, presses;
    uint16_t epoch;
    uint8_t head, ok;
    Press ring[RING];
    uint32_t last[KEYS];
    uint8_t heat[KEYS];
} KeyState;
typedef struct { int16_t x,y; uint16_t a,b,c,d; Color tint; uint8_t pad; } Hit;
typedef struct { int16_t x,y; uint16_t fade; Color tint; uint8_t pad; } Spark;
// One autonomous frame is rendered a few keys per call so the main loop is never held for long.
typedef struct {
    Auto a;
    Color c1,c2,c3;
    uint8_t nhits,nsparks;
    uint16_t p[4];
    Hit hits[RING];
    Spark sparks[RING*6];
    uint32_t next_words[127];
    volatile uint32_t due,cyc_last,rem,phase;
    volatile uint16_t epoch;
    volatile uint8_t busy,next_key;
} Frame;
typedef struct { uint32_t max,count,sum16; } IsrStat;
typedef struct {
    uint32_t magic,last_poll,gap_max,polls,slice_max,slices,frames,conv_max,conv_count,scans;
    uint16_t hist[8];
    IsrStat isr[4];
} Perf;
#define MATRIX_STATE ((volatile const uint8_t *)0x00020f42u) // stock matrix sample, one byte per column

_Static_assert(sizeof(State) <= 32, "State must fit reserved scratch tail");
_Static_assert(480+sizeof(State)<=512, "Two frames plus state must fit private scratch");
_Static_assert(sizeof(Auto)<=224 && sizeof(Auto)%4==0, "Autonomous state must remain inside bank1");
_Static_assert(sizeof(KeyState)<=0x400, "KeyState overflows its window");
_Static_assert(sizeof(Frame)<=0x800, "Frame overflows its window");
_Static_assert(sizeof(Perf)<=0x100, "Perf overflows its window");

static uint8_t tri8(unsigned phase) {
    phase&=255u;
    return (uint8_t)(phase<128u?phase*2u:(255u-phase)*2u);
}
static uint8_t soft8(unsigned phase) {
    unsigned t=tri8(phase);
    return (uint8_t)((t*t+255u)>>8);
}
static uint8_t hash8(uint32_t x) {
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
    return (uint8_t)(x>>24);
}
static int valid_state(void) {
    return STATE->magic == MAGIC && STATE->inverse == ~MAGIC;
}
// Pressed state comes straight from the stock matrix sample (clear bit = pressed), so a key
// released while no reactive effect was running can never stay "held".
static int key_held(unsigned key) {
    unsigned m=key_mat[key];
    return !((MATRIX_STATE[m>>3]>>(m&7u))&1u);
}
static int ks_valid(void) {
    return KS->magic == KS_MAGIC && KS->inverse == ~KS_MAGIC && KS->ok;
}
static void ks_init(void) {
    volatile uint32_t *w=(volatile uint32_t *)KS_BASE;
    for(unsigned i=0;i<(PERF_BASE+sizeof(Perf)-KS_BASE)/4u;++i) w[i]=0;
    KS->magic=KS_MAGIC;KS->inverse=~KS_MAGIC;KS->now=1000;
    // Prove the region is real writable SRAM; effects fall back to doing nothing otherwise.
    KS->presses=0x5aa5c33cu;
    KS->ok=(KS->presses==0x5aa5c33cu && KS->magic==KS_MAGIC);
    KS->presses=0;
    PERF->magic=PERF_MAGIC;
}
static void ks_reset_events(void) {
    // Everything time-based restarts.
    volatile uint32_t *w=(volatile uint32_t *)&KS->ring[0];
    for(unsigned i=0;i<(sizeof(KS->ring)+sizeof(KS->last)+sizeof(KS->heat))/4u;++i) w[i]=0;
    KS->head=0;KS->now=1000;
}
static void reset_auto(void) {
    volatile uint8_t *p=(volatile uint8_t *)AUTO;
    for(unsigned i=0;i<sizeof(Auto);++i) p[i]=0;
    AUTO->current_slot=PRESET_NONE;
    AUTO->preset_index=PRESET_NONE;
    AUTO->native_effect=STOCK_EFFECT;
}
static void init_state(void) {
    volatile uint8_t *p=(volatile uint8_t *)STATE;
    for(unsigned i=0;i<sizeof(State);++i) p[i]=0;
    reset_auto();
    STATE->inverse=~MAGIC;
    STATE->magic=MAGIC;
    ks_init();
}
static uint16_t crc16(unsigned bank) {
    // CRC16-CCITT (init FFFF, poly 1021), one byte per step without a table.
    uint16_t crc=0xffff;
    for(unsigned i=0;i<240;++i) {
        unsigned x=((crc>>8)^SCRATCH[bank*240u+i])&255u;
        x^=x>>4;
        crc=(uint16_t)((crc<<8)^(x<<12)^(x<<5)^x);
    }
    return crc;
}
static void commit_rgb(volatile const uint8_t *rgb,unsigned bank,unsigned generation) {
    uint32_t storage[127];
    volatile uint8_t *next=(volatile uint8_t *)storage+1;
    for(unsigned i=0;i<127;++i) storage[i]=0;
    for(unsigned key=0;key<80;++key) {
        unsigned offset=led_offsets[key];
        next[offset+1]=(uint8_t)(((unsigned)rgb[key*3u]*7u*6554u)>>16); // floor(x*70/100)
        next[offset+2]=rgb[key*3u+1u];
        next[offset+3]=rgb[key*3u+2u];
    }
    uint32_t irq;
    __asm volatile("mrs %0, primask\n cpsid i":"=r"(irq)::"memory");
    if(!valid_state() || (bank==2u?(STATE->active!=2u || AUTO->generation!=generation):!(STATE->ready&(1u<<bank)))) {
        __asm volatile("msr primask, %0"::"r"(irq):"memory");
        return;
    }
    OUTPUT[0]=next[0];OUTPUT[1]=next[1];OUTPUT[2]=next[2];
    for(unsigned i=0;i<125;++i) ((volatile uint32_t *)(OUTPUT+3))[i]=storage[i+1];
    OUTPUT[503]=next[503];
    STATE->frames++;
    if(STATE->active!=2u) STATE->active=1;
    __asm volatile("msr primask, %0"::"r"(irq):"memory");
}
static void page(volatile uint8_t *p) {
    if(!valid_state()) init_state();
    if(p[1]==0) { STATE->active=0;STATE->ready=0;STATE->next_page=0xff;return; }
    if(STATE->active==2) {STATE->active=0;STATE->ready=0;STATE->write_bank=0;}
    uint16_t seq=(uint16_t)(p[2]|((uint16_t)p[3]<<8));
    unsigned n=p[4], length=n==4?16:56;
    if(p[1]!=1 || n>=5 || p[5]!=5 || p[6]!=length) goto bad;
    unsigned bank=STATE->write_bank;
    if(STATE->ready&(1u<<bank)) goto bad;
    if(n==0) {STATE->bank_sequence[bank]=seq;STATE->next_page=0;}
    if(STATE->bank_sequence[bank]!=seq || STATE->next_page!=n) goto bad;
    for(unsigned i=0;i<length;++i) SCRATCH[bank*240u+n*56u+i]=p[8+i];
    STATE->next_page=(uint8_t)(n+1u);
    if(n==4) {
        STATE->expected[bank]=(uint16_t)(p[24]|((uint16_t)p[25]<<8));
        STATE->ready&=(uint8_t)~(0x10u<<bank);
        STATE->ready|=(uint8_t)(1u<<bank);
        STATE->write_bank^=1u;
        STATE->next_page=0xff;
    }
    return;
bad:
    STATE->errors++;
    STATE->next_page=0xff;
}
static void effect_page(volatile uint8_t *p) {
    if(!valid_state()) init_state();
    if(STATE->active==2) {STATE->active=0;STATE->ready=0;STATE->write_bank=0;}
    unsigned bank=STATE->write_bank;
    if(p[1]>5 || (STATE->ready&(1u<<bank))) {STATE->errors++;return;}
    for(unsigned i=0;i<11;++i) SCRATCH[bank*240u+i]=p[i];
    STATE->bank_sequence[bank]=(uint16_t)(p[2]|((uint16_t)p[3]<<8));
    STATE->ready|=(uint8_t)((1u<<bank)|(0x10u<<bank));
    STATE->write_bank^=1u;STATE->next_page=0xff;
}
static unsigned core_cycles_ms(void) {
    return (192000000u/((*(volatile uint32_t *)0xf8560&15u)+2u))/1000u;
}
// Free-running CPU cycle counter from the stock 100 ms tick plus the SysTick down-counter. It wraps
// every ~44 s, so only differences of nearby samples are meaningful (and they are exact).
static uint32_t cycle_counter(void) {
    unsigned t,v,reload=*(volatile uint32_t *)0xe000e014;
    do {t=*(volatile uint32_t *)0x20718;v=*(volatile uint32_t *)0xe000e018;}
    while(t!=*(volatile uint32_t *)0x20718);
    if(v>reload) v=reload;
    return t*(reload+1u)+(reload-v);
}
static unsigned div255(unsigned x) {
    return (x+1u+(x>>8))>>8; // exact floor(x/255) for 0..65534; Cortex-M0 has no divider
}
static Color blend(Color a,Color b,unsigned t) {
    Color out;
    unsigned inv=255u-t;
    out.r=(uint8_t)div255((unsigned)a.r*inv+(unsigned)b.r*t);
    out.g=(uint8_t)div255((unsigned)a.g*inv+(unsigned)b.g*t);
    out.b=(uint8_t)div255((unsigned)a.b*inv+(unsigned)b.b*t);
    return out;
}
static Color palette3(Color a,Color b,Color c,unsigned t) {
    t&=255u;
    if(t<85u) return blend(a,b,t*3u);
    if(t<170u) return blend(b,c,(t-85u)*3u);
    return blend(c,a,(t-170u)*3u);
}
static Color wheel(uint8_t hue) {
    unsigned h=(unsigned)hue*6u,sector=h>>8,f=h&255u;
    Color out;
    switch(sector) {
    case 0:out=(Color){255,(uint8_t)f,0};break;
    case 1:out=(Color){(uint8_t)(255u-f),255,0};break;
    case 2:out=(Color){0,255,(uint8_t)f};break;
    case 3:out=(Color){0,(uint8_t)(255u-f),255};break;
    case 4:out=(Color){(uint8_t)f,0,255};break;
    default:out=(Color){255,0,(uint8_t)(255u-f)};break;
    }
    return out;
}
static void ensure_palette(volatile Auto *a) {
    // Legacy clients sent only colour A; derive the rest. New clients flag an explicit
    // palette so black stays a valid colour (e.g. dark background, unlit trail).
    if(!(a->flags&AW60_FLAG_EXPLICIT_PALETTE)) {
        if(!(a->r1|a->g1|a->b1)) {a->r1=0;a->g1=170;a->b1=255;}
        if(!(a->r2|a->g2|a->b2)) {a->r2=a->g1;a->g2=a->b1;a->b2=a->r1?a->r1:180;}
        if(!(a->r3|a->g3|a->b3)) {a->r3=a->b1?a->b1:255;a->g3=a->r1;a->b3=a->g1?a->g1:96;}
    }
    if(a->speed<1 || a->speed>100) a->speed=35;
    if(a->fps<1 || a->fps>60) a->fps=60;
    if(a->scale<1 || a->scale>100) a->scale=50;
    if(a->amount<1 || a->amount>100) a->amount=60;
    if(a->current_slot>=AW60_PRESET_SLOT_COUNT) a->current_slot=PRESET_NONE;
    if(a->preset_index>=AW60_PRESET_SLOT_COUNT) a->preset_index=PRESET_NONE;
}
static void start_auto(unsigned generation) {
    if(ks_valid()) ks_reset_events(); else ks_init();
    uint32_t cyc=cycle_counter();
    Frame *f=FRAME;
    f->busy=0;f->next_key=0;f->due=cyc;f->cyc_last=cyc;f->rem=0;f->phase=0;
    KS->epoch=(uint16_t)(KS->epoch+1u);
    AUTO->generation=(uint16_t)(generation?generation:1u);
    STATE->active=2;
}
static int preset_valid(const uint8_t *p) {
    return p[0]=='A' && p[1]=='W' && p[2]=='P' && p[3]=='5' &&
           p[4]==AW60_PRESET_VERSION && p[AW60_PRESET_MODE_OFFSET]<AW60_EFFECT_COUNT;
}
static void scan_presets(void) {
    AUTO->preset_count=0;
    for(unsigned slot=0;slot<AW60_PRESET_SLOT_COUNT;++slot) {
        const uint8_t *p=PRESET_FLASH+slot*AW60_PRESET_FLASH_STRIDE;
        if(preset_valid(p) && AUTO->preset_count<AW60_PRESET_SLOT_COUNT)
            AUTO->slots[AUTO->preset_count++]=(uint8_t)slot;
    }
    AUTO->preset_index=PRESET_NONE;
    if(AUTO->current_slot!=PRESET_NONE)
        for(unsigned i=0;i<AUTO->preset_count;++i)
            if(AUTO->slots[i]==AUTO->current_slot) {AUTO->preset_index=(uint8_t)i;break;}
}
static void load_preset_bytes(volatile Auto *a,const uint8_t *p) {
    a->mode=p[AW60_PRESET_MODE_OFFSET];
    a->speed=p[AW60_PRESET_SPEED_OFFSET];
    a->brightness=p[AW60_PRESET_BRIGHTNESS_OFFSET];
    a->fps=p[AW60_PRESET_FPS_OFFSET];
    a->scale=p[AW60_PRESET_SCALE_OFFSET];
    a->amount=p[AW60_PRESET_AMOUNT_OFFSET];
    a->flags=p[AW60_PRESET_FLAGS_OFFSET];
    a->r1=p[AW60_PRESET_COLOR1_OFFSET+0];a->g1=p[AW60_PRESET_COLOR1_OFFSET+1];a->b1=p[AW60_PRESET_COLOR1_OFFSET+2];
    a->r2=p[AW60_PRESET_COLOR2_OFFSET+0];a->g2=p[AW60_PRESET_COLOR2_OFFSET+1];a->b2=p[AW60_PRESET_COLOR2_OFFSET+2];
    a->r3=p[AW60_PRESET_COLOR3_OFFSET+0];a->g3=p[AW60_PRESET_COLOR3_OFFSET+1];a->b3=p[AW60_PRESET_COLOR3_OFFSET+2];
    ensure_palette(a);
}
static int activate_preset(unsigned slot,unsigned index) {
    const uint8_t *p=PRESET_FLASH+slot*AW60_PRESET_FLASH_STRIDE;
    if(!preset_valid(p)) return 0;
    STATE->active=0;STATE->ready=0;STATE->write_bank=0;
    load_preset_bytes(AUTO,p);
    AUTO->current_slot=(uint8_t)slot;
    AUTO->preset_index=(uint8_t)index;
    AUTO->generation=(uint16_t)(AUTO->generation+1u);
    start_auto(AUTO->generation);
    return 1;
}
static void leave_auto(void) {
    STATE->active=0;STATE->ready=0;STATE->write_bank=0;
}
// 1 = a saved preset is now running, 2 = cycle wrapped past the last preset so the stock
// effect is shown once (otherwise native effects could never be reached again), 0 = no presets.
static int cycle_preset(void) {
    scan_presets();
    unsigned count=AUTO->preset_count;
    if(!count) return 0;
    unsigned next=AUTO->preset_index<count?AUTO->preset_index+1u:0u;
    if(next>=count) {
        AUTO->current_slot=PRESET_NONE;AUTO->preset_index=PRESET_NONE;
        if(STATE->active) leave_auto();
        return 2;
    }
    return activate_preset(AUTO->slots[next],next)?1:0;
}
static void service_native_presets(void) {
    if(!valid_state()) init_state();
    uint8_t effect=STOCK_EFFECT;
    if(effect==AUTO->native_effect) return;
    AUTO->native_effect=effect;
    // A host command (Studio's hardware-effect mode, profile switch, ...) that changed the
    // stock effect itself must win; only the keyboard's own shortcut walks the presets.
    uint32_t tick=*(volatile uint32_t *)0x20718;
    if(AUTO->host_tick && (uint32_t)(tick-(AUTO->host_tick-1u))<=15u) return;
    int step=cycle_preset();
    if(step==1) {STOCK_LIGHT_DIRTY=0;return;}
    if(step==0 && STATE->active) leave_auto();
}
static void generate_legacy_effect(volatile const uint8_t *params,volatile uint8_t *rgb) {
    unsigned mode=params[1],phase=params[5],brightness=params[6];
    unsigned red=params[8],green=params[9],blue=params[10];
    for(unsigned key=0;key<80;++key) {
        unsigned offset=led_offsets[key],row=offset/84u,col=(offset%84u)/4u;
        unsigned r=red,g=green,b=blue,v=255u;
        if(mode==0 || mode==2) {
            Color c=wheel((uint8_t)(phase+col*(mode==0?12u:8u)+(mode==2?row*10u:0u)));
            r=c.r;g=c.g;b=c.b;
        } else if(mode==1) v=20u+tri8(phase+col*18u)*235u/254u;
        else if(mode==3) v=tri8(phase);
        else if(mode==4) {
            unsigned spark=tri8(phase*(1u+key%3u)+key*97u);
            v=spark*spark/255u;v=v*v/255u;
        } else {
            int delta=(int)(col*18u)-(int)tri8(phase);
            if(delta<0) delta=-delta;
            v=delta<54?255u-(unsigned)delta*255u/54u:0u;
        }
        r=r*v/255u;g=g*v/255u;b=b*v/255u;
        rgb[key*3u]=(uint8_t)(r*brightness/255u);
        rgb[key*3u+1u]=(uint8_t)(g*brightness/255u);
        rgb[key*3u+2u]=(uint8_t)(b*brightness/255u);
    }
}
// ---- Autonomous effects, one key at a time -----------------------------------------------------
// Per-frame constants are prepared once (frame_begin); the per-key functions are pure, so the frame
// can be rendered in slices of SLICE_KEYS keys between main-loop iterations.
static unsigned udiff(unsigned a,unsigned b) { return a>b?a-b:b-a; }
static unsigned dist10(unsigned dx,unsigned dy) {
    unsigned hi=dx>dy?dx:dy,lo=dx>dy?dy:dx;
    return hi+((lo*13u)>>5);
}
static Color scale_color(Color c,unsigned v) {
    v+=1u;
    c.r=(uint8_t)(((unsigned)c.r*v)>>8);c.g=(uint8_t)(((unsigned)c.g*v)>>8);c.b=(uint8_t)(((unsigned)c.b*v)>>8);
    return c;
}
static Color clamp_color(unsigned r,unsigned g,unsigned b) {
    Color c;
    c.r=(uint8_t)(r>255u?255u:r);c.g=(uint8_t)(g>255u?255u:g);c.b=(uint8_t)(b>255u?255u:b);
    return c;
}
static void prepare_auto(Frame *f) {
    f->p[0]=(uint16_t)((f->phase/4800u)&255u);   // t
    f->p[1]=(uint16_t)(3u+f->a.scale/4u);          // spatial
    f->p[2]=(uint16_t)(12u+f->a.amount/2u);        // width
}
static Color auto_pixel(const Frame *f,unsigned key) {
    const Auto *a=&f->a;
    Color c1=f->c1,c2=f->c2,c3=f->c3;
    unsigned t=f->p[0],spatial=f->p[1],width=f->p[2],amount=a->amount;
    uint32_t phase=f->phase;
    unsigned offset=led_offsets[key],row=offset/84u,col=(offset%84u)/4u;
    unsigned sample=0,intensity=255u;
    Color base=c1,accent=c3;
    unsigned x=col*spatial,y=row*(spatial+4u),mirror=col<=10u?col:20u-col;
    switch(a->mode) {
    case 0: base=wheel((uint8_t)(t+col*(4u+a->scale/10u)+row*3u));break;
    case 1:
        sample=(x+t)&255u;base=palette3(c1,c2,c3,sample);
        intensity=64u+div255(soft8(t*2u+col*(8u+spatial))*191u);break;
    case 2:
        sample=(x+y/2u+t/2u)&255u;base=palette3(c1,c2,c3,sample);
        intensity=192u+div255(soft8(sample+t/3u)*63u);break;
    case 3:
        base=blend(c1,c2,soft8(t*2u));
        accent=palette3(c1,c2,c3,(t+row*21u)&255u);
        intensity=48u+div255(soft8(t*2u)*207u);
        base=blend(base,accent,soft8(t+col*9u)/3u);break;
    case 4: {
        sample=(x/2u+t/3u)&255u;base=palette3(c1,c2,c3,sample);
        uint8_t spark=hash8(key*71u+(phase/1024u)*29u+row*13u+col*7u);
        unsigned threshold=200u-(amount*3u/2u);
        if(spark>threshold) base=blend(base,c3,(spark-threshold)*255u/(255u-threshold));
        intensity=72u+div255(soft8(sample+t)*183u);break; }
    case 5: {
        sample=(row*20u+t/4u)&255u;base=palette3(c1,c2,c3,sample);
        int delta=(int)(col*16u)-(int)tri8(t*2u);
        if(delta<0) delta=-delta;
        intensity=delta<(int)width?255u-(unsigned)delta*255u/width:24u;break; }
    case 6:
        sample=(x/2u+soft8(t+row*17u)/2u+y/3u)&255u;base=palette3(c1,c2,c3,sample);
        intensity=96u+div255(soft8(t/2u+row*27u+col*11u)*159u);break;
    case 7: {
        unsigned s1=soft8(t+x*3u+y),s2=soft8(255u-t/2u+y*2u);
        sample=(s1/2u+s2/2u+x)&255u;base=palette3(c1,c2,c3,sample);
        intensity=112u+div255(soft8(sample+t/3u)*143u);break; }
    case 8:
        sample=(row*28u+t/2u+soft8(col*17u+t)/3u)&255u;base=palette3(c1,c2,c3,sample);
        intensity=184u+div255(soft8(t+row*33u)*71u);break;
    case 9: {
        unsigned dx=col>10u?col-10u:10u-col,dy=row>2u?row-2u:2u-row;
        unsigned dist=dx*(spatial+2u)+dy*(spatial+10u);
        sample=(dist+t)&255u;base=palette3(c1,c2,c3,sample);
        intensity=72u+div255(soft8(t+dist/2u)*183u);break; }
    case 10: {
        sample=(255u-row*34u+t)&255u;base=blend(c1,c2,sample);
        uint8_t spark=hash8(key*91u+(phase/768u)+row*19u);
        unsigned threshold=225u-(amount/2u);
        if(spark>threshold) base=blend(base,c3,(spark-threshold)*255u/(255u-threshold));
        intensity=80u+div255(soft8(sample+t/2u)*175u);break; }
    case 11: {
        unsigned diag=col*18u+row*26u,head=(t*2u)&511u;
        int delta=(int)diag-(int)head;if(delta<0) delta=-delta;
        sample=(diag+t)&255u;base=palette3(c1,c2,c3,sample);
        intensity=delta<(int)(width+18u)?255u-(unsigned)delta*255u/(width+18u):0u;break; }
    case 12:
        sample=(mirror*(6u+spatial/2u)+row*11u+t)&255u;base=palette3(c1,c2,c3,sample);
        intensity=136u+div255(soft8(t+mirror*23u)*119u);break;
    default:
        sample=(soft8(x+t/2u)+soft8(y*2u-t))/2u;base=palette3(c1,c2,c3,sample);
        intensity=96u+div255(soft8(sample+t/3u)*(96u+amount));break;
    }
    if(intensity>255u) intensity=255u; // Velvet Wave exceeded 255 for depth > 63 and wrapped to dark
    base.r=(uint8_t)div255((unsigned)base.r*intensity);
    base.g=(uint8_t)div255((unsigned)base.g*intensity);
    base.b=(uint8_t)div255((unsigned)base.b*intensity);
    return base;
}

// ---- Key-reactive effects (modes 14..18) -------------------------------------------------
// Presses arrive from the stock matrix scanner through key_event_hook. Geometry is in tenths of
// a half-key (key pitch 20 in both axes) so distances are plain integer maths: no sqrt, and
// per-hit constants are computed once per frame instead of per key.
static unsigned fade_q8(uint32_t age,unsigned life) {
    return age>=life?0u:(unsigned)(((life-age)<<8)/life);
}
static Color bg_pixel(Color base,unsigned key,uint32_t now,unsigned level) {
    unsigned shimmer=soft8((now>>4)+((unsigned)key_x10[key]>>2));
    return scale_color(base,(level*(150u+((shimmer*105u)>>8)))>>8);
}
static unsigned heat_mul(unsigned scale) {
    return (255u<<16)/(4000u+scale*400u); // fully cooled after 4 s (small) .. 44 s (large)
}
static unsigned heat_level(unsigned key,uint32_t now,unsigned mul) {
    uint32_t last=KS->last[key];
    if(!last) return 0;
    uint32_t age=now-last;
    if(age>60000u) age=60000u;
    unsigned drop=(unsigned)((age*mul)>>16),heat=KS->heat[key];
    return heat>drop?heat-drop:0u;
}
static const int8_t spark_dx[8]={64,45,0,-45,-64,-45,0,45};
static const int8_t spark_dy[8]={0,45,64,45,0,-45,-64,-45};
static void prepare_reactive(Frame *f,uint32_t now) {
    const Auto *a=&f->a;
    unsigned mode=a->mode,scale=a->scale;
    f->nhits=0;f->nsparks=0;
    f->p[0]=(uint16_t)(8u+(unsigned)a->amount*56u/100u);   // background level
    if(mode==14) {f->p[1]=(uint16_t)(250u+scale*20u);return;}
    if(mode==17) {f->p[1]=(uint16_t)heat_mul(scale);return;}
    unsigned life=mode==15?1500u:mode==16?900u:1100u;
    unsigned velocity=8u+(unsigned)a->speed/2u,width=24u+scale,trail=70u+scale*2u,radius=26u+scale/4u;
    f->p[1]=(uint16_t)(mode==15?width:mode==16?trail:radius);
    f->p[2]=(uint16_t)((255u<<8)/f->p[1]);
    for(unsigned i=0;i<RING;++i) {
        uint32_t t=KS->ring[i].t;
        unsigned key=KS->ring[i].key,seed=KS->ring[i].seed;
        if(!t || key>=KEYS) continue;
        uint32_t age32=now-t;
        if(age32>=life) continue;
        unsigned age=(unsigned)age32;
        Hit *h=&f->hits[f->nhits++];
        h->x=(int16_t)key_x10[key];h->y=(int16_t)key_y10[key];
        h->a=h->b=h->c=h->d=0;
        if(mode==15) {
            unsigned r=(age*velocity)>>6;
            h->a=(uint16_t)r;h->b=(uint16_t)fade_q8(age,life);h->d=(uint16_t)(r+width);
            h->c=(uint16_t)(age<140u?((140u-age)*467u)>>8:0u);
            h->tint=palette3(f->c1,f->c2,f->c3,(r>>2)+seed);
        } else if(mode==16) {
            h->a=(uint16_t)((age*(20u+a->speed))>>6);h->b=(uint16_t)fade_q8(age,life);
        } else {
            h->c=(uint16_t)(age<120u?((120u-age)*545u)>>8:0u);
            unsigned fade=fade_q8(age,life);
            int fall=(int)((age*age)>>14);
            for(unsigned j=0;j<6;++j) {
                uint8_t r=hash8((uint32_t)seed*131u+j*977u+key);
                unsigned dir=(r>>5)&7u,speed=7u+((unsigned)a->speed>>3)+(r&7u);
                Spark *s=&f->sparks[f->nsparks++];
                s->x=(int16_t)((int)key_x10[key]+(((int)spark_dx[dir]*(int)age*(int)speed)>>12));
                s->y=(int16_t)((int)key_y10[key]+(((int)spark_dy[dir]*(int)age*(int)speed)>>12)+fall);
                s->fade=(uint16_t)fade;
                s->tint=palette3(f->c1,f->c2,f->c3,(unsigned)r*3u+j*43u);
            }
        }
    }
}
static Color reactive_key(const Frame *f,unsigned key,uint32_t now) {
    Color c1=f->c1,c2=f->c2,c3=f->c3;
    Color bg=bg_pixel(c1,key,now,f->p[0]);
    unsigned mode=f->a.mode,kx=key_x10[key],ky=key_y10[key];
    if(mode==14) {
        unsigned fade=f->p[1],lit=0;
        if(key_held(key)) lit=255u;
        else if(KS->last[key]) {
            uint32_t age=now-KS->last[key];
            if(age<fade) {lit=(unsigned)(((fade-age)<<8)/fade);if(lit>255u) lit=255u;lit=(lit*lit)>>8;}
        }
        return lit?blend(bg,blend(c2,c3,lit),lit):bg;
    }
    if(mode==17) {
        unsigned heat=heat_level(key,now,f->p[1]);
        if(key_held(key) && heat<220u) heat=220u;
        if(!heat) return bg;
        Color hot=heat<128u?blend(c1,c2,heat*2u):blend(c2,c3,(heat-128u)*2u);
        return blend(bg,hot,heat);
    }
    unsigned r=bg.r,g=bg.g,b=bg.b;
    if(mode==15) {
        unsigned width=f->p[1],winv=f->p[2];
        for(unsigned i=0;i<f->nhits;++i) {
            const Hit *h=&f->hits[i];
            unsigned dx=udiff(kx,(unsigned)h->x);
            if(dx>=h->d && dx>=22u) continue; // outside the ring and the key-press flash
            unsigned d=dist10(dx,udiff(ky,(unsigned)h->y)),dd=udiff(d,h->a),ring=0;
            if(dd<width) ring=((width-dd)*winv)>>8;
            Color tint=h->tint;
            if(h->c && d<22u) {if(h->c>ring) ring=h->c;tint=c3;}
            if(!ring) continue;
            unsigned v=((ring*h->b)>>8)+1u;
            r+=((unsigned)tint.r*v)>>8;g+=((unsigned)tint.g*v)>>8;b+=((unsigned)tint.b*v)>>8;
        }
    } else if(mode==16) {
        unsigned trail=f->p[1],tinv=f->p[2];
        for(unsigned i=0;i<f->nhits;++i) {
            const Hit *h=&f->hits[i];
            unsigned dx=udiff(kx,(unsigned)h->x),dy=udiff(ky,(unsigned)h->y),best=0;
            if(dy>10u && dx>14u) continue;
            for(unsigned beam=0;beam<2;++beam) {
                unsigned along=beam?dy:dx,across=beam?dx:dy;
                if(across>(beam?14u:10u) || along>h->a || h->a-along>=trail) continue;
                unsigned v=((trail-(h->a-along))*tinv)>>8;
                if(v>best) best=v;
            }
            if(!best) continue;
            Color tint=blend(c2,c3,best);
            unsigned v=((best*h->b)>>8)+1u;
            r+=((unsigned)tint.r*v)>>8;g+=((unsigned)tint.g*v)>>8;b+=((unsigned)tint.b*v)>>8;
        }
    } else {
        int reach=(int)f->p[1],ik=(int)kx,jk=(int)ky;
        for(unsigned i=0;i<f->nsparks;++i) {
            const Spark *s=&f->sparks[i];
            int sx=ik-s->x;
            if(sx>=reach || sx<=-reach) continue;
            int sy=jk-s->y;
            if(sy>=reach || sy<=-reach) continue;
            unsigned d=dist10((unsigned)(sx<0?-sx:sx),(unsigned)(sy<0?-sy:sy));
            if(d>=(unsigned)reach) continue;
            unsigned v=((((((unsigned)reach-d)*f->p[2])>>8)*s->fade)>>8)+1u;
            r+=((unsigned)s->tint.r*v)>>8;g+=((unsigned)s->tint.g*v)>>8;b+=((unsigned)s->tint.b*v)>>8;
        }
        for(unsigned i=0;i<f->nhits;++i) {
            const Hit *h=&f->hits[i];
            if(!h->c) continue;
            if(dist10(udiff(kx,(unsigned)h->x),udiff(ky,(unsigned)h->y))<24u) {
                unsigned v=(unsigned)h->c+1u;
                r+=((unsigned)c3.r*v)>>8;g+=((unsigned)c3.g*v)>>8;b+=((unsigned)c3.b*v)>>8;
            }
        }
    }
    return clamp_color(r,g,b);
}
static void record_key(unsigned row,unsigned col,unsigned pressed) {
    unsigned key=slot_key[col*6u+row];
    if(key>=KEYS) return;
    if(!pressed) return;
    uint32_t now=KS->now;
    unsigned heat=heat_level(key,now,heat_mul(AUTO->scale?AUTO->scale:50u))+HEAT_STEP;
    KS->heat[key]=(uint8_t)(heat>255u?255u:heat);
    KS->last[key]=now;
    unsigned head=KS->head;
    KS->ring[head].key=(uint8_t)key;
    KS->ring[head].seed=hash8(now*2654435761u+key*31u+KS->presses);
    KS->ring[head].t=now;
    KS->head=(uint8_t)(head+1u>=RING?0u:head+1u);
    KS->presses++;
}
// Called by the stock matrix scanner for every debounced edge: row 0..5, col 0..14, pressed 0/1.
void key_event_hook(unsigned row,unsigned col,unsigned pressed) {
    if(row<6u && col<15u && valid_state() && STATE->active==2 && ks_valid()) record_key(row,col,pressed);
    ((void (*)(unsigned,unsigned,unsigned))0x0101f507u)(row,col,pressed); // stock handler keeps running
}

static void autonomous(volatile uint8_t *p) {
    if(!valid_state()) init_state();
    if(p[1]==255) {STATE->active=1;STATE->ready=0;return;}
    if(!ks_valid()) {STATE->errors++;return;}
    if(p[1]>=AW60_EFFECT_COUNT || p[2]<1 || p[2]>100 || p[4]<1 || p[4]>60) {STATE->errors++;return;}
    STATE->active=0;STATE->ready=0;STATE->write_bank=0;
    AUTO->mode=p[1];AUTO->speed=p[2];AUTO->brightness=p[3];AUTO->fps=p[4];
    AUTO->r1=p[5];AUTO->g1=p[6];AUTO->b1=p[8];
    AUTO->scale=p[11];AUTO->amount=p[12];AUTO->flags=p[19];
    AUTO->r2=p[13];AUTO->g2=p[14];AUTO->b2=p[15];
    AUTO->r3=p[16];AUTO->g3=p[17];AUTO->b3=p[18];
    AUTO->current_slot=PRESET_NONE;AUTO->preset_index=PRESET_NONE;
    AUTO->generation=(uint16_t)(p[9]|((uint16_t)p[10]<<8));
    scan_presets();ensure_palette(AUTO);start_auto(AUTO->generation);
}
// ---- Performance counters (read with the ED command) -------------------------------------------
static void perf_hist(uint32_t gap) {
    static const uint32_t edge[7]={9600,24000,48000,96000,192000,480000,960000}; // 0.1 .. 10 ms at 96 MHz
    unsigned i=0;
    while(i<7 && gap>=edge[i]) ++i;
    if(PERF->hist[i]!=0xffffu) PERF->hist[i]++;
}
// Called at the top of both hooks (main-loop poll and LED-scan converter sites): counts each kind and
// records the interval between any two consecutive entries, i.e. how often rendering gets a chance to run.
static void perf_enter(int scan) {
    if(PERF->magic!=PERF_MAGIC) return;
    uint32_t cyc=cycle_counter(),last=PERF->last_poll,gap=cyc-last;
    PERF->last_poll=cyc;
    if(scan) PERF->scans++; else PERF->polls++;
    if(!last || gap>=0x08000000u) return;
    if(gap>PERF->gap_max) PERF->gap_max=gap;
    perf_hist(gap);
}
static void perf_slice(uint32_t cycles,int frame_done) {
    if(PERF->magic!=PERF_MAGIC) return;
    PERF->slices++;
    if(cycles>PERF->slice_max) PERF->slice_max=cycles;
    if(frame_done) PERF->frames++;
}
static void perf_isr(unsigned index,uint32_t cycles) {
    if(PERF->magic!=PERF_MAGIC || cycles>=0x08000000u) return;
    volatile IsrStat *s=&PERF->isr[index];
    s->count++;
    s->sum16+=cycles>>4;
    if(cycles>s->max) s->max=cycles;
}

// ---- Autonomous frame pipeline ---------------------------------------------------------------
static int frame_begin(Frame *f,uint32_t cyc) {
    uint32_t irq;
    __asm volatile("mrs %0, primask\n cpsid i":"=r"(irq)::"memory");
    int ok=valid_state() && STATE->active==2;
    if(ok) {
        volatile uint32_t *src=(volatile uint32_t *)AUTO;
        uint32_t *dst=(uint32_t *)&f->a;
        for(unsigned i=0;i<sizeof(Auto)/4u;++i) dst[i]=src[i];
        f->epoch=KS->epoch;
    }
    __asm volatile("msr primask, %0"::"r"(irq):"memory");
    if(!ok) return 0;
    int32_t dt=(int32_t)(cyc-f->cyc_last);
    // The coarse stock tick and the SysTick down-counter are sampled separately; if the tick ISR is
    // still pending when the counter wraps, time appears to step back by 100 ms.
    if(dt<0) return 0;
    unsigned cycles_ms=core_cycles_ms(),total=(unsigned)dt+f->rem,dms=total/cycles_ms;
    f->rem=total-dms*cycles_ms;
    f->cyc_last=cyc;
    KS->now+=dms;
    f->phase=(f->phase+dms*(f->a.speed*5u+30u))%1228800u;
    unsigned frame_cycles=cycles_ms*1000u/f->a.fps;
    uint32_t next=f->due+frame_cycles;
    if((int32_t)(cyc-next)>=0) next=cyc+frame_cycles; // fell behind: do not burst to catch up
    f->due=next;
    f->c1.r=f->a.r1;f->c1.g=f->a.g1;f->c1.b=f->a.b1;
    f->c2.r=f->a.r2;f->c2.g=f->a.g2;f->c2.b=f->a.b2;
    f->c3.r=f->a.r3;f->c3.g=f->a.g3;f->c3.b=f->a.b3;
    if(f->a.mode>=AW60_FIRST_REACTIVE) prepare_reactive(f,KS->now); else prepare_auto(f);
    f->next_key=0;f->busy=1;
    return 1;
}
static void pack_key(Frame *f,unsigned key,Color c) {
    unsigned b=f->a.brightness;
    volatile uint8_t *next=(volatile uint8_t *)f->next_words+1;
    unsigned off=led_offsets[key];
    next[off+1]=(uint8_t)((div255((unsigned)c.r*b)*7u*6554u)>>16); // stock red calibration, floor(x*70/100)
    next[off+2]=(uint8_t)div255((unsigned)c.g*b);
    next[off+3]=(uint8_t)div255((unsigned)c.b*b);
}
static void publish_frame(Frame *f) {
    uint32_t irq;
    __asm volatile("mrs %0, primask\n cpsid i":"=r"(irq)::"memory");
    if(valid_state() && STATE->active==2 && KS->epoch==f->epoch) {
        volatile uint8_t *next=(volatile uint8_t *)f->next_words+1;
        OUTPUT[0]=next[0];OUTPUT[1]=next[1];OUTPUT[2]=next[2];
        volatile uint32_t *dst=(volatile uint32_t *)(OUTPUT+3);
        const uint32_t *src=f->next_words+1;
        for(unsigned i=0;i<125;++i) *dst++=*src++;   // pointer walk: shortest possible masked section
        OUTPUT[503]=next[503];
        STATE->frames++;
    }
    __asm volatile("msr primask, %0"::"r"(irq):"memory");
}
static void service_auto(void) {
    if(!ks_valid()) return;
    Frame *f=FRAME;
    uint32_t cyc=cycle_counter();
    if(!f->busy) {
        if((int32_t)(cyc-f->due)<0) return;            // idle path: about 40 instructions
        if(!frame_begin(f,cyc)) return;
    }
    // On the keyboard the hooks that drive us run only ~60..100 times a second (the key scan itself is
    // an interrupt, not main-loop work), so the whole frame is rendered per call. It is still built in
    // SLICE_KEYS steps so an effect change or reset arriving mid-frame aborts it immediately.
    uint32_t now=KS->now;
    for(unsigned k0=f->next_key;k0<KEYS;k0+=SLICE_KEYS) {
        if(STATE->active!=2 || f->epoch!=KS->epoch) {f->busy=0;return;}
        unsigned k1=k0+SLICE_KEYS;
        if(k1>KEYS) k1=KEYS;
        for(unsigned key=k0;key<k1;++key)
            pack_key(f,key,f->a.mode>=AW60_FIRST_REACTIVE?reactive_key(f,key,now):auto_pixel(f,key));
        f->next_key=(uint8_t)k1;
    }
    publish_frame(f);
    f->busy=0;
    perf_slice(cycle_counter()-cyc,1);
}
void service_pending(void) {
    uint32_t exception;
    __asm volatile("mrs %0, ipsr":"=r"(exception));
    if(exception || !valid_state()) return;
    if(STATE->active==2) {service_auto();return;}
    uint32_t irq;
    __asm volatile("mrs %0, primask\n cpsid i":"=r"(irq)::"memory");
    unsigned ready=STATE->ready,producer=STATE->write_bank;
    __asm volatile("msr primask, %0"::"r"(irq):"memory");
    unsigned queued=ready&3u;
    if(!queued) return;
    unsigned bank=queued==3u?producer:((queued&1u)?0u:1u);
    unsigned procedural=ready&(0x10u<<bank);
    unsigned sequence=STATE->bank_sequence[bank];
    uint8_t generated[240];
    if(procedural) generate_legacy_effect(SCRATCH+bank*240u,generated);
    uint16_t crc=procedural?0u:crc16(bank);
    if(valid_state() && STATE->active!=2 && (STATE->ready&(1u<<bank)) && STATE->bank_sequence[bank]==sequence) {
        STATE->crc=crc;
        STATE->sequence=STATE->bank_sequence[bank];
        if(procedural || crc==STATE->expected[bank]) commit_rgb(procedural?generated:SCRATCH+bank*240u,bank,0);
        else STATE->errors++;
        __asm volatile("mrs %0, primask\n cpsid i":"=r"(irq)::"memory");
        STATE->ready&=(uint8_t)~((1u<<bank)|(0x10u<<bank));
        __asm volatile("msr primask, %0"::"r"(irq):"memory");
    }
}
void poll_hook(void) {
    perf_enter(0);
    service_native_presets();
    service_pending();
    ((void (*)(void))0x01015cb5u)();
}
static void finish_reply(volatile uint8_t *p) {
    unsigned sum=0;for(unsigned i=0;i<7;++i) sum+=p[i];p[7]=(uint8_t)(255u-sum);
    *(volatile uint32_t *)0x204a4=0;
    *(volatile uint32_t *)0x204a0=64;
}
static void status(volatile uint8_t *p) {
    uint32_t frames=valid_state()?STATE->frames:0;
    uint16_t errors=valid_state()?STATE->errors:0;
    uint16_t ticks=valid_state()?STATE->ticks:0;
    uint16_t seq=valid_state()?STATE->sequence:0;
    uint16_t crc=valid_state()?STATE->crc:0;
    unsigned active=valid_state()?STATE->active:0;
    for(unsigned i=0;i<64;++i) p[i]=0;
    p[0]=0xe9;p[1]='A';p[2]='W';p[3]='6';p[4]='0';p[5]=1;p[6]=(uint8_t)active;
    for(unsigned i=0;i<4;++i) p[8+i]=(uint8_t)(frames>>(i*8));
    p[12]=(uint8_t)errors;p[13]=(uint8_t)(errors>>8);
    p[14]=(uint8_t)ticks;p[15]=(uint8_t)(ticks>>8);
    p[16]=(uint8_t)crc;p[17]=(uint8_t)(crc>>8);
    p[18]=(uint8_t)seq;p[19]=(uint8_t)(seq>>8);
    p[20]=80;p[21]=5;
    p[22]=valid_state()?(STATE->ready&3u):0;p[23]=9;
    uint32_t tick=*(volatile uint32_t *)0x20718;
    for(unsigned i=0;i<4;++i) p[24+i]=(uint8_t)(tick>>(i*8));
    if(valid_state()) {p[43]=AUTO->preset_count;p[44]=AUTO->current_slot;p[45]=AUTO->preset_index;p[46]=AUTO->native_effect;p[47]=STOCK_EFFECT;}
    p[48]=PRESET_FLASH[0];p[49]=PRESET_FLASH[1];p[50]=PRESET_FLASH[2];p[51]=PRESET_FLASH[3];
    p[52]=PRESET_FLASH[0x100];p[53]=PRESET_FLASH[0x101];p[54]=PRESET_FLASH[0x102];p[55]=PRESET_FLASH[0x103];
    if(active==2) {
        p[28]=AUTO->mode;p[29]=AUTO->speed;p[30]=AUTO->brightness;p[31]=AUTO->fps;
        p[32]=AUTO->r1;p[33]=AUTO->g1;p[34]=AUTO->b1;
        p[35]=AUTO->scale;p[36]=AUTO->amount;
        p[37]=AUTO->r2;p[38]=AUTO->g2;p[39]=AUTO->b2;
        p[40]=AUTO->r3;p[41]=AUTO->g3;p[42]=AUTO->b3;
    }
    p[56]=(uint8_t)ks_valid();
    if(ks_valid()) for(unsigned i=0;i<4;++i) p[57+i]=(uint8_t)(KS->presses>>(i*8));
    finish_reply(p);
}
// EC: live key state for the Studio key monitor. Raw bytes are the stock debounced matrix
// (one byte per column, bit per row), held bits are what the reactive effects see.
static void keys_status(volatile uint8_t *p) {
    for(unsigned i=0;i<64;++i) p[i]=0;
    p[0]=0xec;p[1]='K';p[2]='E';p[3]='Y';p[4]=1;p[5]=(uint8_t)ks_valid();
    p[6]=(uint8_t)(valid_state()?STATE->active:0);
    for(unsigned col=0;col<15;++col) p[8+col]=MATRIX_STATE[col];
    if(ks_valid()) {
        for(unsigned key=0;key<KEYS;++key) if(key_held(key)) p[24+key/8u]|=(uint8_t)(1u<<(key&7u));
        for(unsigned i=0;i<4;++i) p[34+i]=(uint8_t)(KS->presses>>(i*8));
    }
    finish_reply(p);
}
static void put32(volatile uint8_t *p,unsigned off,uint32_t v) {
    for(unsigned i=0;i<4;++i) p[off+i]=(uint8_t)(v>>(i*8));
}
// ED: system and performance report. Request byte 1 selects the page, byte 2 == 1 resets counters.
//   page 0: clock/NVIC facts      page 1: main loop + rendering + converter    page 2: ISR load
static void perf_status(volatile uint8_t *p) {
    unsigned page=p[1],reset=p[2]==1u;
    for(unsigned i=0;i<64;++i) p[i]=0;
    p[0]=0xed;p[1]='P';p[2]='F';p[3]=(uint8_t)page;p[4]=1;p[5]=(uint8_t)(PERF->magic==PERF_MAGIC);
    if(page==0) {
        put32(p,8,*(volatile uint32_t *)0xf8560);       // SYSCTRL HCLK select (hclk = 192 MHz / (sel+2))
        put32(p,12,*(volatile uint32_t *)0xe000e014);   // SysTick reload
        put32(p,16,*(volatile uint32_t *)0xe000e010);   // SysTick control
        put32(p,20,*(volatile uint32_t *)0xe000e400);   // NVIC priority IRQ0..3
        put32(p,24,*(volatile uint32_t *)0xe000e40c);   // IRQ12..15
        put32(p,28,*(volatile uint32_t *)0xe000e410);   // IRQ16..19
        put32(p,32,*(volatile uint32_t *)0xe000e100);   // NVIC enabled set
        put32(p,36,core_cycles_ms());
        put32(p,40,*(volatile uint32_t *)0x20718);
    } else if(page==1) {
        put32(p,8,PERF->gap_max);put32(p,12,PERF->polls);put32(p,16,PERF->slice_max);put32(p,20,PERF->slices);
        put32(p,24,PERF->frames);put32(p,28,PERF->conv_max);put32(p,32,PERF->conv_count);put32(p,36,PERF->scans);
        for(unsigned i=0;i<8;++i) {p[40+i*2]=(uint8_t)PERF->hist[i];p[41+i*2]=(uint8_t)(PERF->hist[i]>>8);}
    } else if(page==2) {
        for(unsigned i=0;i<4;++i) {
            put32(p,8+i*12,PERF->isr[i].max);put32(p,12+i*12,PERF->isr[i].count);put32(p,16+i*12,PERF->isr[i].sum16);
        }
    }
    if(reset && PERF->magic==PERF_MAGIC) {
        volatile uint32_t *w=(volatile uint32_t *)PERF;
        for(unsigned i=1;i<sizeof(Perf)/4u;++i) w[i]=0;
    }
    finish_reply(p);
}
int checksum_hook(volatile uint8_t *p) {
    int ok=((int (*)(volatile uint8_t *))0x01022a6fu)(p);
    if(!ok) return 0;
    if(p[0]==0xe8) {page(p);return 0;}
    if(p[0]==0xea) {effect_page(p);return 0;}
    if(p[0]==0xeb) {autonomous(p);return 0;}
    if(p[0]==0xe9) {status(p);return 1;}
    if(p[0]==0xec) {keys_status(p);return 1;}
    if(p[0]==0xed) {perf_status(p);return 1;}
    if(p[0]<0x80 && valid_state()) {
        leave_auto();
        AUTO->host_tick=*(volatile uint32_t *)0x20718+1u;
        AUTO->current_slot=PRESET_NONE;AUTO->preset_index=PRESET_NONE;
    }
    return ok;
}
void dispatcher_hook(void) {
    volatile uint8_t *p=(volatile uint8_t *)0x21ad8u;
    unsigned command=p[0];
    int ok=checksum_hook(p);
    if(command>=0xe8 && command<=0xed) {
        *(volatile uint8_t *)0x2048d=((command==0xe9 || command==0xec || command==0xed) && ok)?1:0;
        return;
    }
    ((void (*)(void))0x01022ac3u)();
}
// Stock work->scan converter (0x0102006c): all 7 rows x 21 columns, red scaled to 70 %, state byte
// untouched. The stock loop spends ~52000 instructions per call in its bitwise divider; this is the
// same result in about 1900.
void fast_convert(void) {
    volatile const uint8_t *w=(volatile const uint8_t *)0x213e1u;
    volatile uint8_t *o=OUTPUT;
    for(unsigned i=0;i<147u;++i) {
        o[i*4u+1u]=(uint8_t)(((unsigned)w[i*4u+1u]*7u*6554u)>>16);
        o[i*4u+2u]=w[i*4u+2u];
        o[i*4u+3u]=w[i*4u+3u];
    }
}
void scan_copy_hook(void) {
    if(!valid_state()) init_state();
    perf_enter(1);
    service_native_presets();
    service_pending();
    if(valid_state() && STATE->active) {STATE->ticks++;return;}
    if(valid_state()) STATE->active=0;
    uint32_t t0=cycle_counter();
    fast_convert();
    uint32_t dt=cycle_counter()-t0;
    if(PERF->magic==PERF_MAGIC) {
        PERF->conv_count++;
        if(dt>PERF->conv_max) PERF->conv_max=dt;
    }
}

// ISR timing wrappers: the flash vector table entries for the four busy IRQs point here (see
// build_patch.py). Each measures wall time, including anything that preempts it.
#define IRQ_WRAP(NAME,ORIG,INDEX) void NAME(void) { \
    uint32_t t0=cycle_counter(); \
    ((void (*)(void))(ORIG))(); \
    perf_isr(INDEX,cycle_counter()-t0); }
IRQ_WRAP(irq0_wrap,0x0102366fu,0)    // USB
IRQ_WRAP(irq14_wrap,0x0101e93du,1)   // timer 1
IRQ_WRAP(irq15_wrap,0x0101e9d5u,2)   // timer 2: matrix scan, key events, encoder, report building
IRQ_WRAP(irq16_wrap,0x0101eb67u,3)   // timer 3

// Replacement for the stock unsigned divide at 0x01029a6c (bit-serial, 268..460 instructions for
// every call, 123 call sites). Same contract: r0/r1 = quotient/remainder, divide by zero gives
// q = 0xffffffff, r = dividend. Reached through a 2-instruction trampoline written by build_patch.py.
// A 64-bit return value comes back in r0 (low word = quotient) and r1 (high word = remainder), which
// is exactly the stock register contract; a two-word struct would be returned through memory instead.
unsigned long long fast_udivmod(unsigned a,unsigned b) {
    if(!b) return 0xffffffffull|((unsigned long long)a<<32);
    unsigned q=a/b;
    return (unsigned long long)q|((unsigned long long)(a-q*b)<<32);
}
