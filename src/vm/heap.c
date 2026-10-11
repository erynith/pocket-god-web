// Mark-sweep heap. Collection only happens at safepoints (between frames),
// where the only live interpreter state is suspended green threads (scanned conservatively).
#include "vm.h"
#include "dex.h"

static Object **g_objs; static size_t g_nobjs, g_capobjs;
static size_t g_since, g_live;
static Object ***g_roots; static int g_nroots, g_caproots;
static Object **g_mstack; static size_t g_msp, g_mcap;
size_t g_heap_bytes;

extern Thread **g_threads; extern int g_nthreads;
void extra_roots(void);  // provided by android/natives

void *gc_alloc(Class *c, size_t size) {
    Object *o = calloc(1, size);
    if (!o) vm_fatal("out of memory allocating %zu bytes", size);
    o->cls = c;
    if (g_nobjs == g_capobjs) {
        g_capobjs = g_capobjs ? g_capobjs * 2 : 65536;
        g_objs = realloc(g_objs, g_capobjs * sizeof(Object *));
    }
    g_objs[g_nobjs++] = o;
    g_since += size;
    g_heap_bytes += size;
    return o;
}

Object *new_object(Class *c) {
    return gc_alloc(c, c->inst_size);
}

Array *new_array(Class *ac, int len) {
    if (len < 0) len = 0;
    size_t sz = ARR_DATA_OFF + (size_t)len * ac->elem_size;
    Array *a = gc_alloc(ac, sz);
    a->length = len;
    return a;
}
Array *new_array_desc(const char *desc, int len) {
    return new_array(find_class_or_die(desc), len);
}

static uint32_t g_hashseed = 0x12345;
uint32_t obj_identity_hash(Object *o) {
    if (!o->hash) {
        g_hashseed = g_hashseed * 1103515245u + 12345u;
        o->hash = (g_hashseed >> 1) | 1;
    }
    return o->hash;
}

void gc_add_root(Object **slot) {
    if (g_nroots == g_caproots) {
        g_caproots = g_caproots ? g_caproots * 2 : 64;
        g_roots = realloc(g_roots, g_caproots * sizeof(Object **));
    }
    g_roots[g_nroots++] = slot;
}

void gc_mark(Object *o) {
    if (!o || (o->gcflags & (GC_MARK | GC_PERM))) return;
    o->gcflags |= GC_MARK;
    if (g_msp == g_mcap) {
        g_mcap = g_mcap ? g_mcap * 2 : 4096;
        g_mstack = realloc(g_mstack, g_mcap * sizeof(Object *));
    }
    g_mstack[g_msp++] = o;
}

static void scan(Object *o) {
    Class *c = o->cls;
    if (c->is_array) {
        if (!c->elem->prim) {
            Array *a = (Array *)o;
            Object **e = ARR_DATA(a);
            for (int i = 0; i < a->length; i++) gc_mark(e[i]);
        }
        return;
    }
    for (int i = 0; i < c->nref_offs; i++) gc_mark(*(Object **)((uint8_t *)o + c->ref_offs[i]));
    if (c->mark) c->mark(o);
}

