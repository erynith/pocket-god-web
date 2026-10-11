// android.* framework natives + app driver (Activity lifecycle, GL thread, input)
#include "vm.h"
#include "dex.h"
#include "objs.h"
#include "host.h"

void *buf_addr(Object *o);
StreamObj *stream_from_bytes(Class *c, uint8_t *data, int len, int owned);
void dump_stack(Thread *t);
int thread_begin(Thread *t, Method *m, Reg *args);
void thread_free(Thread *t);
extern Thread **g_threads; extern int g_nthreads;
size_t gc_object_count(void);
extern size_t g_heap_bytes;

/* ---------- app state ---------- */
static Thread *g_main;
static Object *g_activity, *g_renderer, *g_view, *g_gl, *g_sensor_listener, *g_assets, *g_resources;
static Object **g_uiq; static int g_uiq_n, g_uiq_cap;   // runOnUiThread queue
#define MAX_DIALOGS 16
static Object *g_dialogs[MAX_DIALOGS];
static int g_surface_created;
static int g_exited;

void extra_roots(void) {
    gc_mark(g_activity); gc_mark(g_renderer); gc_mark(g_view); gc_mark(g_gl); gc_mark(g_sensor_listener);
    gc_mark(g_assets); gc_mark(g_resources);
    for (int i = 0; i < g_uiq_n; i++) gc_mark(g_uiq[i]);
    for (int i = 0; i < MAX_DIALOGS; i++) gc_mark(g_dialogs[i]);
}

double vm_now_ms(void) { return host_now(); }

