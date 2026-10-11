// Dex parsing, class loading/linking, selectors, builtin registry.
#include "vm.h"
#include "dex.h"
#include "host.h"
#include <stdarg.h>

Dex *g_dex;
Class *C_Object, *C_String, *C_Class, *C_Throwable;
Class *C_arrI, *C_arrB, *C_arrC, *C_arrF, *C_arrS, *C_arrZ, *C_arrJ, *C_arrD, *C_arrObj, *C_arrString, *C_arrClass;

/* ---------- logging ---------- */
void vm_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    host_log(buf, n);
}
void vm_fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    host_log("FATAL: ", 7);
    host_log(buf, n);
    host_exit(1);
    abort();
}

/* ---------- generic string hash table ---------- */
static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
typedef struct SMap { int cap, n; const char **keys; void **vals; } SMap;
static void *smap_get(SMap *m, const char *k) {
    if (!m->cap) return NULL;
    uint32_t i = hash_str(k) & (m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], k)) return m->vals[i];
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}
static void smap_put(SMap *m, const char *k, void *v) {
    if ((m->n + 1) * 2 > m->cap) {
        int oc = m->cap; const char **ok = m->keys; void **ov = m->vals;
        m->cap = oc ? oc * 2 : 256;
        m->keys = calloc(m->cap, sizeof(char *));
        m->vals = calloc(m->cap, sizeof(void *));
        m->n = 0;
        for (int i = 0; i < oc; i++) if (ok[i]) smap_put(m, ok[i], ov[i]);
        free(ok); free(ov);
    }
    uint32_t i = hash_str(k) & (m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], k)) { m->vals[i] = v; return; }
        i = (i + 1) & (m->cap - 1);
    }
    m->keys[i] = k; m->vals[i] = v; m->n++;
}

static SMap g_strs;
const char *str_intern(const char *s) {
    const char *r = smap_get(&g_strs, s);
    if (r) return r;
    char *c = strdup(s);
    smap_put(&g_strs, c, c);
    return c;
}

static SMap g_sels;
static int g_nsels;
int sel_intern(const char *name, const char *sig) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s%s", name, sig);
    void *v = smap_get(&g_sels, buf);
    if (v) return (int)(intptr_t)v;
    int id = ++g_nsels;
    smap_put(&g_sels, strdup(buf), (void *)(intptr_t)id);
    return id;
}

