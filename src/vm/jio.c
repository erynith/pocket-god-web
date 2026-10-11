// java.io streams and java.nio buffers
#include "vm.h"
#include "objs.h"
#include "host.h"

/* ---------- streams ---------- */
#define STRM(n) ((StreamObj *)ARG_L(n))
static void Stream_mark(Object *o) { gc_mark(((StreamObj *)o)->inner); }
static void Stream_fin(Object *o) {
    StreamObj *s = (StreamObj *)o;
    if (s->owned) free(s->data);
    free(s->fname);
}
// follow wrappers down to the byte source
static StreamObj *src_of(StreamObj *s) {
    int guard = 0;
    while (s && !s->data && s->inner && guard++ < 16) s = (StreamObj *)s->inner;
    return s;
}
StreamObj *stream_from_bytes(Class *c, uint8_t *data, int len, int owned) {
    StreamObj *s = (StreamObj *)new_object(c);
    s->data = data; s->len = len; s->pos = 0; s->owned = owned;
    if (!data) { s->data = malloc(1); s->owned = 1; }
    return s;
}
static void IS_available(Thread *t, Reg *args, Reg *ret) { (void)t; StreamObj *s = src_of(STRM(0)); RET_I(s ? s->len - s->pos : 0); }
static void IS_close(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    StreamObj *s = STRM(0);
    s->closed = 1;
}
static int stream_read(StreamObj *s, uint8_t *dst, int n) {
    s = src_of(s);
    if (!s || !s->data) return -1;
    int avail = s->len - s->pos;
    if (n <= 0) return 0;
    if (avail <= 0) return -1;
    if (n > avail) n = avail;
    memcpy(dst, s->data + s->pos, n);
    s->pos += n;
    return n;
}
static void IS_readB(Thread *t, Reg *args, Reg *ret) {
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    RET_I(stream_read(STRM(0), ARR_DATA(a), a->length));
}
static void IS_readBII(Thread *t, Reg *args, Reg *ret) {
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), n = ARG_I(3);
    if (off < 0 || n < 0 || off + n > a->length) { throw_new(t, "Ljava/lang/IndexOutOfBoundsException;", NULL); return; }
    RET_I(stream_read(STRM(0), (uint8_t *)ARR_DATA(a) + off, n));
}
static void IS_read(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    uint8_t b;
    int n = stream_read(STRM(0), &b, 1);
    RET_I(n == 1 ? b : -1);
}
static void IS_skip(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    StreamObj *s = src_of(STRM(0));
    int64_t n = ARG_J(1);
    if (!s) { RET_J(0); return; }
    int64_t avail = s->len - s->pos;
    if (n > avail) n = avail;
    if (n < 0) n = 0;
    s->pos += (int)n;
    RET_J(n);
}
static void IS_reset(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; StreamObj *s = src_of(STRM(0)); if (s) s->pos = s->cap; }
static void IS_markI(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; StreamObj *s = src_of(STRM(0)); if (s) s->cap = s->pos; }
static void IS_markSupported(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_I(1); }
static NativeDef InputStream_methods[] = {
    {"<init>", "()V", NULL, 0},
    {"available", "()I", IS_available, 0},
    {"close", "()V", IS_close, 0},
    {"read", "([B)I", IS_readB, 0},
    {"read", "([BII)I", IS_readBII, 0},
    {"read", "()I", IS_read, 0},
    {"skip", "(J)J", IS_skip, 0},
    {"reset", "()V", IS_reset, 0},
    {"mark", "(I)V", IS_markI, 0},
    {"markSupported", "()Z", IS_markSupported, 0},
    {0}};
