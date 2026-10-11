// Native test harness: runs the VM against an extracted APK directory with stub GL/audio.
#include "../vm/vm.h"
#include "../vm/host.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *g_root;
static double g_time;
static int g_quiet_gl = 1;
static long g_draws, g_gl_calls, g_tex_uploads;

void host_log(const char *s, int len) { fwrite(s, 1, len, stdout); fputc('\n', stdout); fflush(stdout); }
double host_now(void) { return g_time; }
void host_exit(int code) { printf("host_exit(%d)\n", code); exit(code); }

static char *mkpath(const char *p, int len) {
    char *b = malloc(strlen(g_root) + len + 2);
    sprintf(b, "%s/%.*s", g_root, len, p);
    return b;
}
int host_asset_size(const char *path, int len) {
    char *p = mkpath(path, len);
    struct stat st;
    int r = stat(p, &st) == 0 ? (int)st.st_size : -1;
    free(p);
    return r;
}
void host_asset_read(const char *path, int len, void *dst) {
    char *p = mkpath(path, len);
    FILE *f = fopen(p, "rb");
    if (f) { struct stat st; stat(p, &st); fread(dst, 1, st.st_size, f); fclose(f); }
    free(p);
}
int host_res_path(const char *type, const char *name, char *out, int cap) {
    char dir[1024]; snprintf(dir, sizeof dir, "%s/res", g_root);
    DIR *d = opendir(dir); if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, type, strlen(type))) continue;
        char sub[1024]; snprintf(sub, sizeof sub, "%s/%s", dir, de->d_name);
        DIR *d2 = opendir(sub); if (!d2) continue;
        struct dirent *f;
        while ((f = readdir(d2))) {
            char *dot = strrchr(f->d_name, '.');
            size_t bl = dot ? (size_t)(dot - f->d_name) : strlen(f->d_name);
            if (bl == strlen(name) && !strncmp(f->d_name, name, bl) && !(dot && !strcmp(dot, ".rgba"))) {
                found = snprintf(out, cap, "res/%s/%s", de->d_name, f->d_name);
                break;
            }
        }
        closedir(d2);
        if (found >= 0) break;
    }
    closedir(d);
    return found;
}
static char g_savedir[256] = "/tmp/pgsave";
int host_file_size(const char *name, int len) {
    char p[512]; snprintf(p, sizeof p, "%s/%.*s", g_savedir, len, name);
    struct stat st; return stat(p, &st) == 0 ? (int)st.st_size : -1;
}
void host_file_read(const char *name, int len, void *dst) {
    char p[512]; snprintf(p, sizeof p, "%s/%.*s", g_savedir, len, name);
    FILE *f = fopen(p, "rb"); if (!f) return;
    struct stat st; stat(p, &st); fread(dst, 1, st.st_size, f); fclose(f);
}
void host_file_write(const char *name, int len, const void *data, int dlen) {
    mkdir(g_savedir, 0755);
    char p[512]; snprintf(p, sizeof p, "%s/%.*s", g_savedir, len, name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fwrite(data, 1, dlen, f); fclose(f);
    printf("[host] saved %s (%d bytes)\n", p, dlen);
}

/* bitmaps: expects <path>.rgba = u32 w, u32 h, RGBA bytes (made by tools/png2rgba.py) */
typedef struct { int w, h; uint8_t *px; } Bmp;
static Bmp g_bmps[64]; static int g_nbmp;
int host_bitmap_load(const char *path, int len) {
    char *p = mkpath(path, len);
    char q[1024]; snprintf(q, sizeof q, "%s.rgba", p); free(p);
    FILE *f = fopen(q, "rb");
    if (!f) { printf("[host] no %s\n", q); return 0; }
    uint32_t wh[2]; fread(wh, 4, 2, f);
    Bmp *b = &g_bmps[++g_nbmp];
    b->w = wh[0]; b->h = wh[1]; b->px = malloc((size_t)b->w * b->h * 4);
    fread(b->px, 4, (size_t)b->w * b->h, f); fclose(f);
    return g_nbmp;
}
int host_bitmap_width(int h) { return g_bmps[h].w; }
int host_bitmap_height(int h) { return g_bmps[h].h; }
void host_bitmap_pixels(int h, int32_t *dst, int offset, int stride, int x, int y, int w, int hgt) {
    Bmp *b = &g_bmps[h];
    for (int j = 0; j < hgt; j++) for (int i = 0; i < w; i++) {
        uint8_t *s = b->px + 4 * ((size_t)(y + j) * b->w + x + i);
        dst[offset + j * stride + i] = (int32_t)(((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]);
    }
}
void host_bitmap_free(int h) { (void)h; }
void host_bitmap_teximage(int target, int level, int h) { (void)target; (void)level; (void)h; g_tex_uploads++; }

static int g_nsnd;
int host_snd_load(const char *path, int len) { (void)path; (void)len; return ++g_nsnd; }
int host_snd_play(int sound, float lv, float rv, int loop, float rate) { (void)sound; (void)lv; (void)rv; (void)loop; (void)rate; static int s; return ++s; }
void host_snd_stop(int stream) { (void)stream; }
int host_mus_create(const char *path, int len) { printf("[host] music %.*s\n", len, path); static int m; return ++m; }
void host_mus_cmd(int h, int cmd, float a, float b) { (void)h; (void)cmd; (void)a; (void)b; }
void host_vibrate(int ms) { (void)ms; }
void host_dialog(int id, const char *title, const char *msg, const char *pos, const char *neg, const char *text, int has_input) {
    extern int g_pending_dialog; g_pending_dialog = id;
    printf("[host] dialog %d '%s' '%s' [%s/%s] text='%s' input=%d\n", id, title, msg, pos, neg, text, has_input);
}

/* GL stubs */
int g_pending_dialog = -1;
static int g_texid;
void vm_dialog_result(int id, int which, const char *text);
#define GLC g_gl_calls++
void gl_BindTexture(int a, int b) { (void)a; (void)b; GLC; }
void gl_BlendFunc(int s, int d) { (void)s; (void)d; GLC; }
void gl_Clear(int m) { (void)m; GLC; }
void gl_ClearColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; GLC; }
void gl_Color4f(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; GLC; }
void gl_DeleteTextures(int n, const int32_t *ids) { (void)n; (void)ids; GLC; }
void gl_GenTextures(int n, int32_t *ids) { for (int i = 0; i < n; i++) ids[i] = ++g_texid; GLC; }
void gl_Enable(int c) { (void)c; GLC; }
void gl_Disable(int c) { (void)c; GLC; }
void gl_EnableClientState(int a) { (void)a; GLC; }
void gl_DisableClientState(int a) { (void)a; GLC; }
void gl_DrawArrays(int m, int f, int c) { (void)m; (void)f; (void)c; GLC; g_draws++; }
void gl_DrawElements(int m, int c, int t, const void *i) { (void)m; (void)c; (void)t; (void)i; GLC; g_draws++; }
void gl_GetIntegerv(int p, int32_t *o) { (void)p; o[0] = 0; GLC; }
int gl_GetError(void) { return 0; }
void gl_LoadIdentity(void) { GLC; }
void gl_LoadMatrixf(const float *m) { (void)m; GLC; }
void gl_MultMatrixf(const float *m) { (void)m; GLC; }
void gl_MatrixMode(int m) { (void)m; GLC; }
void gl_Orthof(float l, float r, float b, float t, float n, float f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; GLC; }
void gl_PushMatrix(void) { GLC; }
void gl_PopMatrix(void) { GLC; }
void gl_Rotatef(float a, float x, float y, float z) { (void)a; (void)x; (void)y; (void)z; GLC; }
void gl_Scalef(float x, float y, float z) { (void)x; (void)y; (void)z; GLC; }
void gl_Translatef(float x, float y, float z) { (void)x; (void)y; (void)z; GLC; }
void gl_ShadeModel(int m) { (void)m; GLC; }
void gl_TexParameteri(int t, int p, int v) { (void)t; (void)p; (void)v; GLC; }
void gl_TexImage2D(int target, int level, int ifmt, int w, int h, int border, int fmt, int type, const void *data) {
    (void)target; (void)level; (void)border; (void)data; GLC; g_tex_uploads++;
    if (!g_quiet_gl) printf("[gl] texImage2D %dx%d ifmt=%x fmt=%x type=%x\n", w, h, ifmt, fmt, type);
}
void gl_TexSubImage2D(int target, int level, int x, int y, int w, int h, int fmt, int type, const void *data) {
    (void)target; (void)level; (void)x; (void)y; (void)w; (void)h; (void)fmt; (void)type; (void)data; GLC; g_tex_uploads++;
}
void gl_VertexPointer(int s, int t, int st, const void *p) { (void)s; (void)t; (void)st; (void)p; GLC; }
void gl_TexCoordPointer(int s, int t, int st, const void *p) { (void)s; (void)t; (void)st; (void)p; GLC; }
void gl_ColorPointer(int s, int t, int st, const void *p) { (void)s; (void)t; (void)st; (void)p; GLC; }
void gl_Viewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; GLC; }