/* ---------- method tables ---------- */
void mtab_put(MTab *t, int key, Method *m) {
    if ((t->n + 1) * 2 > t->cap) {
        int oc = t->cap; int *ok = t->keys; Method **ov = t->vals;
        t->cap = oc ? oc * 2 : 16;
        t->keys = calloc(t->cap, sizeof(int));
        t->vals = calloc(t->cap, sizeof(Method *));
        t->n = 0;
        for (int i = 0; i < oc; i++) if (ok[i]) mtab_put(t, ok[i], ov[i]);
        free(ok); free(ov);
    }
    uint32_t i = ((uint32_t)key * 2654435761u) & (t->cap - 1);
    while (t->keys[i]) {
        if (t->keys[i] == key) { t->vals[i] = m; return; }
        i = (i + 1) & (t->cap - 1);
    }
    t->keys[i] = key; t->vals[i] = m; t->n++;
}
Method *mtab_get(MTab *t, int key) {
    if (!t->cap) return NULL;
    uint32_t i = ((uint32_t)key * 2654435761u) & (t->cap - 1);
    while (t->keys[i]) {
        if (t->keys[i] == key) return t->vals[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}
static void mtab_copy(MTab *dst, MTab *src) {
    for (int i = 0; i < src->cap; i++) if (src->keys[i]) mtab_put(dst, src->keys[i], src->vals[i]);
}

/* ---------- dex reading ---------- */
uint32_t uleb(const uint8_t **pp) {
    const uint8_t *p = *pp; uint32_t r = 0; int s = 0;
    for (;;) { uint8_t b = *p++; r |= (uint32_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    *pp = p; return r;
}
int32_t sleb(const uint8_t **pp) {
    const uint8_t *p = *pp; int32_t r = 0; int s = 0; uint8_t b;
    do { b = *p++; r |= (int32_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 32 && (b & 0x40)) r |= -(1 << s);
    *pp = p; return r;
}
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

const char *dex_string(Dex *d, uint32_t idx) {
    const uint8_t *p = d->b + rd32(d->b + d->string_ids_off + 4 * idx);
    uleb(&p);
    return (const char *)p;
}
const char *dex_type(Dex *d, uint32_t idx) {
    return dex_string(d, rd32(d->b + d->type_ids_off + 4 * idx));
}
static void proto_sig(Dex *d, uint32_t pidx, char *out, char *shorty) {
    const uint8_t *p = d->b + d->proto_ids_off + 12 * pidx;
    uint32_t rt = rd32(p + 4), po = rd32(p + 8);
    char *o = out; *o++ = '(';
    char *sh = shorty;
    const char *rts = dex_type(d, rt);
    *sh++ = rts[0] == '[' ? 'L' : rts[0];
    if (po) {
        uint32_t n = rd32(d->b + po);
        for (uint32_t i = 0; i < n; i++) {
            const char *ts = dex_type(d, rd16(d->b + po + 4 + 2 * i));
            size_t l = strlen(ts); memcpy(o, ts, l); o += l;
            *sh++ = ts[0] == '[' ? 'L' : ts[0];
        }
    }
    *o++ = ')';
    strcpy(o, rts);
    *sh = 0;
}
void dex_method_ref(Dex *d, uint32_t idx, const char **cls, const char **name, const char **sig) {
    if (!d->mref[idx].name) {
        const uint8_t *p = d->b + d->method_ids_off + 8 * idx;
        char sigbuf[1024], sh[256];
        proto_sig(d, rd16(p + 2), sigbuf, sh);
        d->mref[idx].cls = dex_type(d, rd16(p));
        d->mref[idx].name = dex_string(d, rd32(p + 4));
        d->mref[idx].sig = str_intern(sigbuf);
        d->mref[idx].shorty = str_intern(sh);
        d->mref[idx].sel = sel_intern(d->mref[idx].name, d->mref[idx].sig);
    }
    if (cls) *cls = d->mref[idx].cls;
    if (name) *name = d->mref[idx].name;
    if (sig) *sig = d->mref[idx].sig;
}
void dex_field_ref(Dex *d, uint32_t idx, const char **cls, const char **name, const char **type) {
    const uint8_t *p = d->b + d->field_ids_off + 8 * idx;
    *cls = dex_type(d, rd16(p));
    *type = dex_type(d, rd16(p + 2));
    *name = dex_string(d, rd32(p + 4));
}

static SMap g_dexclasses;   // descriptor -> classdef idx+1
static SMap g_classes;      // descriptor -> Class*
static SMap g_builtins;     // descriptor -> BuiltinDef*

Dex *dex_open(const uint8_t *b, size_t len) {
    Dex *d = calloc(1, sizeof(Dex));
    d->b = b; d->len = len;
    if (memcmp(b, "dex\n", 4)) vm_fatal("not a dex file");
    d->nstrings = rd32(b + 56); d->string_ids_off = rd32(b + 60);
    d->ntypes = rd32(b + 64); d->type_ids_off = rd32(b + 68);
    d->nprotos = rd32(b + 72); d->proto_ids_off = rd32(b + 76);
    d->nfields = rd32(b + 80); d->field_ids_off = rd32(b + 84);
    d->nmethods = rd32(b + 88); d->method_ids_off = rd32(b + 92);
    d->nclassdefs = rd32(b + 96); d->class_defs_off = rd32(b + 100);
    d->jstrs = calloc(d->nstrings, sizeof(JString *));
    d->types = calloc(d->ntypes, sizeof(Class *));
    d->fres = calloc(d->nfields, sizeof(Field *));
    d->mres = calloc(d->nmethods, sizeof(Method *));
    d->mref = calloc(d->nmethods, sizeof(*d->mref));
    d->vcache_cls = calloc(d->nmethods, sizeof(Class *));
    d->vcache_m = calloc(d->nmethods, sizeof(Method *));
    for (uint32_t i = 0; i < d->nclassdefs; i++) {
        const uint8_t *cd = b + d->class_defs_off + 32 * i;
        smap_put(&g_dexclasses, dex_type(d, rd32(cd)), (void *)(intptr_t)(i + 1));
    }
    return d;
}

/* ---------- field layout helpers ---------- */
static int type_size(const char *t) {
    switch (t[0]) {
    case 'J': case 'D': return 8;
    case 'I': case 'F': return 4;
    case 'S': case 'C': return 2;
    case 'B': case 'Z': return 1;
    default: return (int)sizeof(void *);
    }
}
static int align_up(int v, int a) { return (v + a - 1) & ~(a - 1); }

static int count_arg_words(const char *sig, int is_static) {
    int n = is_static ? 0 : 1;
    const char *p = sig + 1;
    while (*p != ')') {
        if (*p == 'J' || *p == 'D') { n += 2; p++; }
        else if (*p == 'L') { n++; while (*p != ';') p++; p++; }
        else if (*p == '[') { while (*p == '[') p++; if (*p == 'L') { while (*p != ';') p++; } p++; n++; }
        else { n++; p++; }
    }
    return n;
}
static const char *make_shorty(const char *sig) {
    char buf[256]; char *o = buf + 1;
    const char *p = sig + 1;
    while (*p != ')') {
        if (*p == 'L') { *o++ = 'L'; while (*p != ';') p++; p++; }
        else if (*p == '[') { *o++ = 'L'; while (*p == '[') p++; if (*p == 'L') { while (*p != ';') p++; } p++; }
        else *o++ = *p++;
    }
    p++;
    buf[0] = (*p == '[') ? 'L' : *p;
    *o = 0;
    return str_intern(buf);
}

/* ---------- class creation ---------- */
static Class *alloc_class(const char *name) {
    Class *c = calloc(1, sizeof(Class));
    c->h.cls = C_Class;  // may be NULL during bootstrap; patched later
    c->h.gcflags = GC_PERM;
    c->name = str_intern(name);
    c->dex_idx = -1;
    smap_put(&g_classes, c->name, c);
    return c;
}

static void finish_layout(Class *c, int own_start) {
    (void)own_start;
    // inherit ref offsets
    int nsup = c->super ? c->super->nref_offs : 0;
    int nown = 0;
    for (int i = 0; i < c->nfields; i++) if (!c->fields[i].is_static && (c->fields[i].type[0] == 'L' || c->fields[i].type[0] == '[')) nown++;
    c->ref_offs = malloc(sizeof(int) * (nsup + nown + 1));
    if (nsup) memcpy(c->ref_offs, c->super->ref_offs, sizeof(int) * nsup);
    c->nref_offs = nsup;
    int ns = 0;
    for (int i = 0; i < c->nfields; i++) if (c->fields[i].is_static && (c->fields[i].type[0] == 'L' || c->fields[i].type[0] == '[')) ns++;
    c->sref_offs = malloc(sizeof(int) * (ns + 1));
    for (int i = 0; i < c->nfields; i++) {
        Field *f = &c->fields[i];
        if (f->type[0] == 'L' || f->type[0] == '[') {
            if (f->is_static) c->sref_offs[c->nsref_offs++] = f->offset;
            else c->ref_offs[c->nref_offs++] = f->offset;
        }
    }
    if (c->super) {
        if (!c->mark) c->mark = c->super->mark;
        if (!c->fin) c->fin = c->super->fin;
    }
}

static void build_vtable(Class *c) {
    if (c->super) mtab_copy(&c->vt, &c->super->vt);
    for (int i = 0; i < c->nifaces; i++) {
        // interface abstract methods (so lookups through iface-typed builtins resolve)
        MTab *it = &c->ifaces[i]->vt;
        for (int k = 0; k < it->cap; k++) if (it->keys[k] && !mtab_get(&c->vt, it->keys[k])) mtab_put(&c->vt, it->keys[k], it->vals[k]);
    }
    for (int i = 0; i < c->nmethods; i++) {
        Method *m = &c->methods[i];
        if (m->is_static || (m->access & ACC_PRIVATE) || m->name[0] == '<') continue;
        mtab_put(&c->vt, m->sel, m);
    }
}

static void read_encoded_value(Dex *d, const uint8_t **pp, Field *f, uint8_t *dst) {
    const uint8_t *p = *pp;
    uint8_t b = *p++;
    int type = b & 0x1f, arg = b >> 5;
    int sz = arg + 1;
    uint64_t v = 0;
    switch (type) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: { // byte short char int long
        for (int i = 0; i < sz; i++) v |= (uint64_t)p[i] << (8 * i);
        p += sz;
        if (type != 0x03 && sz < 8 && (v >> (8 * sz - 1)) & 1) v |= ~0ULL << (8 * sz);
        break;
    }
    case 0x10: case 0x11: { // float, double
        for (int i = 0; i < sz; i++) v |= (uint64_t)p[i] << (8 * i);
        p += sz;
        int full = type == 0x10 ? 4 : 8;
        v <<= 8 * (full - sz);
        break;
    }
    case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1b: {
        for (int i = 0; i < sz; i++) v |= (uint64_t)p[i] << (8 * i);
        p += sz;
        break;
    }
    case 0x1c: { uint32_t n = uleb(&p); for (uint32_t i = 0; i < n; i++) read_encoded_value(d, &p, NULL, NULL); break; }
    case 0x1d: { uleb(&p); uint32_t n = uleb(&p); for (uint32_t i = 0; i < n; i++) { uleb(&p); read_encoded_value(d, &p, NULL, NULL); } break; }
    case 0x1e: v = 0; break;
    case 0x1f: v = arg; break;
    }
    *pp = p;
    if (!f || !dst) return;
    switch (f->type[0]) {
    case 'Z': case 'B': *(int8_t *)dst = (int8_t)v; break;
    case 'S': case 'C': *(int16_t *)dst = (int16_t)v; break;
    case 'I': *(int32_t *)dst = (int32_t)v; break;
    case 'F': { uint32_t u = (uint32_t)v; memcpy(dst, &u, 4); break; }
    case 'J': case 'D': memcpy(dst, &v, 8); break;
    default:
        if (type == 0x17) { JString *s = const_string((uint32_t)v); memcpy(dst, &s, sizeof(void *)); }
        else if (type == 0x18) { Class *k = find_class(dex_type(d, (uint32_t)v)); memcpy(dst, &k, sizeof(void *)); }
        break;
    }
}

static Class *load_dex_class(int idx) {
    Dex *d = g_dex;
    const uint8_t *cd = d->b + d->class_defs_off + 32 * idx;
    const char *name = dex_type(d, rd32(cd));
    Class *c = alloc_class(name);
    c->dex_idx = idx;
    c->access = rd32(cd + 4);
    c->state = 1;
    uint32_t sup = rd32(cd + 8);
    if (sup != 0xffffffff) {
        c->super = find_class(dex_type(d, sup));
        if (!c->super) vm_fatal("missing superclass %s of %s", dex_type(d, sup), name);
    }
    uint32_t ifo = rd32(cd + 12);
    if (ifo) {
        uint32_t n = rd32(d->b + ifo);
        c->ifaces = calloc(n, sizeof(Class *));
        for (uint32_t i = 0; i < n; i++) {
            Class *ic = find_class(dex_type(d, rd16(d->b + ifo + 4 + 2 * i)));
            if (ic) c->ifaces[c->nifaces++] = ic;
        }
    }
    int off = c->super ? c->super->inst_size : (int)sizeof(Object);
    uint32_t cdo = rd32(cd + 24);
    if (cdo) {
        const uint8_t *p = d->b + cdo;
        uint32_t nsf = uleb(&p), nif = uleb(&p), ndm = uleb(&p), nvm = uleb(&p);
        c->nfields = nsf + nif;
        c->fields = calloc(c->nfields + 1, sizeof(Field));
        uint32_t fidx = 0;
        int soff = 0;
        for (uint32_t i = 0; i < nsf + nif; i++) {
            if (i == nsf) fidx = 0;
            fidx += uleb(&p);
            uint32_t acc = uleb(&p);
            Field *f = &c->fields[i];
            const char *fc;
            dex_field_ref(d, fidx, &fc, &f->name, &f->type);
            f->cls = c; f->access = acc; f->is_static = i < nsf;
            int sz = type_size(f->type);
            if (f->is_static) { soff = align_up(soff, 8); f->offset = soff; soff += 8; }
            else { off = align_up(off, sz); f->offset = off; off += sz; }
        }
        c->statics_size = soff;
        c->statics = calloc(1, soff + 8);
        c->nmethods = ndm + nvm;
        c->methods = calloc(c->nmethods + 1, sizeof(Method));
        uint32_t midx = 0;
        for (uint32_t i = 0; i < ndm + nvm; i++) {
            if (i == ndm) midx = 0;
            midx += uleb(&p);
            uint32_t acc = uleb(&p);
            uint32_t code = uleb(&p);
            Method *m = &c->methods[i];
            const char *mc;
            dex_method_ref(d, midx, &mc, &m->name, &m->sig);
            m->shorty = d->mref[midx].shorty;
            m->sel = d->mref[midx].sel;
            m->cls = c; m->access = acc;
            m->is_static = (acc & ACC_STATIC) != 0;
            m->nargs = count_arg_words(m->sig, m->is_static);
            if (code) {
                const uint8_t *ci = d->b + code;
                m->has_code = 1;
                m->regs = rd16(ci); m->ins = rd16(ci + 2); m->outs = rd16(ci + 4); m->tries = rd16(ci + 6);
                m->insns_size = rd32(ci + 12);
                m->insns = (const uint16_t *)(ci + 16);
                if (m->tries) {
                    const uint8_t *tp = ci + 16 + 2 * m->insns_size;
                    if (m->insns_size & 1) tp += 2;
                    m->tries_ptr = tp;
                    m->handlers_ptr = tp + 8 * m->tries;
                }
            }
        }
    } else {
        c->statics = calloc(1, 8);
    }
    c->inst_size = align_up(off, 8);
    // static initial values
    uint32_t svo = rd32(cd + 28);
    if (svo) {
        const uint8_t *p = d->b + svo;
        uint32_t n = uleb(&p);
        int si = 0;
        for (uint32_t i = 0; i < n; i++) {
            while (si < c->nfields && !c->fields[si].is_static) si++;
            Field *f = si < c->nfields ? &c->fields[si] : NULL;
            read_encoded_value(d, &p, f, f ? c->statics + f->offset : NULL);
            si++;
        }
    }
    finish_layout(c, 0);
    build_vtable(c);
    c->state = 2;
    return c;
}

/* ---------- builtins ---------- */
void register_builtins(BuiltinDef *defs) {
    for (BuiltinDef *b = defs; b->name; b++) smap_put(&g_builtins, b->name, b);
}
BuiltinDef *find_builtin(const char *desc) { return smap_get(&g_builtins, desc); }

static Class *load_builtin(BuiltinDef *b) {
    Class *c = alloc_class(b->name);
    c->builtin = 1;
    c->state = 1;
    c->access = b->access | ACC_PUBLIC;
    if (b->super) {
        c->super = find_class(b->super);
        if (!c->super) vm_fatal("builtin super missing %s", b->super);
    }
    int ni = 0;
    while (ni < 6 && b->ifaces[ni]) ni++;
    if (ni) {
        c->ifaces = calloc(ni, sizeof(Class *));
        for (int i = 0; i < ni; i++) { Class *ic = find_class(b->ifaces[i]); if (ic) c->ifaces[c->nifaces++] = ic; }
    }
    int sz = b->inst_size;
    if (c->super && sz < c->super->inst_size) sz = c->super->inst_size;
    if (sz < (int)sizeof(Object)) sz = sizeof(Object);
    c->inst_size = align_up(sz, 8);
    c->mark = b->mark; c->fin = b->fin;
    int nm = 0;
    if (b->methods) while (b->methods[nm].name) nm++;
    c->nmethods = nm;
    c->methods = calloc(nm + 1, sizeof(Method));
    for (int i = 0; i < nm; i++) {
        Method *m = &c->methods[i];
        NativeDef *nd = &b->methods[i];
        m->cls = c; m->name = str_intern(nd->name); m->sig = str_intern(nd->sig);
        m->sel = sel_intern(m->name, m->sig);
        m->is_static = nd->is_static;
        m->access = ACC_PUBLIC | (nd->is_static ? ACC_STATIC : 0) | (nd->fn ? ACC_NATIVE : ACC_ABSTRACT);
        m->native = nd->fn;
        m->shorty = make_shorty(m->sig);
        m->nargs = count_arg_words(m->sig, m->is_static);
    }
    int nsf = 0;
    while (nsf < 8 && b->sfields[nsf]) nsf++;
    c->nfields = nsf;
    c->fields = calloc(nsf + 1, sizeof(Field));
    for (int i = 0; i < nsf; i++) {
        char tmp[256]; strncpy(tmp, b->sfields[i], 255); tmp[255] = 0;
        char *colon = strchr(tmp, ':'); *colon = 0;
        c->fields[i].cls = c; c->fields[i].name = str_intern(tmp); c->fields[i].type = str_intern(colon + 1);
        c->fields[i].is_static = 1; c->fields[i].offset = i * 8; c->fields[i].access = ACC_PUBLIC | ACC_STATIC;
    }
    c->statics_size = nsf * 8;
    c->statics = calloc(1, nsf * 8 + 8);
    finish_layout(c, 0);
    build_vtable(c);
    c->state = 2;
    return c;
}

/* ---------- primitive & array classes ---------- */
static Class *g_prims[128];
Class *prim_class(char p) {
    if (g_prims[(int)p]) return g_prims[(int)p];
    char n[2] = {p, 0};
    Class *c = calloc(1, sizeof(Class));
    c->h.cls = C_Class; c->h.gcflags = GC_PERM;
    c->name = str_intern(n); c->prim = p; c->dex_idx = -1; c->state = 4;
    c->access = ACC_PUBLIC | ACC_FINAL;
    g_prims[(int)p] = c;
    return c;
}

static Class *make_array_class(const char *desc) {
    Class *elem;
    const char *e = desc + 1;
    if (*e == 'L' || *e == '[') {
        elem = find_class(e);
        if (!elem) return NULL;
    } else elem = prim_class(*e);
    Class *c = alloc_class(desc);
    c->is_array = 1;
    c->elem = elem;
    c->super = C_Object;
    c->access = ACC_PUBLIC | ACC_FINAL;
    c->elem_size = type_size(e);
    c->inst_size = ARR_DATA_OFF;
    finish_layout(c, 0);
    if (C_Object) mtab_copy(&c->vt, &C_Object->vt);
    c->state = 4;
    return c;
}

Class *array_class_of(Class *elem) {
    char buf[512];
    if (elem->prim) snprintf(buf, sizeof buf, "[%c", elem->prim);
    else snprintf(buf, sizeof buf, "[%s", elem->name);
    return find_class(buf);
}

/* ---------- lookup ---------- */
Class *find_class(const char *desc) {
    Class *c = smap_get(&g_classes, desc);
    if (c) return c;
    if (desc[0] == '[') return make_array_class(desc);
    void *di = smap_get(&g_dexclasses, desc);
    if (di) return load_dex_class((int)(intptr_t)di - 1);
    BuiltinDef *b = find_builtin(desc);
    if (b) return load_builtin(b);
    if (desc[0] != 'L') {
        if (desc[1] == 0) return prim_class(desc[0]);
        return NULL;
    }
    // Phantom: unknown external class. Create an empty class so type refs work.
    vm_log("warning: phantom class %s", desc);
    c = alloc_class(desc);
    c->super = C_Object;
    c->inst_size = sizeof(Object);
    c->statics = calloc(1, 8);
    finish_layout(c, 0);
    if (C_Object) mtab_copy(&c->vt, &C_Object->vt);
    c->state = 2;
    return c;
}
Class *find_class_or_die(const char *desc) {
    Class *c = find_class(desc);
    if (!c) vm_fatal("class not found: %s", desc);
    return c;
}

int is_subclass(Class *c, Class *sup) {
    if (c == sup) return 1;
    if (!c) return 0;
    if (sup == C_Object && !c->prim) return 1;
    if (c->is_array) {
        if (!sup->is_array) {
            // arrays implement Cloneable/Serializable; treat as Object only
            return 0;
        }
        if (c->elem->prim || sup->elem->prim) return c->elem == sup->elem;
        return is_subclass(c->elem, sup->elem);
    }
    for (Class *k = c; k; k = k->super) {
        if (k == sup) return 1;
        for (int i = 0; i < k->nifaces; i++) if (is_subclass(k->ifaces[i], sup)) return 1;
    }
    return 0;
}

Method *class_find_method_sel(Class *c, int sel) {
    for (Class *k = c; k; k = k->super) {
        for (int i = 0; i < k->nmethods; i++) if (k->methods[i].sel == sel) return &k->methods[i];
    }
    // interfaces (for abstract classes / interface static lookups)
    for (Class *k = c; k; k = k->super)
        for (int i = 0; i < k->nifaces; i++) {
            Method *m = class_find_method_sel(k->ifaces[i], sel);
            if (m) return m;
        }
    return NULL;
}
Method *class_find_method(Class *c, const char *name, const char *sig) {
    return class_find_method_sel(c, sel_intern(name, sig));
}
Field *class_find_field(Class *c, const char *name, const char *type) {
    for (Class *k = c; k; k = k->super) {
        for (int i = 0; i < k->nfields; i++)
            if (!strcmp(k->fields[i].name, name) && (!type || !strcmp(k->fields[i].type, type))) return &k->fields[i];
        for (int j = 0; j < k->nifaces; j++) {
            Field *f = class_find_field(k->ifaces[j], name, type);
            if (f) return f;
        }
    }
    return NULL;
}
Method *find_virtual(Class *c, const char *name, const char *sig) {
    return mtab_get(&c->vt, sel_intern(name, sig));
}

/* ---------- class init ---------- */
int class_init(Thread *t, Class *c) {
    if (c->state >= 4) return 1;
    if (c->state == 3) return 1; // being initialized (recursive or by this thread)
    c->state = 3;
    if (c->super && !class_init(t, c->super)) return 0;
    Method *cl = NULL;
    for (int i = 0; i < c->nmethods; i++) if (!strcmp(c->methods[i].name, "<clinit>")) { cl = &c->methods[i]; break; }
    if (cl) {
        call_method(t, cl, NULL);
        if (t->exception) {
            vm_log("exception in <clinit> of %s: %s", c->name, t->exception->cls->name);
            c->state = 4;
            return 0;
        }
    }
    c->state = 4;
    return 1;
}

/* ---------- resources R$type ---------- */
typedef struct ResEnt { int id; const char *type; const char *name; } ResEnt;
static ResEnt *g_res; static int g_nres;
static void scan_resources(void) {
    Dex *d = g_dex;
    for (uint32_t i = 0; i < d->nclassdefs; i++) {
        const uint8_t *cd = d->b + d->class_defs_off + 32 * i;
        const char *name = dex_type(d, rd32(cd));
        const char *r = strstr(name, "/R$");
        if (!r) continue;
        char type[64]; snprintf(type, sizeof type, "%s", r + 3);
        char *semi = strchr(type, ';'); if (semi) *semi = 0;
        uint32_t cdo = rd32(cd + 24), svo = rd32(cd + 28);
        if (!cdo || !svo) continue;
        const uint8_t *p = d->b + cdo;
        uint32_t nsf = uleb(&p); uleb(&p); uleb(&p); uleb(&p);
        const uint8_t *sv = d->b + svo;
        uint32_t nv = uleb(&sv);
        uint32_t fidx = 0;
        for (uint32_t k = 0; k < nsf; k++) {
            fidx += uleb(&p); uleb(&p);
            const char *fc, *fn, *ft;
            dex_field_ref(d, fidx, &fc, &fn, &ft);
            if (k < nv && ft[0] == 'I') {
                Field tmpf = {0}; tmpf.type = "I"; int32_t v = 0;
                read_encoded_value(d, &sv, &tmpf, (uint8_t *)&v);
                g_res = realloc(g_res, sizeof(ResEnt) * (g_nres + 1));
                g_res[g_nres].id = v; g_res[g_nres].type = str_intern(type); g_res[g_nres].name = fn;
                g_nres++;
            } else if (k < nv) {
                read_encoded_value(d, &sv, NULL, NULL);
            }
        }
    }
    vm_log("resources: %d ids", g_nres);
}
const char *res_name_for_id(int id, const char **type) {
    for (int i = 0; i < g_nres; i++) if (g_res[i].id == id) { if (type) *type = g_res[i].type; return g_res[i].name; }
    return NULL;
}

void dex_boot(const uint8_t *b, size_t len) {
    g_dex = dex_open(b, len);
    register_builtins(lang_builtins);
    register_builtins(util_builtins);
    register_builtins(io_builtins);
    register_builtins(android_builtins);
    register_builtins(xml_builtins);
    C_Object = find_class_or_die("Ljava/lang/Object;");
    C_Class = find_class_or_die("Ljava/lang/Class;");
    C_Object->h.cls = C_Class; C_Class->h.cls = C_Class;
    // patch classes created before C_Class existed
    for (int i = 0; i < g_classes.cap; i++) if (g_classes.keys[i]) ((Class *)g_classes.vals[i])->h.cls = C_Class;
    for (int i = 0; i < 128; i++) if (g_prims[i]) g_prims[i]->h.cls = C_Class;
    C_String = find_class_or_die("Ljava/lang/String;");
    C_Throwable = find_class_or_die("Ljava/lang/Throwable;");
    C_arrI = find_class("[I"); C_arrB = find_class("[B"); C_arrC = find_class("[C"); C_arrF = find_class("[F");
    C_arrS = find_class("[S"); C_arrZ = find_class("[Z"); C_arrJ = find_class("[J"); C_arrD = find_class("[D");
    C_arrObj = find_class("[Ljava/lang/Object;"); C_arrString = find_class("[Ljava/lang/String;");
    C_arrClass = find_class("[Ljava/lang/Class;");
    scan_resources();
}

/* Iterate all loaded classes (for GC roots). */
void for_each_class(void (*fn)(Class *c)) {
    for (int i = 0; i < g_classes.cap; i++) if (g_classes.keys[i]) fn((Class *)g_classes.vals[i]);
}