static void Obj_noop(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void BAIS_init(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    StreamObj *s = STRM(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    s->inner = (Object *)a; s->data = ARR_DATA(a); s->len = a->length; s->pos = 0; s->owned = 0;
}
static void BAIS_initII(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    StreamObj *s = STRM(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), n = ARG_I(3);
    s->inner = (Object *)a; s->data = (uint8_t *)ARR_DATA(a); s->pos = off; s->len = off + n; if (s->len > a->length) s->len = a->length;
}
static NativeDef BAIS_methods[] = {
    {"<init>", "([B)V", BAIS_init, 0},
    {"<init>", "([BII)V", BAIS_initII, 0},
    {0}};
/* note: ByteArrayInputStream keeps the array in `inner` (for GC) and data points into it.
   src_of() stops at the first stream with data, so it is not followed further. */

static void DIS_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; STRM(0)->inner = ARG_L(1); }
static int need(Thread *t, StreamObj *s, uint8_t *dst, int n) {
    StreamObj *src = src_of(s);
    if (!src || !src->data || src->len - src->pos < n) {
        throw_new(t, "Ljava/io/EOFException;", NULL);
        return 0;
    }
    memcpy(dst, src->data + src->pos, n);
    src->pos += n;
    return 1;
}
static void DIS_readByte(Thread *t, Reg *args, Reg *ret) { uint8_t b; if (!need(t, STRM(0), &b, 1)) return; RET_I((int8_t)b); }
static void DIS_readUByte(Thread *t, Reg *args, Reg *ret) { uint8_t b; if (!need(t, STRM(0), &b, 1)) return; RET_I(b); }
static void DIS_readBool(Thread *t, Reg *args, Reg *ret) { uint8_t b; if (!need(t, STRM(0), &b, 1)) return; RET_I(b != 0); }
static void DIS_readShort(Thread *t, Reg *args, Reg *ret) { uint8_t b[2]; if (!need(t, STRM(0), b, 2)) return; RET_I((int16_t)((b[0] << 8) | b[1])); }
static void DIS_readUShort(Thread *t, Reg *args, Reg *ret) { uint8_t b[2]; if (!need(t, STRM(0), b, 2)) return; RET_I((b[0] << 8) | b[1]); }
static void DIS_readChar(Thread *t, Reg *args, Reg *ret) { uint8_t b[2]; if (!need(t, STRM(0), b, 2)) return; RET_I((b[0] << 8) | b[1]); }
static void DIS_readInt(Thread *t, Reg *args, Reg *ret) {
    uint8_t b[4]; if (!need(t, STRM(0), b, 4)) return;
    RET_I((int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]));
}
static void DIS_readLong(Thread *t, Reg *args, Reg *ret) {
    uint8_t b[8]; if (!need(t, STRM(0), b, 8)) return;
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    RET_J((int64_t)v);
}
static void DIS_readFloat(Thread *t, Reg *args, Reg *ret) {
    uint8_t b[4]; if (!need(t, STRM(0), b, 4)) return;
    uint32_t u = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    ret->raw = u;
}
static void DIS_readUTF(Thread *t, Reg *args, Reg *ret) {
    uint8_t b[2]; if (!need(t, STRM(0), b, 2)) return;
    int n = (b[0] << 8) | b[1];
    char *buf = malloc(n + 1);
    if (n && !need(t, STRM(0), (uint8_t *)buf, n)) { free(buf); return; }
    buf[n] = 0;
    RET_L(jstr_from_mutf8(buf));
    free(buf);
}
static void DIS_readFully(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    need(t, STRM(0), ARR_DATA(a), a->length);
}
static void DIS_readFullyII(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    need(t, STRM(0), (uint8_t *)ARR_DATA(a) + ARG_I(2), ARG_I(3));
}
static NativeDef DIS_methods[] = {
    {"<init>", "(Ljava/io/InputStream;)V", DIS_init, 0},
    {"readByte", "()B", DIS_readByte, 0},
    {"readUnsignedByte", "()I", DIS_readUByte, 0},
    {"readBoolean", "()Z", DIS_readBool, 0},
    {"readShort", "()S", DIS_readShort, 0},
    {"readUnsignedShort", "()I", DIS_readUShort, 0},
    {"readChar", "()C", DIS_readChar, 0},
    {"readInt", "()I", DIS_readInt, 0},
    {"readLong", "()J", DIS_readLong, 0},
    {"readFloat", "()F", DIS_readFloat, 0},
    {"readUTF", "()Ljava/lang/String;", DIS_readUTF, 0},
    {"readFully", "([B)V", DIS_readFully, 0},
    {"readFully", "([BII)V", DIS_readFullyII, 0},
    {0}};

