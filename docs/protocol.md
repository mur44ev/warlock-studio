# Protocol and memory map (rev9)

Vendor HID is Report ID0 plus64 payload bytes. Most commands use checksum at
payload[7]: `(255-sum(payload[:7]))&255`. Hardware lighting07 has a different header;
use Protocol.h rather than guessing offsets. A write may persist to flash.

| Command | Purpose |
| --- | --- |
| E8 | Full80×RGB frame,5 pages56/56/56/56/16, sequence16 and CRC16-CCITT; subcommand0 yields to stock |
| E9 | AW60 wire version1, implementation revision9, state/counters/parameters |
| EA | Compact single procedural frame with host-supplied phase |
| EB | Autonomous effect0..18, speed/brightness/FPS/palette/size/depth;255 freezes |
| EC | Debounced matrix/held keys and press count |
| ED | System/loop/frame/IRQ performance counters, pages0..2 |

Receiver3151:4011 uses F7 status, F6/0A select, normal packet, F7 reply-ready,
FC release, GET. See radio.py. A receiver transport does not support the same
streaming throughput as cable; use EB for autonomous60FPS.

| Address | Meaning |
| --- | --- |
| 0x01010200 | Stock application entry region (image base0x01000200, app file offset65536) |
| 0x01034000 | Custom code in validated stock zero window |
| 0x213e1 | Stock lighting work buffer |
| 0x21681 | Scan output7×21×4 bytes `[state,R,G,B]`, row stride84; last row indicators |
| 0x21b18..0x21d17 | **Stock layout/flash staging. RGB MUST NOT write here.** |
| 0x2c000 | Reactive key state |
| 0x2c400 | Frame under construction |
| 0x2cc00 | Performance counters |
| 0x2cd00..0x2ceff | Private RGB banks / Auto / State; rev9 fix |

Earlier rev<=8 mistakenly shared stock staging. Stock0x01021282 copies its
16×32 bytes into profile flash0x01040100; this could persist RGB metadata as
key assignments. Rev9 leaves it untouched, with an emulator regression assertion.

CRC/render work runs in thread context. Only final short framebuffer publication
masks IRQs. Stock SysTick has coarse100ms ticks; use LOAD/VAL interpolation, not
a fictitious1ms counter. Effects are local and do not need an open host handle.

Known performance facts are device-specific: core96MHz, LED scan~2.5kHz, matrix
scan500Hz. Do not interpret a USB1ms descriptor as measured end-to-end input latency.