static void noop(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void ret_false(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(0); }
static void ret_true(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(1); }

/* ---------- Context / Activity ---------- */
static void Ctx_getAssets(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args;
    if (!g_assets) g_assets = new_object(find_class("Landroid/content/res/AssetManager;"));
    RET_L(g_assets);
}
static void Ctx_getResources(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args;
    if (!g_resources) g_resources = new_object(find_class("Landroid/content/res/Resources;"));
    RET_L(g_resources);
}
static Object *g_sensor_mgr, *g_vibrator;
static void Ctx_getSystemService(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = (JString *)ARG_L(1);
    if (s && jstr_eq_c(s, "sensor")) {
        if (!g_sensor_mgr) { g_sensor_mgr = new_object(find_class("Landroid/hardware/SensorManager;")); gc_add_root(&g_sensor_mgr); }
        RET_L(g_sensor_mgr);
    } else if (s && jstr_eq_c(s, "vibrator")) {
        if (!g_vibrator) { g_vibrator = new_object(find_class("Landroid/os/Vibrator;")); gc_add_root(&g_vibrator); }
        RET_L(g_vibrator);
    } else RET_L(NULL);
}
static void Ctx_openFileInput(Thread *t, Reg *args, Reg *ret) {
    JString *s = (JString *)ARG_L(1); NPE_CHECK(s);
    char *n = jstr_to_utf8(s);
    int sz = host_file_size(n, (int)strlen(n));
    if (sz < 0) { throw_new(t, "Ljava/io/FileNotFoundException;", n); free(n); return; }
    uint8_t *d = malloc(sz + 1);
    host_file_read(n, (int)strlen(n), d);
    free(n);
    RET_L(stream_from_bytes(find_class("Ljava/io/FileInputStream;"), d, sz, 1));
}
static void Ctx_openFileOutput(Thread *t, Reg *args, Reg *ret) {
    JString *s = (JString *)ARG_L(1); NPE_CHECK(s);
    StreamObj *o = (StreamObj *)new_object(find_class("Ljava/io/FileOutputStream;"));
    o->fname = jstr_to_utf8(s);
    RET_L(o);
}
static void Act_runOnUiThread(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    if (g_uiq_n == g_uiq_cap) { g_uiq_cap = g_uiq_cap ? g_uiq_cap * 2 : 16; g_uiq = realloc(g_uiq, sizeof(Object *) * g_uiq_cap); }
    g_uiq[g_uiq_n++] = ARG_L(1);
}
static void Act_setContentView(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; g_view = ARG_L(1); }
static void Act_finish(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    vm_log("Activity.finish()");
    g_exited = 1;
    host_exit(0);
}
static NativeDef Context_methods[] = {
    {"<init>", "()V", noop, 0},
    {"getAssets", "()Landroid/content/res/AssetManager;", Ctx_getAssets, 0},
    {"getResources", "()Landroid/content/res/Resources;", Ctx_getResources, 0},
    {"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", Ctx_getSystemService, 0},
    {"openFileInput", "(Ljava/lang/String;)Ljava/io/FileInputStream;", Ctx_openFileInput, 0},
    {"openFileOutput", "(Ljava/lang/String;I)Ljava/io/FileOutputStream;", Ctx_openFileOutput, 0},
    {0}};
static NativeDef Activity_methods[] = {
    {"<init>", "()V", noop, 0},
    {"onCreate", "(Landroid/os/Bundle;)V", noop, 0},
    {"onStart", "()V", noop, 0},
    {"onResume", "()V", noop, 0},
    {"onPause", "()V", noop, 0},
    {"onStop", "()V", noop, 0},
    {"onDestroy", "()V", noop, 0},
    {"onRestart", "()V", noop, 0},
    {"onBackPressed", "()V", Act_finish, 0},
    {"finish", "()V", Act_finish, 0},
    {"runOnUiThread", "(Ljava/lang/Runnable;)V", Act_runOnUiThread, 0},
    {"setContentView", "(Landroid/view/View;)V", Act_setContentView, 0},
    {"setRequestedOrientation", "(I)V", noop, 0},
    {"getWindow", "()Landroid/view/Window;", ret_false, 0},
    {0}};

/* ---------- View / GLSurfaceView ---------- */
static void GLSV_setRenderer(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; g_renderer = ARG_L(1); if (!g_view) g_view = ARG_L(0); }
static NativeDef View_methods[] = {
    {"<init>", "(Landroid/content/Context;)V", noop, 0},
    {"onTouchEvent", "(Landroid/view/MotionEvent;)Z", ret_false, 0},
    {"setFocusable", "(Z)V", noop, 0},
    {"setFocusableInTouchMode", "(Z)V", noop, 0},
    {"setKeepScreenOn", "(Z)V", noop, 0},
    {0}};
static NativeDef GLSV_methods[] = {
    {"<init>", "(Landroid/content/Context;)V", noop, 0},
    {"setRenderer", "(Landroid/opengl/GLSurfaceView$Renderer;)V", GLSV_setRenderer, 0},
    {"setRenderMode", "(I)V", noop, 0},
    {"onPause", "()V", noop, 0},
    {"onResume", "()V", noop, 0},
    {"requestRender", "()V", noop, 0},
    {"setEGLConfigChooser", "(Z)V", noop, 0},
    {"setEGLConfigChooser", "(IIIIII)V", noop, 0},
    {"setDebugFlags", "(I)V", noop, 0},
    {0}};

/* ---------- GL ---------- */
#define GLFN(name, body) \
    static void GLS_##name(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; Reg *a = args; body; } \
    static void GLI_##name(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; Reg *a = args + 1; body; }
#define AI(n) (a[n].i)
#define AF(n) (a[n].f)
#define AL(n) (a[n].l)
GLFN(BindTexture, gl_BindTexture(AI(0), AI(1)))
GLFN(BlendFunc, gl_BlendFunc(AI(0), AI(1)))
GLFN(Clear, gl_Clear(AI(0)))
GLFN(ClearColor, gl_ClearColor(AF(0), AF(1), AF(2), AF(3)))
GLFN(Color4f, gl_Color4f(AF(0), AF(1), AF(2), AF(3)))
GLFN(Color4ub, gl_Color4f(((uint8_t)AI(0)) / 255.f, ((uint8_t)AI(1)) / 255.f, ((uint8_t)AI(2)) / 255.f, ((uint8_t)AI(3)) / 255.f))
GLFN(DeleteTexturesB, gl_DeleteTextures(AI(0), buf_addr(AL(1))))
GLFN(DeleteTexturesA, gl_DeleteTextures(AI(0), (int32_t *)ARR_DATA(AL(1)) + AI(2)))
GLFN(GenTexturesB, gl_GenTextures(AI(0), buf_addr(AL(1))))
GLFN(GenTexturesA, gl_GenTextures(AI(0), (int32_t *)ARR_DATA(AL(1)) + AI(2)))
GLFN(Enable, gl_Enable(AI(0)))
GLFN(Disable, gl_Disable(AI(0)))
GLFN(EnableClientState, gl_EnableClientState(AI(0)))
GLFN(DisableClientState, gl_DisableClientState(AI(0)))
GLFN(DrawArrays, gl_DrawArrays(AI(0), AI(1), AI(2)))
GLFN(DrawElements, gl_DrawElements(AI(0), AI(1), AI(2), buf_addr(AL(3))))
GLFN(GetIntegervB, gl_GetIntegerv(AI(0), buf_addr(AL(1))))
GLFN(GetIntegervA, gl_GetIntegerv(AI(0), (int32_t *)ARR_DATA(AL(1)) + AI(2)))
GLFN(LoadIdentity, gl_LoadIdentity())
GLFN(LoadMatrixf, gl_LoadMatrixf((float *)ARR_DATA(AL(0)) + AI(1)))
GLFN(MultMatrixf, gl_MultMatrixf((float *)ARR_DATA(AL(0)) + AI(1)))
GLFN(LoadMatrixfB, gl_LoadMatrixf(buf_addr(AL(0))))
GLFN(MultMatrixfB, gl_MultMatrixf(buf_addr(AL(0))))
GLFN(MatrixMode, gl_MatrixMode(AI(0)))
GLFN(Orthof, gl_Orthof(AF(0), AF(1), AF(2), AF(3), AF(4), AF(5)))
GLFN(PushMatrix, gl_PushMatrix())
GLFN(PopMatrix, gl_PopMatrix())
GLFN(Rotatef, gl_Rotatef(AF(0), AF(1), AF(2), AF(3)))
GLFN(Scalef, gl_Scalef(AF(0), AF(1), AF(2)))
GLFN(Translatef, gl_Translatef(AF(0), AF(1), AF(2)))
GLFN(ShadeModel, gl_ShadeModel(AI(0)))
GLFN(TexParameteri, gl_TexParameteri(AI(0), AI(1), AI(2)))
GLFN(TexParameterf, gl_TexParameteri(AI(0), AI(1), (int)AF(2)))
GLFN(TexEnvf, (void)a)
GLFN(TexEnvi, (void)a)
GLFN(Hint, (void)a)
GLFN(DepthMask, (void)a)
GLFN(AlphaFunc, (void)a)
GLFN(TexImage2D, gl_TexImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), AL(8) ? buf_addr(AL(8)) : NULL))
GLFN(TexSubImage2D, gl_TexSubImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), AL(8) ? buf_addr(AL(8)) : NULL))
GLFN(VertexPointer, gl_VertexPointer(AI(0), AI(1), AI(2), buf_addr(AL(3))))
GLFN(TexCoordPointer, gl_TexCoordPointer(AI(0), AI(1), AI(2), buf_addr(AL(3))))
GLFN(ColorPointer, gl_ColorPointer(AI(0), AI(1), AI(2), buf_addr(AL(3))))
GLFN(Viewport, gl_Viewport(AI(0), AI(1), AI(2), AI(3)))
static void GLS_GetError(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(gl_GetError()); }
static void GLI_GetError(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(gl_GetError()); }