/* output streams: byte sink buffered in `data` of the innermost stream */
static StreamObj *sink_of(StreamObj *s) {
    int guard = 0;
    while (s && s->inner && guard++ < 16) s = (StreamObj *)s->inner;
    return s;
}
static void sink_write(StreamObj *s, const uint8_t *b, int n) {
    s = sink_of(s);
    if (!s) return;
    if (s->len + n > s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        while (nc < s->len + n) nc *= 2;
        s->data = realloc(s->data, nc); s->cap = nc; s->owned = 1;
    }
    memcpy(s->data + s->len, b, n);
    s->len += n;
}
static void sink_flush(StreamObj *s) {
    s = sink_of(s);
    if (s && s->fname) host_file_write(s->fname, (int)strlen(s->fname), s->data, s->len);
}
static void OS_close(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; sink_flush(STRM(0)); }
static void OS_flush(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; sink_flush(STRM(0)); }
static void OS_writeI(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; uint8_t b = (uint8_t)ARG_I(1); sink_write(STRM(0), &b, 1); }
static void OS_writeB(Thread *t, Reg *args, Reg *ret) { (void)ret; Array *a = (Array *)ARG_L(1); NPE_CHECK(a); sink_write(STRM(0), ARR_DATA(a), a->length); }
static void OS_writeBII(Thread *t, Reg *args, Reg *ret) { (void)ret; Array *a = (Array *)ARG_L(1); NPE_CHECK(a); sink_write(STRM(0), (uint8_t *)ARR_DATA(a) + ARG_I(2), ARG_I(3)); }
static NativeDef OS_methods[] = {
    {"<init>", "()V", Obj_noop, 0},
    {"close", "()V", OS_close, 0},
    {"flush", "()V", OS_flush, 0},
    {"write", "(I)V", OS_writeI, 0},
    {"write", "([B)V", OS_writeB, 0},
    {"write", "([BII)V", OS_writeBII, 0},
    {0}};
static void DOS_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; STRM(0)->inner = ARG_L(1); }
static void DOS_writeByte(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; uint8_t b = (uint8_t)ARG_I(1); sink_write(STRM(0), &b, 1); }
static void DOS_writeShort(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; uint8_t b[2] = {(uint8_t)(ARG_I(1) >> 8), (uint8_t)ARG_I(1)}; sink_write(STRM(0), b, 2); }
static void DOS_writeInt(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    uint32_t v = (uint32_t)ARG_I(1);
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    sink_write(STRM(0), b, 4);
}
static void DOS_writeFloat(Thread *t, Reg *args, Reg *ret) { Reg a2[2] = {args[0], args[1]}; DOS_writeInt(t, a2, ret); }
static void DOS_writeBool(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; uint8_t b = ARG_I(1) ? 1 : 0; sink_write(STRM(0), &b, 1); }
static void DOS_writeUTF(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    JString *s = (JString *)ARG_L(1); NPE_CHECK(s);
    // modified UTF-8
    uint8_t *buf = malloc(3 * (size_t)s->length + 2); int n = 0;
    for (int i = 0; i < s->length; i++) {
        uint16_t c = s->chars[i];
        if (c && c < 0x80) buf[2 + n++] = (uint8_t)c;
        else if (c < 0x800) { buf[2 + n++] = 0xc0 | (c >> 6); buf[2 + n++] = 0x80 | (c & 0x3f); }
        else { buf[2 + n++] = 0xe0 | (c >> 12); buf[2 + n++] = 0x80 | ((c >> 6) & 0x3f); buf[2 + n++] = 0x80 | (c & 0x3f); }
    }
    buf[0] = (uint8_t)(n >> 8); buf[1] = (uint8_t)n;
    sink_write(STRM(0), buf, n + 2);
    free(buf);
}
static NativeDef DOS_methods[] = {
    {"<init>", "(Ljava/io/OutputStream;)V", DOS_init, 0},
    {"writeByte", "(I)V", DOS_writeByte, 0},
    {"writeBoolean", "(Z)V", DOS_writeBool, 0},
    {"writeShort", "(I)V", DOS_writeShort, 0},
    {"writeInt", "(I)V", DOS_writeInt, 0},
    {"writeFloat", "(F)V", DOS_writeFloat, 0},
    {"writeUTF", "(Ljava/lang/String;)V", DOS_writeUTF, 0},
    {0}};
static void BAOS_toByteArray(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    StreamObj *s = STRM(0);
    Array *a = new_array(C_arrB, s->len);
    if (s->len) memcpy(ARR_DATA(a), s->data, s->len);
    RET_L(a);
}
static void BAOS_size(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(STRM(0)->len); }
static NativeDef BAOS_methods[] = {
    {"<init>", "()V", Obj_noop, 0},
    {"<init>", "(I)V", Obj_noop, 0},
    {"toByteArray", "()[B", BAOS_toByteArray, 0},
    {"size", "()I", BAOS_size, 0},
    {0}};

