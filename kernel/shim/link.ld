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
    ASSERT(__shim_end <= 0x80846000, "shim code overlaps the block mailbox")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