#define GLDEF(nm, sig, fn) {nm, sig, GLI_##fn, 0}
#define GLSDEF(nm, sig, fn) {nm, sig, GLS_##fn, 1}
#define GL_TABLE(D) \
    D("glBindTexture", "(II)V", BindTexture), \
    D("glBlendFunc", "(II)V", BlendFunc), \
    D("glClear", "(I)V", Clear), \
    D("glClearColor", "(FFFF)V", ClearColor), \
    D("glColor4f", "(FFFF)V", Color4f), \
    D("glColor4ub", "(BBBB)V", Color4ub), \
    D("glDeleteTextures", "(ILjava/nio/IntBuffer;)V", DeleteTexturesB), \
    D("glDeleteTextures", "(I[II)V", DeleteTexturesA), \
    D("glGenTextures", "(ILjava/nio/IntBuffer;)V", GenTexturesB), \
    D("glGenTextures", "(I[II)V", GenTexturesA), \
    D("glEnable", "(I)V", Enable), \
    D("glDisable", "(I)V", Disable), \
    D("glEnableClientState", "(I)V", EnableClientState), \
    D("glDisableClientState", "(I)V", DisableClientState), \
    D("glDrawArrays", "(III)V", DrawArrays), \
    D("glDrawElements", "(IIILjava/nio/Buffer;)V", DrawElements), \
    D("glGetIntegerv", "(ILjava/nio/IntBuffer;)V", GetIntegervB), \
    D("glGetIntegerv", "(I[II)V", GetIntegervA), \
    D("glGetError", "()I", GetError), \
    D("glLoadIdentity", "()V", LoadIdentity), \
    D("glLoadMatrixf", "([FI)V", LoadMatrixf), \
    D("glMultMatrixf", "([FI)V", MultMatrixf), \
    D("glLoadMatrixf", "(Ljava/nio/FloatBuffer;)V", LoadMatrixfB), \
    D("glMultMatrixf", "(Ljava/nio/FloatBuffer;)V", MultMatrixfB), \
    D("glMatrixMode", "(I)V", MatrixMode), \
    D("glOrthof", "(FFFFFF)V", Orthof), \
    D("glPushMatrix", "()V", PushMatrix), \
    D("glPopMatrix", "()V", PopMatrix), \
    D("glRotatef", "(FFFF)V", Rotatef), \
    D("glScalef", "(FFF)V", Scalef), \
    D("glTranslatef", "(FFF)V", Translatef), \
    D("glShadeModel", "(I)V", ShadeModel), \
    D("glTexParameteri", "(III)V", TexParameteri), \
    D("glTexParameterf", "(IIF)V", TexParameterf), \
    D("glTexParameterx", "(III)V", TexParameteri), \
    D("glTexEnvf", "(IIF)V", TexEnvf), \
    D("glTexEnvx", "(III)V", TexEnvi), \
    D("glHint", "(II)V", Hint), \
    D("glDepthMask", "(Z)V", DepthMask), \
    D("glAlphaFunc", "(IF)V", AlphaFunc), \
    D("glTexImage2D", "(IIIIIIIILjava/nio/Buffer;)V", TexImage2D), \
    D("glTexSubImage2D", "(IIIIIIIILjava/nio/Buffer;)V", TexSubImage2D), \
    D("glVertexPointer", "(IIILjava/nio/Buffer;)V", VertexPointer), \
    D("glTexCoordPointer", "(IIILjava/nio/Buffer;)V", TexCoordPointer), \
    D("glColorPointer", "(IIILjava/nio/Buffer;)V", ColorPointer), \
    D("glViewport", "(IIII)V", Viewport)
static NativeDef GLImpl_methods[] = { GL_TABLE(GLDEF), {0} };
static NativeDef GLES11_methods[] = { GL_TABLE(GLSDEF), {0} };