static int cmp_ptr(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)*(Object **)a, y = (uintptr_t)*(Object **)b;
    return x < y ? -1 : x > y;
}
static int g_sorted;
int gc_is_heap_ptr(void *p) {
    if (!g_sorted) return 0;
    size_t lo = 0, hi = g_nobjs;
    uintptr_t v = (uintptr_t)p;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uintptr_t m = (uintptr_t)g_objs[mid];
        if (m == v) return 1;
        if (m < v) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static void mark_conservative(uint64_t raw) {
    void *p = (void *)(uintptr_t)raw;
    if ((uintptr_t)p & 3) return;
    if (gc_is_heap_ptr(p)) gc_mark((Object *)p);
}

static void mark_class_statics(Class *c) {
    for (int i = 0; i < c->nsref_offs; i++) gc_mark(*(Object **)(c->statics + c->sref_offs[i]));
}

void gc_collect(void) {
    qsort(g_objs, g_nobjs, sizeof(Object *), cmp_ptr);
    g_sorted = 1;
    for_each_class(mark_class_statics);
    for (uint32_t i = 0; i < g_dex->nstrings; i++) gc_mark((Object *)g_dex->jstrs[i]);
    for (int i = 0; i < g_nroots; i++) gc_mark(*g_roots[i]);
    for (int i = 0; i < g_nthreads; i++) {
        Thread *t = g_threads[i];
        for (size_t k = 0; k < t->sp; k++) mark_conservative(t->stack[k].raw);
        mark_conservative(t->ret.raw);
        gc_mark(t->exception); gc_mark(t->jthread); gc_mark(t->runnable);
    }
    extra_roots();
    while (g_msp) scan(g_mstack[--g_msp]);
    size_t w = 0, freed = 0, live = 0;
    for (size_t i = 0; i < g_nobjs; i++) {
        Object *o = g_objs[i];
        if (o->gcflags & GC_MARK) {
            o->gcflags &= ~GC_MARK;
            g_objs[w++] = o;
            live++;
        } else {
            if (o->cls->fin) o->cls->fin(o);
            free(o);
            freed++;
        }
    }
    g_nobjs = w;
    g_sorted = 0;
    g_since = 0;
    g_live = live;
    (void)freed;
}

size_t gc_bytes_since(void) { return g_since; }
size_t gc_object_count(void) { return g_nobjs; }

/* ---------- strings ---------- */
JString *jstr_new(int len) {
    JString *s = gc_alloc(C_String, sizeof(JString) + 2 * (size_t)(len > 0 ? len : 0) + 2);
    s->length = len;
    s->chars = JSTR_INLINE(s);
    return s;
}
JString *jstr_from_utf16(const uint16_t *c, int n) {
    JString *s = jstr_new(n);
    if (n && c) memcpy(s->chars, c, 2 * n);
    return s;
}
static int decode_utf8(const char *s, uint16_t *out, int mutf) {
    const uint8_t *p = (const uint8_t *)s;
    int n = 0;
    while (*p) {
        uint32_t c = *p++;
        if (c < 0x80) {}
        else if ((c & 0xe0) == 0xc0) { c = ((c & 0x1f) << 6) | (*p++ & 0x3f); }
        else if ((c & 0xf0) == 0xe0) { c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
        else if (!mutf && (c & 0xf8) == 0xf0) {
            c = ((c & 0x07) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
            c -= 0x10000;
            if (out) { out[n] = 0xd800 + (c >> 10); out[n + 1] = 0xdc00 + (c & 0x3ff); }
            n += 2; continue;
        }
        if (out) out[n] = (uint16_t)c;
        n++;
    }
    return n;
}
JString *jstr_from_utf8(const char *s) {
    int n = decode_utf8(s, NULL, 0);
    JString *j = jstr_new(n);
    decode_utf8(s, j->chars, 0);
    return j;
}
JString *jstr_from_mutf8(const char *s) {
    int n = decode_utf8(s, NULL, 1);
    JString *j = jstr_new(n);
    decode_utf8(s, j->chars, 1);
    return j;
}
char *jstr_to_utf8(JString *s) {
    char *o = malloc(3 * (size_t)s->length + 1), *p = o;
    for (int i = 0; i < s->length; i++) {
        uint32_t c = s->chars[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s->length) {
            uint32_t c2 = s->chars[i + 1];
            if (c2 >= 0xdc00 && c2 < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00); i++;
                *p++ = 0xf0 | (c >> 18); *p++ = 0x80 | ((c >> 12) & 0x3f); *p++ = 0x80 | ((c >> 6) & 0x3f); *p++ = 0x80 | (c & 0x3f);
                continue;
            }
        }
        if (c < 0x80 && c) *p++ = c;
        else if (c < 0x800) { *p++ = 0xc0 | (c >> 6); *p++ = 0x80 | (c & 0x3f); }
        else { *p++ = 0xe0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 0x3f); *p++ = 0x80 | (c & 0x3f); }
    }
    *p = 0;
    return o;
}
int jstr_eq(JString *a, JString *b) {
    if (a == b) return 1;
    if (!a || !b || a->length != b->length) return 0;
    return !memcmp(a->chars, b->chars, 2 * (size_t)a->length);
}
int jstr_eq_c(JString *a, const char *c) {
    int n = (int)strlen(c);
    if (a->length != n) return 0;
    for (int i = 0; i < n; i++) if (a->chars[i] != (uint8_t)c[i]) return 0;
    return 1;
}
int32_t jstr_hash(JString *s) {
    if (!s->hash_set) {
        uint32_t h = 0;
        for (int i = 0; i < s->length; i++) h = 31 * h + s->chars[i];
        s->hash = (int32_t)h; s->hash_set = 1;
    }
    return s->hash;
}
JString *jstr_concat(JString *a, JString *b) {
    JString *s = jstr_new(a->length + b->length);
    memcpy(s->chars, a->chars, 2 * (size_t)a->length);
    memcpy(s->chars + a->length, b->chars, 2 * (size_t)b->length);
    return s;
}
JString *const_string(int idx) {
    JString *s = g_dex->jstrs[idx];
    if (!s) {
        s = jstr_from_mutf8(dex_string(g_dex, idx));
        g_dex->jstrs[idx] = s;
    }
    return s;
}