/* ---------- nio buffers ---------- */
#define BUF(n) ((BufferObj *)ARG_L(n))
static Class *C_ByteBuffer, *C_FloatBuffer, *C_IntBuffer, *C_ShortBuffer, *C_ByteOrder;
static Object *g_order_le, *g_order_be;
static void ensure_nio(void) {
    if (!C_ByteBuffer) {
        C_ByteBuffer = find_class_or_die("Ljava/nio/ByteBuffer;");
        C_FloatBuffer = find_class_or_die("Ljava/nio/FloatBuffer;");
        C_IntBuffer = find_class_or_die("Ljava/nio/IntBuffer;");
        C_ShortBuffer = find_class_or_die("Ljava/nio/ShortBuffer;");
        C_ByteOrder = find_class_or_die("Ljava/nio/ByteOrder;");
    }
}
static void Buf_mark(Object *o) { gc_mark(((BufferObj *)o)->backing); }
static void Buf_fin(Object *o) { BufferObj *b = (BufferObj *)o; if (b->owned) free(b->base); }
void *buf_addr(Object *o) {
    BufferObj *b = (BufferObj *)o;
    if (!b) return NULL;
    return b->base + (size_t)b->position * b->esize;
}
static BufferObj *new_buf(Class *c, Object *backing, uint8_t *base, int cap, int esize) {
    BufferObj *b = (BufferObj *)new_object(c);
    b->backing = backing; b->base = base; b->capacity = cap; b->limit = cap; b->position = 0; b->mark = -1;
    b->esize = esize; b->big_endian = 0;
    return b;
}
static void Buf_position(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); int p = ARG_I(1);
    if (p < 0 || p > b->limit) { throw_new(t, "Ljava/lang/IllegalArgumentException;", NULL); return; }
    b->position = p; RET_L(b);
}
static void Buf_getPosition(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(BUF(0)->position); }
static void Buf_limit(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(BUF(0)->limit); }
static void Buf_setLimit(Thread *t, Reg *args, Reg *ret) { (void)t; BufferObj *b = BUF(0); b->limit = ARG_I(1); if (b->position > b->limit) b->position = b->limit; RET_L(b); }
static void Buf_capacity(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(BUF(0)->capacity); }
static void Buf_remaining(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(BUF(0)->limit - BUF(0)->position); }
static void Buf_rewind(Thread *t, Reg *args, Reg *ret) { (void)t; BUF(0)->position = 0; RET_L(BUF(0)); }
static void Buf_clear(Thread *t, Reg *args, Reg *ret) { (void)t; BUF(0)->position = 0; BUF(0)->limit = BUF(0)->capacity; RET_L(BUF(0)); }
static void Buf_flip(Thread *t, Reg *args, Reg *ret) { (void)t; BUF(0)->limit = BUF(0)->position; BUF(0)->position = 0; RET_L(BUF(0)); }
static void Buf_hasRemaining(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(BUF(0)->position < BUF(0)->limit); }
static NativeDef Buffer_methods[] = {
    {"position", "(I)Ljava/nio/Buffer;", Buf_position, 0},
    {"position", "()I", Buf_getPosition, 0},
    {"limit", "()I", Buf_limit, 0},
    {"limit", "(I)Ljava/nio/Buffer;", Buf_setLimit, 0},
    {"capacity", "()I", Buf_capacity, 0},
    {"remaining", "()I", Buf_remaining, 0},
    {"rewind", "()Ljava/nio/Buffer;", Buf_rewind, 0},
    {"clear", "()Ljava/nio/Buffer;", Buf_clear, 0},
    {"flip", "()Ljava/nio/Buffer;", Buf_flip, 0},
    {"hasRemaining", "()Z", Buf_hasRemaining, 0},
    {0}};

static uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
static uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static int bb_check(Thread *t, BufferObj *b, int idx, int n) {
    if (idx < 0 || idx + n > b->limit) { throw_new(t, "Ljava/nio/BufferUnderflowException;", NULL); return 0; }
    return 1;
}
static uint32_t bb_get32(BufferObj *b, int idx) { uint32_t v; memcpy(&v, b->base + idx, 4); return b->big_endian ? bswap32(v) : v; }
static void bb_put32(BufferObj *b, int idx, uint32_t v) { if (b->big_endian) v = bswap32(v); memcpy(b->base + idx, &v, 4); }

