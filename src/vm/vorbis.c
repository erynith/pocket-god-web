// Ogg Vorbis fallback decoder for browsers without native Vorbis (uses stb_vorbis, public domain).
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "../third_party/stb_vorbis.c"
#ifdef __wasm__
#define EXPORT(n) __attribute__((export_name(#n)))
#else
#define EXPORT(n)
#endif
static int g_vch, g_vrate;
static short *g_vout;
EXPORT(vorbis_decode) int vorbis_decode(const unsigned char *data, int len) {
    if (g_vout) { free(g_vout); g_vout = 0; }
    return stb_vorbis_decode_memory(data, len, &g_vch, &g_vrate, &g_vout);
}
EXPORT(vorbis_channels) int vorbis_channels(void) { return g_vch; }
EXPORT(vorbis_rate) int vorbis_rate(void) { return g_vrate; }
EXPORT(vorbis_samples) short *vorbis_samples(void) { return g_vout; }
EXPORT(vorbis_free) void vorbis_free(void) { if (g_vout) free(g_vout); g_vout = 0; }