/* ---------- Bitmap ---------- */
typedef struct BitmapObj { Object h; int handle; int w, hgt; } BitmapObj;
static void Bitmap_fin(Object *o) { BitmapObj *b = (BitmapObj *)o; if (b->handle) host_bitmap_free(b->handle); }
static Object *load_bitmap_res(int id) {
    const char *type = NULL;
    const char *name = res_name_for_id(id, &type);
    if (!name) { vm_log("bitmap: unknown resource id 0x%x", id); return NULL; }
    char path[256];
    int n = host_res_path(type, name, path, sizeof path - 1);
    if (n < 0) { vm_log("bitmap: resource %s/%s not in apk", type, name); return NULL; }
    path[n] = 0;
    int h = host_bitmap_load(path, n);
    if (h <= 0) { vm_log("bitmap: failed to decode %s", path); return NULL; }
    BitmapObj *b = (BitmapObj *)new_object(find_class("Landroid/graphics/Bitmap;"));
    b->handle = h; b->w = host_bitmap_width(h); b->hgt = host_bitmap_height(h);
    return (Object *)b;
}
static void BF_decodeResource(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(load_bitmap_res(ARG_I(1))); }
static void Bmp_getPixels(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    BitmapObj *b = (BitmapObj *)ARG_L(0);
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), stride = ARG_I(3), x = ARG_I(4), y = ARG_I(5), w = ARG_I(6), h = ARG_I(7);
    if (x < 0 || y < 0 || x + w > b->w || y + h > b->hgt) { throw_new(t, "Ljava/lang/IllegalArgumentException;", NULL); return; }
    host_bitmap_pixels(b->handle, ARR_DATA(a), off, stride, x, y, w, h);
}
static void Bmp_recycle(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    BitmapObj *b = (BitmapObj *)ARG_L(0);
    if (b->handle) host_bitmap_free(b->handle);
    b->handle = 0;
}
static void Bmp_getWidth(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((BitmapObj *)ARG_L(0))->w); }
static void Bmp_getHeight(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((BitmapObj *)ARG_L(0))->hgt); }
static void GLU_texImage2D(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    BitmapObj *b = (BitmapObj *)ARG_L(2);
    if (b && b->handle) host_bitmap_teximage(ARG_I(0), ARG_I(1), b->handle);
}
static NativeDef Bitmap_methods[] = {
    {"getPixels", "([IIIIIII)V", Bmp_getPixels, 0},
    {"recycle", "()V", Bmp_recycle, 0},
    {"getWidth", "()I", Bmp_getWidth, 0},
    {"getHeight", "()I", Bmp_getHeight, 0},
    {0}};
static NativeDef BF_methods[] = {
    {"decodeResource", "(Landroid/content/res/Resources;I)Landroid/graphics/Bitmap;", BF_decodeResource, 1},
    {0}};
static NativeDef GLU_methods[] = {
    {"texImage2D", "(IILandroid/graphics/Bitmap;I)V", GLU_texImage2D, 1},
    {0}};

/* ---------- assets ---------- */
typedef struct AFDObj { Object h; JString *path; } AFDObj;
static void AFD_mark(Object *o) { gc_mark((Object *)((AFDObj *)o)->path); }
static char *asset_path(JString *s) {
    char *u = jstr_to_utf8(s);
    char *p = malloc(strlen(u) + 8);
    sprintf(p, "assets/%s", u);
    free(u);
    return p;
}
static void AM_open(Thread *t, Reg *args, Reg *ret) {
    JString *s = (JString *)ARG_L(1); NPE_CHECK(s);
    char *p = asset_path(s);
    int sz = host_asset_size(p, (int)strlen(p));
    if (sz < 0) { vm_log("asset not found: %s", p); throw_new(t, "Ljava/io/FileNotFoundException;", p); free(p); return; }
    uint8_t *d = malloc(sz + 1);
    host_asset_read(p, (int)strlen(p), d);
    free(p);
    RET_L(stream_from_bytes(find_class("Landroid/content/res/AssetManager$AssetInputStream;"), d, sz, 1));
}
static void AM_openFd(Thread *t, Reg *args, Reg *ret) {
    JString *s = (JString *)ARG_L(1); NPE_CHECK(s);
    char *p = asset_path(s);
    int sz = host_asset_size(p, (int)strlen(p));
    if (sz < 0) { vm_log("asset fd not found: %s", p); throw_new(t, "Ljava/io/FileNotFoundException;", p); free(p); return; }
    AFDObj *a = (AFDObj *)new_object(find_class("Landroid/content/res/AssetFileDescriptor;"));
    a->path = jstr_from_utf8(p);
    free(p);
    RET_L(a);
}
static NativeDef AM_methods[] = {
    {"open", "(Ljava/lang/String;)Ljava/io/InputStream;", AM_open, 0},
    {"openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;", AM_openFd, 0},
    {0}};
static NativeDef AFD_methods[] = {
    {"close", "()V", noop, 0},
    {0}};

/* ---------- audio ---------- */
static void SP_load(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    AFDObj *a = (AFDObj *)ARG_L(1);
    if (!a || !a->path) { RET_I(0); return; }
    char *p = jstr_to_utf8(a->path);
    RET_I(host_snd_load(p, (int)strlen(p)));
    free(p);
}
static void SP_play(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    // play(soundID, leftVolume, rightVolume, priority, loop, rate)
    RET_I(host_snd_play(ARG_I(1), ARG_F(2), ARG_F(3), ARG_I(5), ARG_F(6)));
}
static void SP_stop(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; host_snd_stop(ARG_I(1)); }
static NativeDef SoundPool_methods[] = {
    {"<init>", "(III)V", noop, 0},
    {"load", "(Landroid/content/res/AssetFileDescriptor;I)I", SP_load, 0},
    {"play", "(IFFIIF)I", SP_play, 0},
    {"stop", "(I)V", SP_stop, 0},
    {"pause", "(I)V", SP_stop, 0},
    {"release", "()V", noop, 0},
    {"setVolume", "(IFF)V", noop, 0},
    {0}};