/* exported VM API */
int vm_start(const uint8_t *dex, int len);
void vm_surface(int w, int h);
void vm_frame(void);
void vm_touch(int action, int count, const float *data);
double vm_stats(int what);
void vm_gc(void);

static void tap(float x, float y) {
    float d[3] = {0, x, y};
    vm_touch(0, 1, d);
    vm_frame(); g_time += 16.67;
    vm_touch(1, 1, d);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <extracted-apk-dir> [frames] [script]\n", argv[0]); return 1; }
    g_root = argv[1];
    int frames = argc > 2 ? atoi(argv[2]) : 200;
    const char *script = argc > 3 ? argv[3] : "";
    char dp[1024]; snprintf(dp, sizeof dp, "%s/classes.dex", g_root);
    FILE *f = fopen(dp, "rb");
    if (!f) { perror(dp); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *dex = malloc(n); fread(dex, 1, n, f); fclose(f);
    g_time = 1000;
    if (!vm_start(dex, (int)n)) return 1;
    vm_surface(800, 480);
    struct timespec ts0, ts1;
    for (int i = 0; i < frames; i++) {
        long d0 = g_draws;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        double ic0 = vm_stats(0);
        vm_frame();
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        double ms = (ts1.tv_sec - ts0.tv_sec) * 1e3 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
        if (i % 50 == 0 || ms > 50)
            printf("[frame %d] %.1fms draws=%ld insns=%.0f objs=%.0f heap=%.0fMB tex=%ld\n", i, ms, g_draws - d0, vm_stats(0) - ic0, vm_stats(1), vm_stats(2) / 1048576.0, g_tex_uploads);
        if (g_pending_dialog >= 0) { int d = g_pending_dialog; g_pending_dialog = -1; printf("[script] answering dialog %d\n", d); vm_dialog_result(d, -1, "Webby Tribe"); }
        g_time += 16.67;
        { static int ge = -1; if (ge < 0) { const char *e = getenv("PG_GC_EVERY"); ge = e ? atoi(e) : 0; } if (ge && i % ge == 0) vm_gc(); }
        // simple script: "t<frame>:<x>,<y>;..."
        for (const char *p = script; *p;) {
            int fr; float x, y;
            if (sscanf(p, "t%d:%f,%f", &fr, &x, &y) == 3 && fr == i) { printf("[script] tap %.0f,%.0f\n", x, y); tap(x, y); }
            const char *s = strchr(p, ';'); if (!s) break; p = s + 1;
        }
    }
    printf("done. total insns=%.0f\n", vm_stats(0));
    return 0;
}
