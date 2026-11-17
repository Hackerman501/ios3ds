/* ios3ds kernel shim: linked at its kernel VA (PA 0x20840000 via physmap) */
OUTPUT_ARCH(arm)
ENTRY(_shim_base)
SECTIONS
{
    . = 0x80840000;
    .text : { *(.text) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss* COMMON) }
    __shim_end = .;
    ASSERT(__shim_end <= 0x80845F00, "shim code overlaps the probe snapshot (+0x5F00) / block mailbox (+0x6000)")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
