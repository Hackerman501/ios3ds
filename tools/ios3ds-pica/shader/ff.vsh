; ios3ds fixed-function vertex shader for the PICA200.
;
; The GPU has no fixed-function transform stage, so OpenGL ES 1.1's is this
; program. Lighting is not done here: the GL engine lights on the CPU and
; passes the result as the vertex colour.
;
; Inputs    v0 position (x, y, z[, w]; missing components read 0, 0, 0, 1)
;           v1 colour
;           v2 texture coordinate 0 (s, t)
;           v3 texture coordinate 1 (s, t)
; Uniforms  c0-c3  rows of the clip matrix (projection * modelview, depth
;                  already remapped to the GPU's -w..0)
;           c4,c5  rows s and t of texture matrix 0
;           c6,c7  rows s and t of texture matrix 1
;           c8     colour scale (1/255 when v1 is unsigned bytes, else 1)
;           c9     (0, 0, 0, 1)
;
; Assemble with tools/ios3ds-pica/picasm.py (the syntax is a subset of
; picasso's, so picasso assembles this file to the same words).
; SPDX-License-Identifier: GPL-3.0-or-later

.out opos   position
.out ocolor color
.out otc0   texcoord0
.out otc1   texcoord1

.proc main
    dp4 opos.x, c0, v0
    dp4 opos.y, c1, v0
    dp4 opos.z, c2, v0
    dp4 opos.w, c3, v0

    mul ocolor, c8, v1

    dp4 otc0.x, c4, v2
    dp4 otc0.y, c5, v2
    mov otc0.zw, c9

    dp4 otc1.x, c6, v3
    dp4 otc1.y, c7, v3
    mov otc1.zw, c9

    end
.end
