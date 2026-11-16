/* ios3ds GL engine: one 12-byte stub per __GLIFunctionDispatchRec slot.
 * Every slot the engine does not implement points here; the stub reports
 * its own slot number once (gl_unimpl) and returns 0.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gl_slots.h"
    .text
    .arm
    .align 2
    .globl _gl_unimpl_thunks
_gl_unimpl_thunks:
    .set slot, 0
    .rept GLI_NSLOTS
    mov r0, #(slot & 0xff)
    add r0, r0, #(slot & 0x300)
    b _gl_unimpl
    .set slot, slot + 1
    .endr