static void BB_allocateDirect(Thread *t, Reg *args, Reg *ret) {
    (void)t; ensure_nio();
    int n = ARG_I(0);
    uint8_t *mem = calloc(1, n + 8);
    BufferObj *b = new_buf(C_ByteBuffer, NULL, mem, n, 1);
    b->owned = 1; b->big_endian = 1;
    RET_L(b);
}
static void BB_allocate(Thread *t, Reg *args, Reg *ret) {
    (void)t; ensure_nio();
    Array *a = new_array(C_arrB, ARG_I(0));
    BufferObj *b = new_buf(C_ByteBuffer, (Object *)a, ARR_DATA(a), a->length, 1);
    b->big_endian = 1;
    RET_L(b);
}
static void BB_wrap(Thread *t, Reg *args, Reg *ret) {
    ensure_nio();
    Array *a = (Array *)ARG_L(0); NPE_CHECK(a);
    BufferObj *b = new_buf(C_ByteBuffer, (Object *)a, ARR_DATA(a), a->length, 1);
    b->big_endian = 1;
    RET_L(b);
}
static void BB_wrapII(Thread *t, Reg *args, Reg *ret) {
    ensure_nio();
    Array *a = (Array *)ARG_L(0); NPE_CHECK(a);
    BufferObj *b = new_buf(C_ByteBuffer, (Object *)a, ARR_DATA(a), a->length, 1);
    b->position = ARG_I(1); b->limit = ARG_I(1) + ARG_I(2);
    b->big_endian = 1;
    RET_L(b);
}
static void BB_order(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    BufferObj *b = BUF(0);
    Object *o = ARG_L(1);
    b->big_endian = (o == g_order_be);
    RET_L(b);
}
static void BB_getOrder(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(BUF(0)->big_endian ? g_order_be : g_order_le); }
static void BB_asView(Thread *t, Reg *args, Reg *ret, Class *c, int es) {
    (void)t;
    BufferObj *b = BUF(0);
    int rem = b->limit - b->position;
    BufferObj *v = new_buf(c, (Object *)b, b->base + b->position, rem / es, es);
    v->big_endian = b->big_endian;
    RET_L(v);
}
static void BB_asFloat(Thread *t, Reg *args, Reg *ret) { ensure_nio(); BB_asView(t, args, ret, C_FloatBuffer, 4); }
static void BB_asInt(Thread *t, Reg *args, Reg *ret) { ensure_nio(); BB_asView(t, args, ret, C_IntBuffer, 4); }
static void BB_asShort(Thread *t, Reg *args, Reg *ret) { ensure_nio(); BB_asView(t, args, ret, C_ShortBuffer, 2); }
static void BB_getInt(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 4)) return;
    RET_I((int32_t)bb_get32(b, b->position)); b->position += 4;
}
static void BB_getIntI(Thread *t, Reg *args, Reg *ret) { BufferObj *b = BUF(0); if (!bb_check(t, b, ARG_I(1), 4)) return; RET_I((int32_t)bb_get32(b, ARG_I(1))); }
static void BB_getFloat(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 4)) return;
    ret->raw = bb_get32(b, b->position); b->position += 4;
}
static void BB_getFloatI(Thread *t, Reg *args, Reg *ret) { BufferObj *b = BUF(0); if (!bb_check(t, b, ARG_I(1), 4)) return; ret->raw = bb_get32(b, ARG_I(1)); }
static void BB_getShort(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 2)) return;
    uint16_t v; memcpy(&v, b->base + b->position, 2); if (b->big_endian) v = bswap16(v);
    RET_I((int16_t)v); b->position += 2;
}
static void BB_get(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 1)) return;
    RET_I((int8_t)b->base[b->position]); b->position++;
}
static void BB_getI(Thread *t, Reg *args, Reg *ret) { BufferObj *b = BUF(0); if (!bb_check(t, b, ARG_I(1), 1)) return; RET_I((int8_t)b->base[ARG_I(1)]); }
static void BB_getArr(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    if (!bb_check(t, b, b->position, a->length)) return;
    memcpy(ARR_DATA(a), b->base + b->position, a->length); b->position += a->length;
    RET_L(b);
}
static void BB_put(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 1)) return;
    b->base[b->position++] = (uint8_t)ARG_I(1); RET_L(b);
}
static void BB_putArr(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    if (!bb_check(t, b, b->position, a->length)) return;
    memcpy(b->base + b->position, ARR_DATA(a), a->length); b->position += a->length;
    RET_L(b);
}
static void BB_putInt(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!bb_check(t, b, b->position, 4)) return;
    bb_put32(b, b->position, (uint32_t)ARG_I(1)); b->position += 4; RET_L(b);
}
static void BB_putFloat(Thread *t, Reg *args, Reg *ret) { BB_putInt(t, args, ret); }
static NativeDef ByteBuffer_methods[] = {
    {"allocateDirect", "(I)Ljava/nio/ByteBuffer;", BB_allocateDirect, 1},
    {"allocate", "(I)Ljava/nio/ByteBuffer;", BB_allocate, 1},
    {"wrap", "([B)Ljava/nio/ByteBuffer;", BB_wrap, 1},
    {"wrap", "([BII)Ljava/nio/ByteBuffer;", BB_wrapII, 1},
    {"order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;", BB_order, 0},
    {"order", "()Ljava/nio/ByteOrder;", BB_getOrder, 0},
    {"asFloatBuffer", "()Ljava/nio/FloatBuffer;", BB_asFloat, 0},
    {"asIntBuffer", "()Ljava/nio/IntBuffer;", BB_asInt, 0},
    {"asShortBuffer", "()Ljava/nio/ShortBuffer;", BB_asShort, 0},
    {"getInt", "()I", BB_getInt, 0},
    {"getInt", "(I)I", BB_getIntI, 0},
    {"getFloat", "()F", BB_getFloat, 0},
    {"getFloat", "(I)F", BB_getFloatI, 0},
    {"getShort", "()S", BB_getShort, 0},
    {"get", "()B", BB_get, 0},
    {"get", "(I)B", BB_getI, 0},
    {"get", "([B)Ljava/nio/ByteBuffer;", BB_getArr, 0},
    {"put", "(B)Ljava/nio/ByteBuffer;", BB_put, 0},
    {"put", "([B)Ljava/nio/ByteBuffer;", BB_putArr, 0},
    {"putInt", "(I)Ljava/nio/ByteBuffer;", BB_putInt, 0},
    {"putFloat", "(F)Ljava/nio/ByteBuffer;", BB_putFloat, 0},
    {0}};

