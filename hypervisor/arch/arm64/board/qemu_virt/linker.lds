/* SPDX-License-Identifier: TBD */
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : ALIGN(4) {
        KEEP(*(.text._start))
        *(.text.vectors)
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 0x4000;        /* 16 KiB boot stack */
    __stack_top = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment*)
        *(.eh_frame*)
    }
}
