OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
/* Separate ELF permissions; the bare-metal loader still consumes the .bin. */
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x40200000;
    .text : { KEEP(*(.text.start)) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss (NOLOAD) : {
        . = ALIGN(16); __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16); vspi_stack0 = .; . += 0x4000;
        vspi_stack1 = .; . += 0x4000;
        __bss_end = .;
    } :data
}
