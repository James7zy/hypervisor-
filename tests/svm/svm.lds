OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
SECTIONS {
    . = 0x40200000;
    .text : { *(.text*) }
    .data : { *(.data*) }
    .bss  : { *(.bss*)  }
}
