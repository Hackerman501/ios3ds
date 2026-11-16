/* gltest: exercises the ios3ds GL engine through the public OpenGL ES 1.1
 * API and checks the pixels it gets back.
 *
 *   gltest [out.ppm]                     host build: offscreen render + checks
 *   gltest [delay_s [seconds [out.ppm]]] iOS: EAGLContext offscreen render +
 *                                        checks, then after delay_s draws the
 *                                        animated scene straight into the main
 *                                        display's IOSurface for `seconds`
 *
 * The scene: smooth-shaded triangle, checkerboard texture (GL_LINEAR,
 * MODULATE), a 50% alpha-blended quad, a scissored clear, glDrawTexiOES, and
 * a lit, depth-tested cube under a perspective projection.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "glapi.h"

#ifndef GL_TEXTURE_CROP_RECT_OES
#define GL_TEXTURE_CROP_RECT_OES 0x8B9D
#endif

static int W = 320, H = 480;
static GLuint tex;
static int fails;

static void make_texture(void)
{
    static uint32_t px[64 * 64];
    int x, y;
    GLint crop[4] = {0, 0, 64, 64};
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)            /* RGBA bytes: white / orange 8x8 cells */
            px[y * 64 + x] = ((x >> 3) ^ (y >> 3)) & 1 ? 0xff2080ffu : 0xffffffffu;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);     /* for glDrawTexiOES */
}