typedef struct MPObj { Object h; int handle; } MPObj;
static void MP_fin(Object *o) { MPObj *m = (MPObj *)o; if (m->handle) host_mus_cmd(m->handle, 9, 0, 0); }
static void MP_create(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    const char *type = NULL;
    const char *name = res_name_for_id(ARG_I(1), &type);
    if (!name) { RET_L(NULL); return; }
    char path[256];
    int n = host_res_path(type, name, path, sizeof path - 1);
    if (n < 0) { RET_L(NULL); return; }
    path[n] = 0;
    MPObj *m = (MPObj *)new_object(find_class("Landroid/media/MediaPlayer;"));
    m->handle = host_mus_create(path, n);
    RET_L(m);
}
#define MPCMD(nm, c) static void MP_##nm(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; MPObj *m = (MPObj *)ARG_L(0); if (m->handle) host_mus_cmd(m->handle, c, ARG_F(1), ARG_F(2)); }
MPCMD(start, 1) MPCMD(stop, 2) MPCMD(pause, 3) MPCMD(setVolume, 5)
static void MP_setLooping(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; MPObj *m = (MPObj *)ARG_L(0); if (m->handle) host_mus_cmd(m->handle, 4, (float)ARG_I(1), 0); }
static void MP_release(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; MPObj *m = (MPObj *)ARG_L(0); if (m->handle) host_mus_cmd(m->handle, 9, 0, 0); m->handle = 0; }
static NativeDef MP_methods[] = {
    {"create", "(Landroid/content/Context;I)Landroid/media/MediaPlayer;", MP_create, 1},
    {"start", "()V", MP_start, 0},
    {"stop", "()V", MP_stop, 0},
    {"pause", "()V", MP_pause, 0},
    {"setVolume", "(FF)V", MP_setVolume, 0},
    {"setLooping", "(Z)V", MP_setLooping, 0},
    {"release", "()V", MP_release, 0},
    {"prepare", "()V", noop, 0},
    {"isPlaying", "()Z", ret_false, 0},
    {0}};

/* ---------- sensors, vibrator, process ---------- */
static void SM_register(Thread *t, Reg *args, Reg *ret) { (void)t; g_sensor_listener = ARG_L(1); RET_I(1); }
static void SM_unregister(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; if (g_sensor_listener == ARG_L(1)) g_sensor_listener = NULL; }
static NativeDef SM_methods[] = {
    {"registerListener", "(Landroid/hardware/SensorListener;II)Z", SM_register, 0},
    {"registerListener", "(Landroid/hardware/SensorListener;I)Z", SM_register, 0},
    {"unregisterListener", "(Landroid/hardware/SensorListener;)V", SM_unregister, 0},
    {0}};
static void Vib_vibrate(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; host_vibrate((int)ARG_J(1)); }
static void Vib_cancel(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; (void)args; host_vibrate(0); }
static NativeDef Vib_methods[] = {
    {"vibrate", "(J)V", Vib_vibrate, 0},
    {"cancel", "()V", Vib_cancel, 0},
    {0}};
static void Proc_myPid(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(1234); }
static void Proc_kill(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; vm_log("Process.killProcess"); g_exited = 1; host_exit(0); }
static NativeDef Proc_methods[] = {
    {"myPid", "()I", Proc_myPid, 1},
    {"killProcess", "(I)V", Proc_kill, 1},
    {"sendSignal", "(II)V", noop, 1},
    {0}};

/* ---------- MotionEvent ---------- */
typedef struct MEObj { Object h; int action; int count; int ids[8]; float x[8], y[8]; } MEObj;
#define ME ((MEObj *)ARG_L(0))
static void ME_getAction(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ME->action); }
static void ME_getActionMasked(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ME->action & 0xff); }
static void ME_getPointerCount(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ME->count); }
static void ME_getPointerId(Thread *t, Reg *args, Reg *ret) { (void)t; int i = ARG_I(1); RET_I(i >= 0 && i < ME->count ? ME->ids[i] : 0); }
static void ME_getX(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(ME->x[0]); }
static void ME_getY(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(ME->y[0]); }
static void ME_getXI(Thread *t, Reg *args, Reg *ret) { (void)t; int i = ARG_I(1); RET_F(i >= 0 && i < ME->count ? ME->x[i] : 0); }
static void ME_getYI(Thread *t, Reg *args, Reg *ret) { (void)t; int i = ARG_I(1); RET_F(i >= 0 && i < ME->count ? ME->y[i] : 0); }
static NativeDef ME_methods[] = {
    {"getAction", "()I", ME_getAction, 0},
    {"getActionMasked", "()I", ME_getActionMasked, 0},
    {"getPointerCount", "()I", ME_getPointerCount, 0},
    {"getPointerId", "(I)I", ME_getPointerId, 0},
    {"getX", "()F", ME_getX, 0},
    {"getY", "()F", ME_getY, 0},
    {"getRawX", "()F", ME_getX, 0},
    {"getRawY", "()F", ME_getY, 0},
    {"getX", "(I)F", ME_getXI, 0},
    {"getY", "(I)F", ME_getYI, 0},
    {0}};

