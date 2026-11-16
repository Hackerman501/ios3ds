# ios3ds-gl: OpenGL ES 1.1 engine for iOS 4.2.1 (replaces MBXGLEngine) and
# its test program. See docs/gpu.md.
#   make            -> build/rootfs-add/.../OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine
#                      build/rootfs-add/usr/libexec/ios3ds-gltest
#   make host-test  -> the same engine core + gltest built for the build host,
#                      run offscreen; writes build/gl-host/gltest.ppm
#   make slots CACHE=<dyld_shared_cache_armv6>   regenerate gl_slots.h
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
ADD  := $(ROOT)/build/rootfs-add
ENGINE := $(ADD)/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine
GLTEST := $(ADD)/usr/libexec/ios3ds-gltest
HOSTDIR := $(ROOT)/build/gl-host

CORE  := api.c draw.c raster.c
HDRS  := engine.h gl_defs.h gl_slots.h
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -mfpu=vfp

all: $(ENGINE) $(GLTEST)

$(ENGINE): $(CORE) iface.c thunks.S $(HDRS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -bundle -fvisibility=hidden $(CORE) iface.c thunks.S \
	    -framework CoreFoundation -o $@

$(GLTEST): test/gltest.c test/glapi.h gl_defs.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) test/gltest.c -framework OpenGLES -lobjc -o $@

host-test: $(HOSTDIR)/gltest
	$(HOSTDIR)/gltest $(HOSTDIR)/gltest.ppm

$(HOSTDIR)/gltest: $(CORE) test/host_gl.c test/gltest.c test/glapi.h $(HDRS)
	mkdir -p $(dir $@)
	cc -DGL_HOST -O2 -g -Wall -Wno-unused-result -Wno-array-bounds -Wno-maybe-uninitialized \
	    $(CORE) test/host_gl.c test/gltest.c -lm -o $@

slots:
	python3 genslots.py $(CACHE) -o gl_slots.h

clean:
	rm -f $(ENGINE) $(GLTEST)
	rm -rf $(HOSTDIR)
.PHONY: all host-test slots clean
