/* Host build of gltest: the gl* calls go straight into the engine's
 * dispatch table, exactly as OpenGLES.framework's trampolines do on iOS
 * (GC first, then the GL arguments). Also stubs the iface.c hooks.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../engine.h"
#include "../gl_slots.h"
#include "glapi.h"

static GC *gc;
static void *tab[GLI_NSLOTS];

void host_gl_init(void)
{
    gc = gl_gc_new(gl_share_new());
    gl_install_dispatch(gc, tab);
}

#define STRIP(...) __VA_ARGS__
#define HOST_F(ret, name, params, args) \
    ret name params { return ((ret (*)(GC *, STRIP params))tab[SLOT_##name])(gc, STRIP args); }
#define HOST_F0(ret, name) \
    ret name(void) { return ((ret (*)(GC *))tab[SLOT_##name])(gc); }
GLTEST_API(HOST_F, HOST_F0)

/* iface.c hooks: the host build has no drawables or IOSurfaces */
int drawable_begin(GC *g, Rb *rb) { (void)g; (void)rb; return 0; }
void drawable_detach(Rb *rb) { rb->dr = 0; }
void tex_release_surface(Tex *t) { t->surface = 0; }
int tex_surface_seed(const Tex *t, uint32_t *seed) { (void)t; (void)seed; return 0; }
