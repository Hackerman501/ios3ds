/*
 * ios3ds shim: calls from the main shim into its GPU part (kernel/shim/
 * pica.c and friends, linked at GPU_PART_VA and loaded from gpu.bin by the
 * ARM9, see boot/common/gpu_mirror.h). If gpu.bin was not loaded (an old
 * boot.cfg, a missing file) the part's header magic is absent and nothing
 * is called: no GPU service, no mirror, everything else unaffected.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "gpu_mirror.h"

void shim_pica_register(void);
void shim_gpu_thread_poll(void);
void shim_gpu_irq(uint32_t id);

static int gpu_part(void)
{
    return *(vu32 *)GPU_PART_VA == GPU_PART_MAGIC;
}

/* thread context: register the ios3ds_gpu sysctl (blk.c, first md0 request) */
void shim_gpu_register(void)
{
    if (gpu_part())
        shim_pica_register();
}

/* thread context: the input daemon's sysctl (tools/ios3ds-input/shim/input.c) */
void shim_gpu_poll(void)
{
    if (gpu_part())
        shim_gpu_thread_poll();
}

/* IRQ mode, private stack (entry.S): timer and VBlank interrupts */
void shim_mcu_irq_hook(uint32_t id);   /* tools/ios3ds-mcu/shim/mcu.c: VBlank off while the screen is */

void shim_gpu_irq_hook(uint32_t id)
{
    shim_mcu_irq_hook(id);
    if (gpu_part())
        shim_gpu_irq(id);
}
