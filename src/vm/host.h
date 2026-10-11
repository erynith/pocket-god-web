// Functions the VM imports from its host (JS in the browser, C in the test harness).
#ifndef HOST_H
#define HOST_H
#include <stdint.h>

#ifdef __wasm__
#define HOST_IMPORT(name) __attribute__((import_module("host"), import_name(#name)))
#else
#define HOST_IMPORT(name)
#endif

HOST_IMPORT(log) void host_log(const char *s, int len);
HOST_IMPORT(now) double host_now(void);
HOST_IMPORT(exit) void host_exit(int code);

/* assets: returns size or -1 */
HOST_IMPORT(asset_size) int host_asset_size(const char *path, int len);
HOST_IMPORT(asset_read) void host_asset_read(const char *path, int len, void *dst);
/* resources: type ("raw","drawable"), name -> zip path written to out; returns len or -1 */
HOST_IMPORT(res_path) int host_res_path(const char *type, const char *name, char *out, int outcap);

/* private files (save data) */
HOST_IMPORT(file_size) int host_file_size(const char *name, int len);
HOST_IMPORT(file_read) void host_file_read(const char *name, int len, void *dst);
HOST_IMPORT(file_write) void host_file_write(const char *name, int len, const void *data, int dlen);

/* bitmaps (decoded from zip PNG), handle > 0 */
HOST_IMPORT(bitmap_load) int host_bitmap_load(const char *path, int len);
HOST_IMPORT(bitmap_width) int host_bitmap_width(int h);
HOST_IMPORT(bitmap_height) int host_bitmap_height(int h);
HOST_IMPORT(bitmap_pixels) void host_bitmap_pixels(int h, int32_t *dst, int offset, int stride, int x, int y, int w, int hgt);
HOST_IMPORT(bitmap_free) void host_bitmap_free(int h);
HOST_IMPORT(bitmap_teximage) void host_bitmap_teximage(int target, int level, int h);

/* audio */
HOST_IMPORT(snd_load) int host_snd_load(const char *path, int len);
HOST_IMPORT(snd_play) int host_snd_play(int sound, float lv, float rv, int loop, float rate);
HOST_IMPORT(snd_stop) void host_snd_stop(int stream);
HOST_IMPORT(mus_create) int host_mus_create(const char *path, int len);
HOST_IMPORT(mus_cmd) void host_mus_cmd(int h, int cmd, float a, float b);
HOST_IMPORT(vibrate) void host_vibrate(int ms);

/* dialogs */
HOST_IMPORT(dialog) void host_dialog(int id, const char *title, const char *msg, const char *pos, const char *neg, const char *text, int has_input);

/* GL (ES 1.1 subset). pointers are linear-memory addresses */
HOST_IMPORT(glBindTexture) void gl_BindTexture(int target, int tex);
HOST_IMPORT(glBlendFunc) void gl_BlendFunc(int s, int d);
HOST_IMPORT(glClear) void gl_Clear(int mask);
HOST_IMPORT(glClearColor) void gl_ClearColor(float r, float g, float b, float a);
HOST_IMPORT(glColor4f) void gl_Color4f(float r, float g, float b, float a);
HOST_IMPORT(glDeleteTextures) void gl_DeleteTextures(int n, const int32_t *ids);
HOST_IMPORT(glGenTextures) void gl_GenTextures(int n, int32_t *ids);
HOST_IMPORT(glEnable) void gl_Enable(int cap);
HOST_IMPORT(glDisable) void gl_Disable(int cap);
HOST_IMPORT(glEnableClientState) void gl_EnableClientState(int a);
HOST_IMPORT(glDisableClientState) void gl_DisableClientState(int a);
HOST_IMPORT(glDrawArrays) void gl_DrawArrays(int mode, int first, int count);
HOST_IMPORT(glDrawElements) void gl_DrawElements(int mode, int count, int type, const void *idx);
HOST_IMPORT(glGetIntegerv) void gl_GetIntegerv(int pname, int32_t *out);
HOST_IMPORT(glGetError) int gl_GetError(void);
HOST_IMPORT(glLoadIdentity) void gl_LoadIdentity(void);
HOST_IMPORT(glLoadMatrixf) void gl_LoadMatrixf(const float *m);
HOST_IMPORT(glMultMatrixf) void gl_MultMatrixf(const float *m);
HOST_IMPORT(glMatrixMode) void gl_MatrixMode(int m);
HOST_IMPORT(glOrthof) void gl_Orthof(float l, float r, float b, float t, float n, float f);
HOST_IMPORT(glPushMatrix) void gl_PushMatrix(void);
HOST_IMPORT(glPopMatrix) void gl_PopMatrix(void);
HOST_IMPORT(glRotatef) void gl_Rotatef(float a, float x, float y, float z);
HOST_IMPORT(glScalef) void gl_Scalef(float x, float y, float z);
HOST_IMPORT(glTranslatef) void gl_Translatef(float x, float y, float z);
HOST_IMPORT(glShadeModel) void gl_ShadeModel(int m);
HOST_IMPORT(glTexParameteri) void gl_TexParameteri(int target, int pname, int param);
HOST_IMPORT(glTexImage2D) void gl_TexImage2D(int target, int level, int ifmt, int w, int h, int border, int fmt, int type, const void *data);
HOST_IMPORT(glTexSubImage2D) void gl_TexSubImage2D(int target, int level, int x, int y, int w, int h, int fmt, int type, const void *data);
HOST_IMPORT(glVertexPointer) void gl_VertexPointer(int size, int type, int stride, const void *p);
HOST_IMPORT(glTexCoordPointer) void gl_TexCoordPointer(int size, int type, int stride, const void *p);
HOST_IMPORT(glColorPointer) void gl_ColorPointer(int size, int type, int stride, const void *p);
HOST_IMPORT(glViewport) void gl_Viewport(int x, int y, int w, int h);

#endif
