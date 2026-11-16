/* ios3ds ARM11 stub in AXI WRAM. Must stay clear of 0x1FFF0000 (mailbox),
   0x1FFFF000+ (Luma params / boot11 vectors / core entry words). */
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { AXIWRAM (rwx) : ORIGIN = 0x1FF90000, LENGTH = 0x00040000 }
SECTIONS
{
    .text : ALIGN(4) { *(.text.start) *(.text*) } > AXIWRAM
    .rodata : ALIGN(4) { *(.rodata*) } > AXIWRAM
    .data : ALIGN(4) { *(.data*) } > AXIWRAM
    . = ALIGN(4);
    __image_end = .;
    .bss (NOLOAD) : ALIGN(4) { __bss_start = .; *(.bss* COMMON); . = ALIGN(4); __bss_end = .; } > AXIWRAM
    . = ALIGN(8);
    . += 0x2000;
    __exc_stack_top = .;
    . += 0x4000;
    __svc_stack_top = .;
    ASSERT(__svc_stack_top <= 0x1FFF0000, "ARM11 image overlaps the mailbox")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
