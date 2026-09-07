OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
SECTIONS {
    . = 0x40200000;
    .text : { KEEP(*(.text.start)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : {
        . = ALIGN(16); __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16); vspi_stack0 = .; . += 0x4000;
        vspi_stack1 = .; . += 0x4000;
        __bss_end = .;
    }
}