static void cube(float t)
{
    static const GLfloat v[] = {
        -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,     /* +z */
         1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,     /* -z */
         1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,     /* +x */
        -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,     /* -x */
        -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,     /* +y */
        -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,     /* -y */
    };
    static const GLfloat fn[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const GLfloat fc[6][4] = {{1,.3f,.3f,1},{.3f,1,.3f,1},{.3f,.3f,1,1},{1,1,.3f,1},{.3f,1,1,1},{1,.3f,1,1}};
    static GLfloat n[72], c[96];
    static const GLubyte idx[36] = {0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11, 12,13,14,12,14,15,
                                    16,17,18,16,18,19, 20,21,22,20,22,23};
    static const GLfloat lpos[4] = {1, 2, 3, 0}, amb[4] = {.25f, .25f, .25f, 1}, dif[4] = {1, 1, 1, 1};
    int f, k;
    for (f = 0; f < 6; f++)
        for (k = 0; k < 4; k++) {
            memcpy(n + (f * 4 + k) * 3, fn[f], 12);
            memcpy(c + (f * 4 + k) * 4, fc[f], 16);
        }
    glViewport(W / 8, H / 32, W * 3 / 4, H * 3 / 8);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustumf(-1, 1, -1, 1, 2, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -5);
    glRotatef(30 + t * 40, 1, 0, 0);
    glRotatef(40 + t * 60, 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glNormalPointer(GL_FLOAT, 0, n);
    glColorPointer(4, GL_FLOAT, 0, c);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, idx);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}

/* Layout in GL coordinates (origin bottom left), for a W x H target. */
#define SX(x) ((float)(x) * W / 320)
#define SY(y) ((float)(y) * H / 480)

static void draw_scene(float t)
{
    GLfloat tri[6] = {SX(20), SY(300), SX(150), SY(300), SX(85), SY(440)};
    static const GLfloat tricol[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    GLfloat quad[8] = {SX(170), SY(300), SX(300), SY(300), SX(170), SY(430), SX(300), SY(430)};
    static const GLfloat uv[8] = {0, 0, 2, 0, 0, 2, 2, 2};
    GLfloat blend[8] = {SX(230), SY(250), SX(310), SY(250), SX(230), SY(350), SX(310), SY(350)};

    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.1f, 0.12f, 0.25f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glShadeModel(GL_SMOOTH);

    /* smooth-shaded triangle, spinning about its centroid */
    glPushMatrix();
    glTranslatef(SX(85), SY(346.67f), 0);
    glRotatef(t * 90, 0, 0, 1);
    glTranslatef(-SX(85), -SY(346.67f), 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, tri);
    glColorPointer(4, GL_FLOAT, 0, tricol);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_COLOR_ARRAY);
    glPopMatrix();

    /* textured quad (2x2 repeats of the checkerboard) */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quad);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    /* glDrawTexiOES: the texture, unscaled, bottom left */
    glDrawTexiOES((int)SX(8), (int)SY(200), 0, 64, 64);
    glDisable(GL_TEXTURE_2D);

    /* 50% red, blended */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f);
    glVertexPointer(2, GL_FLOAT, 0, blend);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);

    /* scissored clear: a yellow bar */
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)SX(90), (int)SY(250), (int)SX(120), (int)SY(30));
    glClearColor(1, 0.85f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    cube(t);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static int near(const GLubyte *p, int r, int g, int b, int tol)
{
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

static void check(const char *what, float x, float y, int r, int g, int b, int tol, int invert)
{
    GLubyte p[4];
    int ok;
    glReadPixels((int)x, (int)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    ok = near(p, r, g, b, tol) != invert;
    printf("gltest: %-28s (%3d,%3d) = %3d %3d %3d %s\n", what, (int)x, (int)y, p[0], p[1], p[2], ok ? "ok" : "FAIL");
    fails += !ok;
}

static void run_checks(void)
{
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    GLenum e;
    printf("gltest: GL_VERSION \"%s\", GL_RENDERER \"%s\"\n", ver ? ver : "?", ren ? ren : "?");
    check("clear colour", SX(160), SY(470), 26, 31, 64, 2, 0);
    check("triangle centroid (grey)", SX(85), SY(347), 85, 85, 85, 12, 0);
    check("triangle red corner", SX(24), SY(302), 255, 0, 0, 40, 0);
    check("texture: white cell", SX(170) + 4, SY(300) + 4, 255, 255, 255, 8, 0);
    check("texture: orange cell", SX(170) + 12 * W / 320.f, SY(300) + 4, 255, 128, 32, 12, 0);
    check("drawtex: white cell", SX(8) + 3, SY(200) + 3, 255, 255, 255, 8, 0);
    check("blend over background", SX(305), SY(260), 140, 15, 32, 4, 0);
    check("scissored clear", SX(150), SY(265), 255, 217, 0, 2, 0);
    check("outside scissor", SX(150), SY(285), 26, 31, 64, 2, 0);
    check("cube centre is drawn", SX(160), SY(105), 26, 31, 64, 6, 1);
    e = glGetError();
    printf("gltest: glGetError = 0x%x %s\n", e, e ? "FAIL" : "ok");
    fails += e != 0;
}

static void write_ppm(const char *path)
{
    unsigned char *px = malloc((size_t)W * H * 4);
    FILE *f;
    int y, x;
    if (!px || !(f = fopen(path, "wb"))) {
        free(px);
        return;
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = H - 1; y >= 0; y--)
        for (x = 0; x < W; x++)
            fwrite(px + ((size_t)y * W + x) * 4, 1, 3, f);
    fclose(f);
    free(px);
    printf("gltest: wrote %s\n", path);
}

/* An offscreen framebuffer: RGBA8 colour + 16-bit depth renderbuffers. */
static int offscreen(void)
{
    GLuint fb, rb[2];
    GLenum st;
    glGenFramebuffersOES(1, &fb);
    glBindFramebufferOES(GL_FRAMEBUFFER, fb);
    glGenRenderbuffersOES(2, rb);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    st = glCheckFramebufferStatusOES(GL_FRAMEBUFFER);
    printf("gltest: offscreen %dx%d framebuffer status 0x%x\n", W, H, st);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

#ifdef GL_HOST
void host_gl_init(void);

int main(int argc, char **argv)
{
    host_gl_init();
    if (!offscreen())
        return 1;
    make_texture();
    draw_scene(0);
    run_checks();
    if (argc > 1)
        write_ppm(argv[1]);
    printf("gltest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

#else /* iOS */
extern int gettimeofday(struct timeval *, void *);     /* the stub SDK has no <sys/time.h> */

typedef void *id;
typedef void *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
#define MSG(type, obj, sel) ((type)objc_msgSend)(obj, sel_registerName(sel)

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* The main display's framebuffer IOSurface (what QuartzCore composites into). */
static void *screen_surface(void)
{
    void *h = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", 1);
    int (*get_main)(void **) = h ? (int (*)(void **))dlsym(h, "IOMobileFramebufferGetMainDisplay") : NULL;
    int (*get_surf)(void *, int, void **) =
        h ? (int (*)(void *, int, void **))dlsym(h, "IOMobileFramebufferGetLayerDefaultSurface") : NULL;
    void *fb = NULL, *s = NULL;
    int r1 = -1, r2 = -1;
    if (get_main)
        r1 = get_main(&fb);
    if (fb && get_surf)
        r2 = get_surf(fb, 0, &s);
    printf("gltest: main display %p (0x%x), layer 0 surface %p (0x%x)\n", fb, r1, s, r2);
    return s;
}

int main(int argc, char **argv)
{
    int delay = argc > 1 ? atoi(argv[1]) : 0;
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    id cls, ctx;
    void *surf;
    GLuint fb, rb[2];
    GLint w = 0, h = 0;
    double t0, t;
    unsigned frames = 0;

    setvbuf(stdout, NULL, 1 /* _IOLBF */, 0);
    cls = objc_getClass("EAGLContext");
    ctx = MSG(id (*)(id, SEL), cls, "alloc"));
    ctx = MSG(id (*)(id, SEL, int), ctx, "initWithAPI:"), 1);
    printf("gltest: EAGLContext %p\n", ctx);
    if (!ctx)
        return 1;
    MSG(signed char (*)(id, SEL, id), cls, "setCurrentContext:"), ctx);

    if (!offscreen())
        return 1;
    make_texture();
    t0 = now();
    draw_scene(0);
    glFinish();
    printf("gltest: offscreen frame took %.0f ms\n", (now() - t0) * 1000);
    run_checks();
    if (argc > 3)
        write_ppm(argv[3]);
    printf("gltest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);

    if (secs <= 0)
        return fails != 0;
    sleep(delay);
    surf = screen_surface();
    if (!surf)
        return 1;
    glGenFramebuffersOES(1, &fb);
    glBindFramebufferOES(GL_FRAMEBUFFER, fb);
    glGenRenderbuffersOES(2, rb);
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[0]);
    if (!MSG(signed char (*)(id, SEL, GLenum, void *, signed char), ctx, "attachImage:toCoreSurface:invertedRender:"),
             GL_RENDERBUFFER, surf, 1)) {
        printf("gltest: attachImage:toCoreSurface: failed (0x%x)\n", glGetError());
        return 1;
    }
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    W = w;
    H = h;
    glBindRenderbufferOES(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorageOES(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    printf("gltest: drawing into the display surface %dx%d, status 0x%x\n", W, H,
           glCheckFramebufferStatusOES(GL_FRAMEBUFFER));
    t0 = now();
    while ((t = now() - t0) < secs) {
        draw_scene((float)t);
        glFinish();
        if (!(frames++ % 20))
            printf("gltest: frame %u at %.1f s (%.1f fps)\n", frames, t, frames / (t > 0 ? t : 1));
        usleep(50000);
    }
    return 0;
}
#endif