/* typed buffers (Float/Int/Short): element access honours big_endian for views */
static uint32_t tb_get32(BufferObj *b, int i) { uint32_t v; memcpy(&v, b->base + 4 * (size_t)i, 4); return b->big_endian ? bswap32(v) : v; }
static void tb_put32(BufferObj *b, int i, uint32_t v) { if (b->big_endian) v = bswap32(v); memcpy(b->base + 4 * (size_t)i, &v, 4); }
static void TB_wrap(Thread *t, Reg *args, Reg *ret, Class *c, int es) {
    Array *a = (Array *)ARG_L(0); NPE_CHECK(a);
    RET_L(new_buf(c, (Object *)a, ARR_DATA(a), a->length, es));
}
static void TB_wrapII(Thread *t, Reg *args, Reg *ret, Class *c, int es) {
    Array *a = (Array *)ARG_L(0); NPE_CHECK(a);
    BufferObj *b = new_buf(c, (Object *)a, ARR_DATA(a), a->length, es);
    b->position = ARG_I(1); b->limit = ARG_I(1) + ARG_I(2);
    RET_L(b);
}
static void TB_allocate(Thread *t, Reg *args, Reg *ret, Class *c, const char *ad, int es) {
    (void)t;
    Array *a = new_array_desc(ad, ARG_I(0));
    RET_L(new_buf(c, (Object *)a, ARR_DATA(a), a->length, es));
}
static void FB_wrap(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrap(t, args, ret, C_FloatBuffer, 4); }
static void FB_wrapII(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrapII(t, args, ret, C_FloatBuffer, 4); }
static void FB_allocate(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_allocate(t, args, ret, C_FloatBuffer, "[F", 4); }
static void IB_wrap(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrap(t, args, ret, C_IntBuffer, 4); }
static void IB_wrapII(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrapII(t, args, ret, C_IntBuffer, 4); }
static void IB_allocate(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_allocate(t, args, ret, C_IntBuffer, "[I", 4); }
static void SB_wrap(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrap(t, args, ret, C_ShortBuffer, 2); }
static void SB_wrapII(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_wrapII(t, args, ret, C_ShortBuffer, 2); }
static void SB_allocate(Thread *t, Reg *args, Reg *ret) { ensure_nio(); TB_allocate(t, args, ret, C_ShortBuffer, "[S", 2); }
static int tb_check(Thread *t, BufferObj *b, int idx, int n) {
    if (idx < 0 || idx + n > b->limit) { throw_new(t, "Ljava/nio/BufferOverflowException;", NULL); return 0; }
    return 1;
}
static void TB32_putArr(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    if (!tb_check(t, b, b->position, a->length)) return;
    uint32_t *src = ARR_DATA(a);
    if (!b->big_endian) memcpy(b->base + 4 * (size_t)b->position, src, 4 * (size_t)a->length);
    else for (int i = 0; i < a->length; i++) tb_put32(b, b->position + i, src[i]);
    b->position += a->length;
    RET_L(b);
}
static void TB32_putArrII(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), n = ARG_I(3);
    if (!tb_check(t, b, b->position, n)) return;
    uint32_t *src = (uint32_t *)ARR_DATA(a) + off;
    for (int i = 0; i < n; i++) tb_put32(b, b->position + i, src[i]);
    b->position += n;
    RET_L(b);
}
static void TB32_put(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, b->position, 1)) return;
    tb_put32(b, b->position++, (uint32_t)args[1].raw); RET_L(b);
}
static void TB32_putI(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, ARG_I(1), 1)) return;
    tb_put32(b, ARG_I(1), (uint32_t)args[2].raw); RET_L(b);
}
static void TB32_get(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, b->position, 1)) return;
    ret->raw = tb_get32(b, b->position++);
}
static void TB32_getI(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, ARG_I(1), 1)) return;
    ret->raw = tb_get32(b, ARG_I(1));
}
static void TB32_getArr(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    if (!tb_check(t, b, b->position, a->length)) return;
    uint32_t *dst = ARR_DATA(a);
    for (int i = 0; i < a->length; i++) dst[i] = tb_get32(b, b->position + i);
    b->position += a->length;
    RET_L(b);
}
static void SB16_putArr(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    if (!tb_check(t, b, b->position, a->length)) return;
    uint16_t *src = ARR_DATA(a);
    for (int i = 0; i < a->length; i++) { uint16_t v = src[i]; if (b->big_endian) v = bswap16(v); memcpy(b->base + 2 * ((size_t)b->position + i), &v, 2); }
    b->position += a->length;
    RET_L(b);
}
static void SB16_put(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, b->position, 1)) return;
    uint16_t v = (uint16_t)ARG_I(1); if (b->big_endian) v = bswap16(v);
    memcpy(b->base + 2 * (size_t)b->position, &v, 2); b->position++; RET_L(b);
}
static void SB16_getI(Thread *t, Reg *args, Reg *ret) {
    BufferObj *b = BUF(0); if (!tb_check(t, b, ARG_I(1), 1)) return;
    uint16_t v; memcpy(&v, b->base + 2 * (size_t)ARG_I(1), 2); if (b->big_endian) v = bswap16(v);
    RET_I((int16_t)v);
}
static NativeDef FloatBuffer_methods[] = {
    {"wrap", "([F)Ljava/nio/FloatBuffer;", FB_wrap, 1},
    {"wrap", "([FII)Ljava/nio/FloatBuffer;", FB_wrapII, 1},
    {"allocate", "(I)Ljava/nio/FloatBuffer;", FB_allocate, 1},
    {"put", "([F)Ljava/nio/FloatBuffer;", TB32_putArr, 0},
    {"put", "([FII)Ljava/nio/FloatBuffer;", TB32_putArrII, 0},
    {"put", "(F)Ljava/nio/FloatBuffer;", TB32_put, 0},
    {"put", "(IF)Ljava/nio/FloatBuffer;", TB32_putI, 0},
    {"get", "()F", TB32_get, 0},
    {"get", "(I)F", TB32_getI, 0},
    {"get", "([F)Ljava/nio/FloatBuffer;", TB32_getArr, 0},
    {0}};
