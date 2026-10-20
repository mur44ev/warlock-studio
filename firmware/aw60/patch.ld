ENTRY(checksum_hook)
SECTIONS {
    . = 0x01034000;
    .text : { *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.comment*) *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) }
}
