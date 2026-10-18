/* SPDX-License-Identifier: TBD */
/* x86 link-check stub: no asm entry, so the ELF entry is hypervisor_main. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(hypervisor_main)

SECTIONS
{
    . = 0x100000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.note.* .comment .eh_frame) }
}