/* ---------- dialogs ---------- */
typedef struct DlgObj { Object h; JString *title, *msg, *pos, *neg; Object *posL, *negL, *view; int id; } DlgObj;
typedef struct EditObj { Object h; JString *text; } EditObj;
static void Dlg_mark(Object *o) {
    DlgObj *d = (DlgObj *)o;
    gc_mark((Object *)d->title); gc_mark((Object *)d->msg); gc_mark((Object *)d->pos); gc_mark((Object *)d->neg);
    gc_mark(d->posL); gc_mark(d->negL); gc_mark(d->view);
}
static void Edit_mark(Object *o) { gc_mark((Object *)((EditObj *)o)->text); }
#define DLG ((DlgObj *)ARG_L(0))
static void Dlg_setTitle(Thread *t, Reg *args, Reg *ret) { DLG->title = obj_to_string(t, ARG_L(1)); RET_L(DLG); }
static void Dlg_setMessage(Thread *t, Reg *args, Reg *ret) { DLG->msg = obj_to_string(t, ARG_L(1)); RET_L(DLG); }
static void Dlg_setPos(Thread *t, Reg *args, Reg *ret) { DLG->pos = obj_to_string(t, ARG_L(1)); DLG->posL = ARG_L(2); RET_L(DLG); }
static void Dlg_setNeg(Thread *t, Reg *args, Reg *ret) { DLG->neg = obj_to_string(t, ARG_L(1)); DLG->negL = ARG_L(2); RET_L(DLG); }
static void Dlg_setView(Thread *t, Reg *args, Reg *ret) { (void)t; DLG->view = ARG_L(1); RET_L(DLG); }
static void Dlg_show(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    DlgObj *d = DLG;
    int id = -1;
    for (int i = 0; i < MAX_DIALOGS; i++) if (!g_dialogs[i]) { id = i; break; }
    if (id < 0) { RET_L(NULL); return; }
    g_dialogs[id] = (Object *)d;
    d->id = id;
    char *ti = d->title ? jstr_to_utf8(d->title) : strdup("");
    char *ms = d->msg ? jstr_to_utf8(d->msg) : strdup("");
    char *po = d->pos ? jstr_to_utf8(d->pos) : strdup("");
    char *ne = d->neg ? jstr_to_utf8(d->neg) : strdup("");
    char *tx = strdup("");
    int has_input = 0;
    Class *ec = find_class("Landroid/widget/EditText;");
    if (d->view && is_subclass(d->view->cls, ec)) {
        has_input = 1;
        EditObj *e = (EditObj *)d->view;
        free(tx); tx = e->text ? jstr_to_utf8(e->text) : strdup("");
    }
    host_dialog(id, ti, ms, po, ne, tx, has_input);
    free(ti); free(ms); free(po); free(ne); free(tx);
    RET_L(new_object(find_class("Landroid/app/AlertDialog;")));
}
static NativeDef Dlg_methods[] = {
    {"<init>", "(Landroid/content/Context;)V", noop, 0},
    {"setTitle", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", Dlg_setTitle, 0},
    {"setMessage", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", Dlg_setMessage, 0},
    {"setPositiveButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", Dlg_setPos, 0},
    {"setNegativeButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", Dlg_setNeg, 0},
    {"setView", "(Landroid/view/View;)Landroid/app/AlertDialog$Builder;", Dlg_setView, 0},
    {"show", "()Landroid/app/AlertDialog;", Dlg_show, 0},
    {0}};
static void Edit_setText(Thread *t, Reg *args, Reg *ret) { (void)ret; ((EditObj *)ARG_L(0))->text = ARG_L(1) ? obj_to_string(t, ARG_L(1)) : NULL; }
static void Edit_getText(Thread *t, Reg *args, Reg *ret) { (void)t; EditObj *e = (EditObj *)ARG_L(0); RET_L(e->text ? e->text : jstr_from_utf8("")); }
static NativeDef Edit_methods[] = {
    {"<init>", "(Landroid/content/Context;)V", noop, 0},
    {"setText", "(Ljava/lang/CharSequence;Landroid/widget/TextView$BufferType;)V", Edit_setText, 0},
    {"setText", "(Ljava/lang/CharSequence;)V", Edit_setText, 0},
    {"getText", "()Landroid/text/Editable;", Edit_getText, 0},
    {0}};
static void BT_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    Class *c = find_class("Landroid/widget/TextView$BufferType;");
    const char *names[3] = {"NORMAL", "SPANNABLE", "EDITABLE"};
    for (int i = 0; i < 3; i++) {
        Object *o = new_object(c);
        Field *f = class_find_field(c, names[i], NULL);
        memcpy(c->statics + f->offset, &o, sizeof o);
    }
}
static NativeDef BT_methods[] = {{"<clinit>", "()V", BT_clinit, 1}, {0}};

static NativeDef none[] = {{0}};
static NativeDef Renderer_iface[] = {
    {"onSurfaceCreated", "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V", NULL, 0},
    {"onSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V", NULL, 0},
    {"onDrawFrame", "(Ljavax/microedition/khronos/opengles/GL10;)V", NULL, 0},
    {0}};

#define IFACE(n) {n, NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}}
#define PLAIN(n, s, m) {n, s, {0}, sizeof(Object), 0, m, NULL, NULL, {0}}
BuiltinDef android_builtins[] = {
    IFACE("Landroid/content/DialogInterface;"),
    IFACE("Landroid/content/DialogInterface$OnClickListener;"),
    IFACE("Landroid/hardware/SensorListener;"),
    IFACE("Landroid/hardware/SensorEventListener;"),
    IFACE("Landroid/text/Editable;"),
    IFACE("Ljavax/microedition/khronos/opengles/GL;"),
    {"Ljavax/microedition/khronos/opengles/GL10;", NULL, {"Ljavax/microedition/khronos/opengles/GL;"}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljavax/microedition/khronos/opengles/GL11;", NULL, {"Ljavax/microedition/khronos/opengles/GL10;"}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    IFACE("Ljavax/microedition/khronos/egl/EGLConfig;"),
    {"Landroid/opengl/GLSurfaceView$Renderer;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Renderer_iface, NULL, NULL, {0}},
    PLAIN("Landroid/content/Context;", "Ljava/lang/Object;", Context_methods),
    PLAIN("Landroid/content/ContextWrapper;", "Landroid/content/Context;", none),
    PLAIN("Landroid/view/ContextThemeWrapper;", "Landroid/content/ContextWrapper;", none),
    PLAIN("Landroid/app/Activity;", "Landroid/view/ContextThemeWrapper;", Activity_methods),
    PLAIN("Landroid/os/Bundle;", "Ljava/lang/Object;", none),
    PLAIN("Landroid/view/View;", "Ljava/lang/Object;", View_methods),
    PLAIN("Landroid/view/SurfaceView;", "Landroid/view/View;", none),
    PLAIN("Landroid/opengl/GLSurfaceView;", "Landroid/view/SurfaceView;", GLSV_methods),
    {"Lcom/vm/GLImpl;", "Ljava/lang/Object;", {"Ljavax/microedition/khronos/opengles/GL11;"}, sizeof(Object), 0, GLImpl_methods, NULL, NULL, {0}},
    PLAIN("Landroid/opengl/GLES10;", "Ljava/lang/Object;", GLES11_methods),
    PLAIN("Landroid/opengl/GLES11;", "Landroid/opengl/GLES10;", none),
    PLAIN("Landroid/opengl/GLUtils;", "Ljava/lang/Object;", GLU_methods),
    {"Landroid/graphics/Bitmap;", "Ljava/lang/Object;", {0}, sizeof(BitmapObj), 0, Bitmap_methods, NULL, Bitmap_fin, {0}},
    PLAIN("Landroid/graphics/BitmapFactory;", "Ljava/lang/Object;", BF_methods),
    PLAIN("Landroid/content/res/Resources;", "Ljava/lang/Object;", none),
    PLAIN("Landroid/content/res/AssetManager;", "Ljava/lang/Object;", AM_methods),
    {"Landroid/content/res/AssetFileDescriptor;", "Ljava/lang/Object;", {0}, sizeof(AFDObj), 0, AFD_methods, AFD_mark, NULL, {0}},
    PLAIN("Landroid/media/SoundPool;", "Ljava/lang/Object;", SoundPool_methods),
    {"Landroid/media/MediaPlayer;", "Ljava/lang/Object;", {0}, sizeof(MPObj), 0, MP_methods, NULL, MP_fin, {0}},
    PLAIN("Landroid/hardware/SensorManager;", "Ljava/lang/Object;", SM_methods),
    PLAIN("Landroid/os/Vibrator;", "Ljava/lang/Object;", Vib_methods),
    PLAIN("Landroid/os/Process;", "Ljava/lang/Object;", Proc_methods),
    {"Landroid/view/MotionEvent;", "Ljava/lang/Object;", {0}, sizeof(MEObj), 0, ME_methods, NULL, NULL, {0}},
    {"Landroid/app/AlertDialog$Builder;", "Ljava/lang/Object;", {0}, sizeof(DlgObj), 0, Dlg_methods, Dlg_mark, NULL, {0}},
    {"Landroid/app/Dialog;", "Ljava/lang/Object;", {"Landroid/content/DialogInterface;"}, sizeof(Object), 0, none, NULL, NULL, {0}},
    PLAIN("Landroid/app/AlertDialog;", "Landroid/app/Dialog;", none),
    PLAIN("Landroid/widget/TextView;", "Landroid/view/View;", none),
    {"Landroid/widget/EditText;", "Landroid/widget/TextView;", {0}, sizeof(EditObj), 0, Edit_methods, Edit_mark, NULL, {0}},
    {"Landroid/widget/TextView$BufferType;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, BT_methods, NULL, NULL, {"NORMAL:Landroid/widget/TextView$BufferType;", "SPANNABLE:Landroid/widget/TextView$BufferType;", "EDITABLE:Landroid/widget/TextView$BufferType;"}},
    {0}};

/* =================== driver =================== */
#ifdef __wasm__
#define EXPORT(n) __attribute__((export_name(#n)))
#else
#define EXPORT(n)
#endif
static void report_exception(Thread *t, const char *where) {
    if (!t->exception) return;
    Object *e = t->exception;
    t->exception = NULL;
    JString *s = obj_to_string(t, e);
    t->exception = NULL;
    char *u = jstr_to_utf8(s);
    vm_log("uncaught exception in %s: %s", where, u);
    free(u);
}

static Object *call0(Object *o, const char *name, const char *sig) {
    Reg a[2];
    Reg r = call_virtual(g_main, o, name, sig, a);
    report_exception(g_main, name);
    return r.l;
}

void green_start(Object *jthread, Object *runnable) {
    Thread *t = thread_new(1);
    t->jthread = jthread;
    t->runnable = runnable;
    Method *m = find_virtual(runnable->cls, "run", "()V");
    if (!m || !m->has_code) {
        if (m && m->native) { Reg a[1]; R_SETL(a[0], runnable); m->native(t, a, &t->ret); }
        t->state = TH_DEAD;
        return;
    }
    Reg a[1]; R_SETL(a[0], runnable);
    thread_begin(t, m, a);
    t->state = TH_RUNNABLE;
    vm_log("thread %d started (%s)", t->id, runnable->cls->name);
}

static void run_green_threads(void) {
    double now = vm_now_ms();
    for (int i = 0; i < g_nthreads; i++) {
        Thread *t = g_threads[i];
        if (!t->green || t->state == TH_DEAD) continue;
        if (t->state == TH_SLEEPING) { if (now < t->wake_time) continue; t->state = TH_RUNNABLE; }
        int y = interp(t, 0);
        if (!y) {
            report_exception(t, "green thread");
            t->state = TH_DEAD;
        }
    }
    for (int i = 0; i < g_nthreads; i++) {
        Thread *t = g_threads[i];
        if (t->green && t->state == TH_DEAD) { thread_free(t); i--; }
    }
}

static Class *find_activity_class(void) {
    Class *act = find_class("Landroid/app/Activity;");
    Dex *d = g_dex;
    for (uint32_t i = 0; i < d->nclassdefs; i++) {
        const uint8_t *cd = d->b + d->class_defs_off + 32 * i;
        uint32_t ti; memcpy(&ti, cd, 4);
        uint32_t sup; memcpy(&sup, cd + 8, 4);
        if (sup == 0xffffffff) continue;
        if (!strcmp(dex_type(d, sup), "Landroid/app/Activity;")) return find_class(dex_type(d, ti));
    }
    (void)act;
    return NULL;
}

static int g_booted;
EXPORT(vm_alloc) void *vm_alloc(int n) { return malloc(n); }
EXPORT(vm_free) void vm_free(void *p) { free(p); }

EXPORT(vm_start) int vm_start(const uint8_t *dex, int len) {
    dex_boot(dex, len);
    g_main = thread_new(0);
    Class *ac = find_activity_class();
    if (!ac) { vm_log("no Activity subclass found"); return 0; }
    vm_log("main activity: %s", ac->name);
    class_init(g_main, ac);
    report_exception(g_main, "activity clinit");
    g_activity = new_object(ac);
    call0(g_activity, "<init>", "()V");
    Reg a[2]; R_SETL(a[0], g_activity); a[1].raw = 0;
    call_virtual(g_main, g_activity, "onCreate", "(Landroid/os/Bundle;)V", a);
    report_exception(g_main, "onCreate");
    call0(g_activity, "onStart", "()V");
    call0(g_activity, "onResume", "()V");
    g_gl = new_object(find_class("Lcom/vm/GLImpl;"));
    g_booted = 1;
    return 1;
}

EXPORT(vm_surface) void vm_surface(int w, int h) {
    if (!g_renderer) return;
    Reg a[4];
    if (!g_surface_created) {
        R_SETL(a[1], g_gl); a[2].raw = 0;
        call_virtual(g_main, g_renderer, "onSurfaceCreated", "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V", a);
        report_exception(g_main, "onSurfaceCreated");
        g_surface_created = 1;
    }
    R_SETL(a[1], g_gl); R_SETI(a[2], w); R_SETI(a[3], h);
    call_virtual(g_main, g_renderer, "onSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V", a);
    report_exception(g_main, "onSurfaceChanged");
}

extern long long g_icount;
static double g_gc_ms;
EXPORT(vm_frame) void vm_frame(void) {
    if (!g_booted || g_exited) return;
    // UI runnables
    while (g_uiq_n) {
        Object *r = g_uiq[0];
        memmove(g_uiq, g_uiq + 1, sizeof(Object *) * (--g_uiq_n));
        call0(r, "run", "()V");
    }
    run_green_threads();
    if (g_renderer) {
        Reg a[2]; R_SETL(a[1], g_gl);
        call_virtual(g_main, g_renderer, "onDrawFrame", "(Ljavax/microedition/khronos/opengles/GL10;)V", a);
        report_exception(g_main, "onDrawFrame");
    }
    if (gc_bytes_since() > 48u * 1024 * 1024) {
        double t0 = vm_now_ms();
        gc_collect();
        g_gc_ms = vm_now_ms() - t0;
    }
}

EXPORT(vm_touch) void vm_touch(int action, int count, const float *data) {
    if (!g_booted || !g_view) return;
    MEObj *e = (MEObj *)new_object(find_class("Landroid/view/MotionEvent;"));
    e->action = action;
    e->count = count > 8 ? 8 : count;
    for (int i = 0; i < e->count; i++) { e->ids[i] = (int)data[3 * i]; e->x[i] = data[3 * i + 1]; e->y[i] = data[3 * i + 2]; }
    Reg a[2]; R_SETL(a[1], e);
    call_virtual(g_main, g_view, "onTouchEvent", "(Landroid/view/MotionEvent;)Z", a);
    report_exception(g_main, "onTouchEvent");
}

EXPORT(vm_accel) void vm_accel(float x, float y, float z) {
    if (!g_booted || !g_sensor_listener) return;
    Array *v = new_array(C_arrF, 3);
    float *f = ARR_DATA(v); f[0] = x; f[1] = y; f[2] = z;
    Reg a[3]; R_SETI(a[1], 2); R_SETL(a[2], v);
    call_virtual(g_main, g_sensor_listener, "onSensorChanged", "(I[F)V", a);
    report_exception(g_main, "onSensorChanged");
}

EXPORT(vm_back) void vm_back(void) {
    if (!g_booted) return;
    call0(g_activity, "onBackPressed", "()V");
}

EXPORT(vm_dialog_result) void vm_dialog_result(int id, int which, const char *text) {
    if (id < 0 || id >= MAX_DIALOGS || !g_dialogs[id]) return;
    DlgObj *d = (DlgObj *)g_dialogs[id];
    g_dialogs[id] = NULL;
    Class *ec = find_class("Landroid/widget/EditText;");
    if (d->view && is_subclass(d->view->cls, ec) && text) ((EditObj *)d->view)->text = jstr_from_utf8(text);
    Object *l = which == -1 ? d->posL : d->negL;
    if (!l) return;
    Reg a[3]; a[1].raw = 0; R_SETI(a[2], which);
    call_virtual(g_main, l, "onClick", "(Landroid/content/DialogInterface;I)V", a);
    report_exception(g_main, "dialog onClick");
}

EXPORT(vm_stats) double vm_stats(int what) {
    switch (what) {
    case 0: return (double)g_icount;
    case 1: return (double)gc_object_count();
    case 2: return (double)g_heap_bytes;
    case 3: return g_gc_ms;
    }
    return 0;
}

EXPORT(vm_gc) void vm_gc(void) { gc_collect(); }