static NativeDef IntBuffer_methods[] = {
    {"wrap", "([I)Ljava/nio/IntBuffer;", IB_wrap, 1},
    {"wrap", "([III)Ljava/nio/IntBuffer;", IB_wrapII, 1},
    {"allocate", "(I)Ljava/nio/IntBuffer;", IB_allocate, 1},
    {"put", "([I)Ljava/nio/IntBuffer;", TB32_putArr, 0},
    {"put", "([III)Ljava/nio/IntBuffer;", TB32_putArrII, 0},
    {"put", "(I)Ljava/nio/IntBuffer;", TB32_put, 0},
    {"put", "(II)Ljava/nio/IntBuffer;", TB32_putI, 0},
    {"get", "()I", TB32_get, 0},
    {"get", "(I)I", TB32_getI, 0},
    {"get", "([I)Ljava/nio/IntBuffer;", TB32_getArr, 0},
    {0}};
static NativeDef ShortBuffer_methods[] = {
    {"wrap", "([S)Ljava/nio/ShortBuffer;", SB_wrap, 1},
    {"wrap", "([SII)Ljava/nio/ShortBuffer;", SB_wrapII, 1},
    {"allocate", "(I)Ljava/nio/ShortBuffer;", SB_allocate, 1},
    {"put", "([S)Ljava/nio/ShortBuffer;", SB16_putArr, 0},
    {"put", "(S)Ljava/nio/ShortBuffer;", SB16_put, 0},
    {"get", "(I)S", SB16_getI, 0},
    {0}};
