/* ios3ds ARM9: runs from ARM9 RAM (0x08000000-0x080FFFFF).
   0x08000000-0x0800003F are the exception veneers boot9 jumps to. */
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { ARM9RAM (rwx) : ORIGIN = 0x08000040, LENGTH = 0x000FFFC0 }
SECTIONS
{
    .text : ALIGN(4) { *(.text.start) *(.text*) } > ARM9RAM
    .rodata : ALIGN(4) { *(.rodata*) } > ARM9RAM
    .data : ALIGN(4) { *(.data*) } > ARM9RAM
    . = ALIGN(4);
    __image_end = .;
    .bss (NOLOAD) : ALIGN(4) { __bss_start = .; *(.bss* COMMON); . = ALIGN(4); __bss_end = .; } > ARM9RAM
    __stack_top = 0x08100000;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