static void BO_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    ensure_nio();
    g_order_le = new_object(C_ByteOrder); gc_add_root(&g_order_le);
    g_order_be = new_object(C_ByteOrder); gc_add_root(&g_order_be);
    Field *f = class_find_field(C_ByteOrder, "LITTLE_ENDIAN", NULL); memcpy(C_ByteOrder->statics + f->offset, &g_order_le, sizeof(Object *));
    f = class_find_field(C_ByteOrder, "BIG_ENDIAN", NULL); memcpy(C_ByteOrder->statics + f->offset, &g_order_be, sizeof(Object *));
}
static void BO_native(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_L(g_order_le); }
static void BO_toString(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(jstr_from_utf8(ARG_L(0) == g_order_le ? "LITTLE_ENDIAN" : "BIG_ENDIAN")); }
static NativeDef ByteOrder_methods[] = {
    {"<clinit>", "()V", BO_clinit, 1},
    {"nativeOrder", "()Ljava/nio/ByteOrder;", BO_native, 1},
    {"toString", "()Ljava/lang/String;", BO_toString, 0},
    {0}};

static NativeDef none[] = {{0}};
#define SDEF(n, s, m) {n, s, {0}, sizeof(StreamObj), 0, m, Stream_mark, Stream_fin, {0}}
BuiltinDef io_builtins[] = {
    {"Ljava/io/Closeable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    SDEF("Ljava/io/InputStream;", "Ljava/lang/Object;", InputStream_methods),
    SDEF("Ljava/io/FilterInputStream;", "Ljava/io/InputStream;", none),
    SDEF("Ljava/io/FileInputStream;", "Ljava/io/InputStream;", none),
    SDEF("Ljava/io/ByteArrayInputStream;", "Ljava/io/InputStream;", BAIS_methods),
    SDEF("Landroid/content/res/AssetManager$AssetInputStream;", "Ljava/io/InputStream;", none),
    SDEF("Ljava/io/DataInputStream;", "Ljava/io/FilterInputStream;", DIS_methods),
    SDEF("Ljava/io/BufferedInputStream;", "Ljava/io/FilterInputStream;", DIS_methods),
    SDEF("Ljava/io/OutputStream;", "Ljava/lang/Object;", OS_methods),
    SDEF("Ljava/io/FilterOutputStream;", "Ljava/io/OutputStream;", none),
    SDEF("Ljava/io/FileOutputStream;", "Ljava/io/OutputStream;", none),
    SDEF("Ljava/io/ByteArrayOutputStream;", "Ljava/io/OutputStream;", BAOS_methods),
    SDEF("Ljava/io/DataOutputStream;", "Ljava/io/FilterOutputStream;", DOS_methods),
    {"Ljava/nio/Buffer;", "Ljava/lang/Object;", {0}, sizeof(BufferObj), ACC_ABSTRACT, Buffer_methods, Buf_mark, Buf_fin, {0}},
    {"Ljava/nio/ByteBuffer;", "Ljava/nio/Buffer;", {0}, sizeof(BufferObj), ACC_ABSTRACT, ByteBuffer_methods, Buf_mark, Buf_fin, {0}},
    {"Ljava/nio/FloatBuffer;", "Ljava/nio/Buffer;", {0}, sizeof(BufferObj), ACC_ABSTRACT, FloatBuffer_methods, Buf_mark, Buf_fin, {0}},
    {"Ljava/nio/IntBuffer;", "Ljava/nio/Buffer;", {0}, sizeof(BufferObj), ACC_ABSTRACT, IntBuffer_methods, Buf_mark, Buf_fin, {0}},
    {"Ljava/nio/ShortBuffer;", "Ljava/nio/Buffer;", {0}, sizeof(BufferObj), ACC_ABSTRACT, ShortBuffer_methods, Buf_mark, Buf_fin, {0}},
    {"Ljava/nio/ByteOrder;", "Ljava/lang/Object;", {0}, sizeof(Object), ACC_FINAL, ByteOrder_methods, NULL, NULL, {"LITTLE_ENDIAN:Ljava/nio/ByteOrder;", "BIG_ENDIAN:Ljava/nio/ByteOrder;"}},
    {"Ljava/nio/BufferUnderflowException;", "Ljava/lang/RuntimeException;", {0}, 0, 0, none, NULL, NULL, {0}},
    {"Ljava/nio/BufferOverflowException;", "Ljava/lang/RuntimeException;", {0}, 0, 0, none, NULL, NULL, {0}},
    {0}};
