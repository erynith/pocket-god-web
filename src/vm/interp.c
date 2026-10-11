// Dalvik bytecode interpreter.
#include "vm.h"
#include "dex.h"
#include <limits.h>

Thread **g_threads; int g_nthreads;
Thread *g_cur;

#define STACK_REGS (512 * 1024)
#define MAX_FRAMES 8192

Thread *thread_new(int green) {
    Thread *t = calloc(1, sizeof(Thread));
    t->stack_cap = STACK_REGS;
    t->stack = calloc(t->stack_cap, sizeof(Reg));
    t->frames_cap = MAX_FRAMES;
    t->frames = calloc(t->frames_cap, sizeof(Frame));
    t->green = green;
    t->id = g_nthreads + 1;
    g_threads = realloc(g_threads, sizeof(Thread *) * (g_nthreads + 1));
    g_threads[g_nthreads++] = t;
    return t;
}

void thread_free(Thread *t) {
    for (int i = 0; i < g_nthreads; i++) if (g_threads[i] == t) { g_threads[i] = g_threads[--g_nthreads]; break; }
    free(t->stack); free(t->frames); free(t);
}

typedef struct ThrowableObj { Object h; JString *message; Object *cause; } ThrowableObj;

void throw_obj(Thread *t, Object *ex) { t->exception = ex; }

void throw_new(Thread *t, const char *cls, const char *msg) {
    Class *c = find_class(cls);
    if (!c) c = find_class_or_die("Ljava/lang/RuntimeException;");
    class_init(t, c);
    ThrowableObj *e = (ThrowableObj *)new_object(c);
    if (msg) e->message = jstr_from_utf8(msg);
    t->exception = (Object *)e;
}

static inline Frame *push_frame(Thread *t, Method *m) {
    if (t->nframes >= t->frames_cap - 1 || t->sp + m->regs + 8 > t->stack_cap) {
        throw_new(t, "Ljava/lang/StackOverflowError;", NULL);
        return NULL;
    }
    Frame *f = &t->frames[t->nframes++];
    f->m = m;
    f->regs = t->stack + t->sp;
    f->pc = m->insns;
    t->sp += m->regs;
    return f;
}

static void log_missing(const char *what, const char *cls, const char *name, const char *sig) {
    vm_log("MISSING %s: %s->%s%s", what, cls, name, sig ? sig : "");
}

/* ---- resolution ---- */
static Class *resolve_type(uint32_t idx) {
    Class *c = g_dex->types[idx];
    if (!c) {
        c = find_class(dex_type(g_dex, idx));
        g_dex->types[idx] = c;
    }
    return c;
}

static Field *resolve_field(uint32_t idx) {
    Field *f = g_dex->fres[idx];
    if (f) return f;
    const char *cn, *name, *type;
    dex_field_ref(g_dex, idx, &cn, &name, &type);
    Class *c = find_class(cn);
    if (c) f = class_find_field(c, name, type);
    if (!f) {
        log_missing("field", cn, name, type);
        // create a dummy static field holder so the program can continue
        f = calloc(1, sizeof(Field));
        f->cls = c ? c : C_Object; f->name = name; f->type = type; f->is_static = 1;
        static uint8_t *dummy; if (!dummy) dummy = calloc(1, 64);
        f->offset = 0;
        // map to dummy storage via a fake class statics pointer
        Class *fake = calloc(1, sizeof(Class)); fake->statics = dummy; fake->state = 4; fake->name = "dummy";
        f->cls = fake;
    }
    g_dex->fres[idx] = f;
    return f;
}

static Method *resolve_method(uint32_t idx) {
    Method *m = g_dex->mres[idx];
    if (m) return m;
    const char *cn, *name, *sig;
    dex_method_ref(g_dex, idx, &cn, &name, &sig);
    Class *c = find_class(cn);
    if (c) m = class_find_method_sel(c, g_dex->mref[idx].sel);
    if (!m) { log_missing("method", cn, name, sig); return NULL; }
    g_dex->mres[idx] = m;
    return m;
}

static Method *resolve_virtual(uint32_t idx, Class *rc) {
    if (g_dex->vcache_cls[idx] == rc) return g_dex->vcache_m[idx];
    dex_method_ref(g_dex, idx, NULL, NULL, NULL);
    Method *m = mtab_get(&rc->vt, g_dex->mref[idx].sel);
    if (!m) {
        // fallback: search declared (e.g. methods on arrays / interface-typed builtins)
        m = class_find_method_sel(rc, g_dex->mref[idx].sel);
        if (!m) {
            const char *cn, *name, *sig;
            dex_method_ref(g_dex, idx, &cn, &name, &sig);
            vm_log("MISSING virtual %s->%s%s on %s", cn, name, sig, rc->name);
            return NULL;
        }
    }
    g_dex->vcache_cls[idx] = rc;
    g_dex->vcache_m[idx] = m;
    return m;
}

static int find_handler(Method *m, uint32_t addr, Class *ec) {
    const uint8_t *tp = m->tries_ptr;
    for (int i = 0; i < m->tries; i++, tp += 8) {
        uint32_t start; uint16_t cnt, hoff;
        memcpy(&start, tp, 4); memcpy(&cnt, tp + 4, 2); memcpy(&hoff, tp + 6, 2);
        if (addr < start || addr >= start + cnt) continue;
        const uint8_t *hp = m->handlers_ptr + hoff;
        int32_t size = sleb(&hp);
        int n = size < 0 ? -size : size;
        for (int k = 0; k < n; k++) {
            uint32_t ti = uleb(&hp), a = uleb(&hp);
            Class *cc = resolve_type(ti);
            if (cc && is_subclass(ec, cc)) return (int)a;
        }
        if (size <= 0) return (int)uleb(&hp);
        return -1;
    }
    return -1;
}

static inline int32_t f2i(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}
static inline int32_t d2i(double f) {
    if (f != f) return 0;
    if (f >= 2147483648.0) return INT32_MAX;
    if (f <= -2147483648.0) return INT32_MIN;
    return (int32_t)f;
}
static inline int64_t f2l(double f) {
    if (f != f) return 0;
    if (f >= 9223372036854775808.0) return INT64_MAX;
    if (f <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)f;
}

static inline float RF(Reg r) { float f; uint32_t u = (uint32_t)r.raw; memcpy(&f, &u, 4); return f; }

static void field_read(Reg *dst, uint8_t *p, const char *type) {
    switch (type[0]) {
    case 'Z': R_SETI(*dst, *(uint8_t *)p); break;
    case 'B': R_SETI(*dst, *(int8_t *)p); break;
    case 'C': R_SETI(*dst, *(uint16_t *)p); break;
    case 'S': R_SETI(*dst, *(int16_t *)p); break;
    case 'I': case 'F': { uint32_t u; memcpy(&u, p, 4); dst->raw = u; break; }
    case 'J': case 'D': memcpy(&dst->j, p, 8); break;
    default: { Object *o; memcpy(&o, p, sizeof o); R_SETL(*dst, o); break; }
    }
}
static void field_write(Reg *src, uint8_t *p, const char *type) {
    switch (type[0]) {
    case 'Z': *(uint8_t *)p = (uint8_t)(src->i != 0 ? (src->i & 0xff) : 0); break;
    case 'B': *(int8_t *)p = (int8_t)src->i; break;
    case 'C': *(uint16_t *)p = (uint16_t)src->i; break;
    case 'S': *(int16_t *)p = (int16_t)src->i; break;
    case 'I': case 'F': { uint32_t u = (uint32_t)src->raw; memcpy(p, &u, 4); break; }
    case 'J': case 'D': memcpy(p, &src->j, 8); break;
    default: { Object *o = src->l; memcpy(p, &o, sizeof o); break; }
    }
}

Reg call_method(Thread *t, Method *m, Reg *args) {
    Reg r; r.raw = 0;
    if (!m) { throw_new(t, "Ljava/lang/NoSuchMethodError;", NULL); return r; }
    if (m->native) {
        m->native(t, args, &r);
        return r;
    }
    if (!m->has_code) {
        vm_log("AbstractMethodError %s->%s%s", m->cls->name, m->name, m->sig);
        throw_new(t, "Ljava/lang/AbstractMethodError;", m->name);
        return r;
    }
    if (m->is_static && m->cls->state < 4) { class_init(t, m->cls); if (t->exception) return r; }
    int base = t->nframes;
    Frame *f = push_frame(t, m);
    if (!f) return r;
    if (m->ins) memcpy(f->regs + (m->regs - m->ins), args, m->ins * sizeof(Reg));
    t->ret.raw = 0;
    interp(t, base);
    return t->ret;
}

Reg call_virtual(Thread *t, Object *recv, const char *name, const char *sig, Reg *args) {
    Reg r; r.raw = 0;
    if (!recv) { throw_new(t, "Ljava/lang/NullPointerException;", name); return r; }
    Method *m = find_virtual(recv->cls, name, sig);
    if (!m) {
        m = class_find_method(recv->cls, name, sig);
        if (!m) { vm_log("call_virtual: no %s%s on %s", name, sig, recv->cls->name); throw_new(t, "Ljava/lang/NoSuchMethodError;", name); return r; }
    }
    R_SETL(args[0], recv);
    return call_method(t, m, args);
}

int thread_begin(Thread *t, Method *m, Reg *args) {
    Frame *f = push_frame(t, m);
    if (!f) return 0;
    if (m->ins) memcpy(f->regs + (m->regs - m->ins), args, m->ins * sizeof(Reg));
    return 1;
}

/* Debug: print java stack */
void dump_stack(Thread *t) {
    for (int i = t->nframes - 1; i >= 0; i--) {
        Frame *f = &t->frames[i];
        vm_log("  at %s.%s%s pc=%d", f->m->cls->name, f->m->name, f->m->sig, (int)(f->pc - f->m->insns));
    }
}

long long g_icount;

int interp(Thread *t, int base) {
    Frame *fr;
    Method *m;
    const uint16_t *pc;
    Reg *r;
    Object *caught = NULL;
    t->nesting++;
    Thread *prev = g_cur;
    g_cur = t;

#define LOAD() do { fr = &t->frames[t->nframes - 1]; m = fr->m; pc = fr->pc; r = fr->regs; } while (0)
#define SAVE() (fr->pc = pc)
#define A4 ((ins >> 8) & 0xf)
#define B4 (ins >> 12)
#define AA (ins >> 8)
#define THROW(cls) do { throw_new(t, cls, NULL); goto exception; } while (0)
#define CHECK_EXC() do { if (t->exception) goto exception; } while (0)

    LOAD();
    for (;;) {
        uint16_t ins = *pc;
        g_icount++;
        switch (ins & 0xff) {
        case 0x00: // nop (also payloads, skipped by flow)
            pc += 1; break;
        case 0x01: case 0x04: case 0x07: r[A4] = r[B4]; pc += 1; break;
        case 0x02: case 0x05: case 0x08: r[AA] = r[pc[1]]; pc += 2; break;
        case 0x03: case 0x06: case 0x09: r[pc[1]] = r[pc[2]]; pc += 3; break;
        case 0x0a: case 0x0b: case 0x0c: r[AA] = t->ret; pc += 1; break;
        case 0x0d: R_SETL(r[AA], caught); caught = NULL; pc += 1; break;
        case 0x0e: t->ret.raw = 0; goto do_return;
        case 0x0f: case 0x10: case 0x11: t->ret = r[AA]; goto do_return;
        case 0x12: { int32_t v = (int32_t)(int16_t)ins >> 12; R_SETI(r[A4], v); pc += 1; break; }
        case 0x13: R_SETI(r[AA], (int16_t)pc[1]); pc += 2; break;
        case 0x14: R_SETI(r[AA], (int32_t)(pc[1] | ((uint32_t)pc[2] << 16))); pc += 3; break;
        case 0x15: R_SETI(r[AA], (int32_t)((uint32_t)pc[1] << 16)); pc += 2; break;
        case 0x16: R_SETJ(r[AA], (int16_t)pc[1]); pc += 2; break;
        case 0x17: R_SETJ(r[AA], (int32_t)(pc[1] | ((uint32_t)pc[2] << 16))); pc += 3; break;
        case 0x18: { uint64_t v = pc[1] | ((uint64_t)pc[2] << 16) | ((uint64_t)pc[3] << 32) | ((uint64_t)pc[4] << 48); r[AA].raw = v; pc += 5; break; }
        case 0x19: r[AA].raw = (uint64_t)pc[1] << 48; pc += 2; break;
        case 0x1a: R_SETL(r[AA], const_string(pc[1])); pc += 2; break;
        case 0x1b: R_SETL(r[AA], const_string(pc[1] | ((uint32_t)pc[2] << 16))); pc += 3; break;
        case 0x1c: { Class *c = resolve_type(pc[1]); if (!c) THROW("Ljava/lang/NoClassDefFoundError;"); R_SETL(r[AA], c); pc += 2; break; }
        case 0x1d: case 0x1e: if (!r[AA].l) THROW("Ljava/lang/NullPointerException;"); pc += 1; break;
        case 0x1f: { // check-cast
            Object *o = r[AA].l;
            if (o) {
                Class *c = resolve_type(pc[1]);
                if (c && !is_subclass(o->cls, c)) {
                    vm_log("ClassCastException: %s -> %s", o->cls->name, c->name);
                    THROW("Ljava/lang/ClassCastException;");
                }
            }
            pc += 2; break;
        }
        case 0x20: { // instance-of
            Object *o = r[B4].l;
            int res = 0;
            if (o) { Class *c = resolve_type(pc[1]); res = c && is_subclass(o->cls, c); }
            R_SETI(r[A4], res); pc += 2; break;
        }
        case 0x21: { Array *a = (Array *)r[B4].l; if (!a) THROW("Ljava/lang/NullPointerException;"); R_SETI(r[A4], a->length); pc += 1; break; }
        case 0x22: { // new-instance
            Class *c = resolve_type(pc[1]);
            if (!c) THROW("Ljava/lang/NoClassDefFoundError;");
            if (c->state < 4) { SAVE(); class_init(t, c); CHECK_EXC(); }
            R_SETL(r[AA], new_object(c)); pc += 2; break;
        }
        case 0x23: { // new-array
            Class *c = resolve_type(pc[1]);
            int32_t n = r[B4].i;
            if (n < 0) THROW("Ljava/lang/NegativeArraySizeException;");
            R_SETL(r[A4], new_array(c, n)); pc += 2; break;
        }
        case 0x24: case 0x25: { // filled-new-array
            Class *c = resolve_type(pc[1]);
            int n, regs[256];
            if ((ins & 0xff) == 0x24) {
                n = B4;
                uint16_t w = pc[2];
                regs[0] = w & 0xf; regs[1] = (w >> 4) & 0xf; regs[2] = (w >> 8) & 0xf; regs[3] = w >> 12; regs[4] = A4;
            } else {
                n = AA;
                for (int i = 0; i < n; i++) regs[i] = pc[2] + i;
            }
            Array *a = new_array(c, n);
            for (int i = 0; i < n; i++) {
                if (c->elem_size == 4) ((int32_t *)ARR_DATA(a))[i] = r[regs[i]].i;
                else ((Object **)ARR_DATA(a))[i] = r[regs[i]].l;
            }
            R_SETL(t->ret, a);
            pc += 3; break;
        }
        case 0x26: { // fill-array-data
            Array *a = (Array *)r[AA].l;
            if (!a) THROW("Ljava/lang/NullPointerException;");
            int32_t off = (int32_t)(pc[1] | ((uint32_t)pc[2] << 16));
            const uint16_t *pl = pc + off;
            uint16_t width = pl[1];
            uint32_t size = pl[2] | ((uint32_t)pl[3] << 16);
            if ((int)size > a->length) THROW("Ljava/lang/ArrayIndexOutOfBoundsException;");
            memcpy(ARR_DATA(a), pl + 4, (size_t)width * size);
            pc += 3; break;
        }
        case 0x27: { // throw
            Object *o = r[AA].l;
            if (!o) THROW("Ljava/lang/NullPointerException;");
            t->exception = o;
            goto exception;
        }
        case 0x28: pc += (int8_t)(ins >> 8); break;
        case 0x29: pc += (int16_t)pc[1]; break;
        case 0x2a: pc += (int32_t)(pc[1] | ((uint32_t)pc[2] << 16)); break;
        case 0x2b: { // packed-switch
            const uint16_t *pl = pc + (int32_t)(pc[1] | ((uint32_t)pc[2] << 16));
            uint16_t size = pl[1];
            int32_t first; memcpy(&first, pl + 2, 4);
            int32_t v = r[AA].i;
            int64_t idx = (int64_t)v - first;
            if (idx >= 0 && idx < size) { int32_t tgt; memcpy(&tgt, pl + 4 + 2 * idx, 4); pc += tgt; }
            else pc += 3;
            break;
        }
        case 0x2c: { // sparse-switch
            const uint16_t *pl = pc + (int32_t)(pc[1] | ((uint32_t)pc[2] << 16));
            uint16_t size = pl[1];
            int32_t v = r[AA].i;
            int found = 0;
            for (int i = 0; i < size; i++) {
                int32_t k; memcpy(&k, pl + 2 + 2 * i, 4);
                if (k == v) { int32_t tgt; memcpy(&tgt, pl + 2 + 2 * size + 2 * i, 4); pc += tgt; found = 1; break; }
            }
            if (!found) pc += 3;
            break;
        }
        case 0x2d: case 0x2e: { // cmpl/cmpg-float
            float a = RF(r[pc[1] & 0xff]), b = RF(r[pc[1] >> 8]);
            int res = a > b ? 1 : a == b ? 0 : a < b ? -1 : ((ins & 0xff) == 0x2d ? -1 : 1);
            R_SETI(r[AA], res); pc += 2; break;
        }
        case 0x2f: case 0x30: {
            double a = r[pc[1] & 0xff].d, b = r[pc[1] >> 8].d;
            int res = a > b ? 1 : a == b ? 0 : a < b ? -1 : ((ins & 0xff) == 0x2f ? -1 : 1);
            R_SETI(r[AA], res); pc += 2; break;
        }
        case 0x31: {
            int64_t a = r[pc[1] & 0xff].j, b = r[pc[1] >> 8].j;
            R_SETI(r[AA], a > b ? 1 : a == b ? 0 : -1); pc += 2; break;
        }
        case 0x32: pc += (r[A4].raw == r[B4].raw) ? (int16_t)pc[1] : 2; break;
        case 0x33: pc += (r[A4].raw != r[B4].raw) ? (int16_t)pc[1] : 2; break;
        case 0x34: pc += (r[A4].i < r[B4].i) ? (int16_t)pc[1] : 2; break;
        case 0x35: pc += (r[A4].i >= r[B4].i) ? (int16_t)pc[1] : 2; break;
        case 0x36: pc += (r[A4].i > r[B4].i) ? (int16_t)pc[1] : 2; break;
        case 0x37: pc += (r[A4].i <= r[B4].i) ? (int16_t)pc[1] : 2; break;
        case 0x38: pc += (r[AA].raw == 0) ? (int16_t)pc[1] : 2; break;
        case 0x39: pc += (r[AA].raw != 0) ? (int16_t)pc[1] : 2; break;
        case 0x3a: pc += (r[AA].i < 0) ? (int16_t)pc[1] : 2; break;
        case 0x3b: pc += (r[AA].i >= 0) ? (int16_t)pc[1] : 2; break;
        case 0x3c: pc += (r[AA].i > 0) ? (int16_t)pc[1] : 2; break;
        case 0x3d: pc += (r[AA].i <= 0) ? (int16_t)pc[1] : 2; break;

        case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
        case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
            Array *a = (Array *)r[pc[1] & 0xff].l;
            int32_t i = r[pc[1] >> 8].i;
            if (!a) THROW("Ljava/lang/NullPointerException;");
            if (i < 0 || i >= a->length) {
                vm_log("AIOOBE idx %d len %d in %s.%s", i, a->length, m->cls->name, m->name);
                THROW("Ljava/lang/ArrayIndexOutOfBoundsException;");
            }
            void *d = ARR_DATA(a);
            Reg *x = &r[AA];
            switch (ins & 0xff) {
            case 0x44: x->raw = ((uint32_t *)d)[i]; break;
            case 0x45: x->j = ((int64_t *)d)[i]; break;
            case 0x46: R_SETL(*x, ((Object **)d)[i]); break;
            case 0x47: R_SETI(*x, ((uint8_t *)d)[i]); break;
            case 0x48: R_SETI(*x, ((int8_t *)d)[i]); break;
            case 0x49: R_SETI(*x, ((uint16_t *)d)[i]); break;
            case 0x4a: R_SETI(*x, ((int16_t *)d)[i]); break;
            case 0x4b: ((uint32_t *)d)[i] = (uint32_t)x->raw; break;
            case 0x4c: ((int64_t *)d)[i] = x->j; break;
            case 0x4d: ((Object **)d)[i] = x->l; break;
            case 0x4e: ((uint8_t *)d)[i] = (uint8_t)x->i; break;
            case 0x4f: ((int8_t *)d)[i] = (int8_t)x->i; break;
            case 0x50: ((uint16_t *)d)[i] = (uint16_t)x->i; break;
            case 0x51: ((int16_t *)d)[i] = (int16_t)x->i; break;
            }
            pc += 2; break;
        }
        case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: { // iget*
            Field *f = resolve_field(pc[1]);
            Object *o = r[B4].l;
            if (!o) {
                vm_log("NPE iget %s.%s in %s.%s@%d", f->cls->name, f->name, m->cls->name, m->name, (int)(pc - m->insns));
                THROW("Ljava/lang/NullPointerException;");
            }
            field_read(&r[A4], (uint8_t *)o + f->offset, f->type);
            pc += 2; break;
        }
        case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: { // iput*
            Field *f = resolve_field(pc[1]);
            Object *o = r[B4].l;
            if (!o) {
                vm_log("NPE iput %s.%s in %s.%s@%d", f->cls->name, f->name, m->cls->name, m->name, (int)(pc - m->insns));
                THROW("Ljava/lang/NullPointerException;");
            }
            field_write(&r[A4], (uint8_t *)o + f->offset, f->type);
            pc += 2; break;
        }
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: { // sget*
            Field *f = resolve_field(pc[1]);
            if (f->cls->state < 4) { SAVE(); class_init(t, f->cls); CHECK_EXC(); }
            field_read(&r[AA], f->cls->statics + f->offset, f->type);
            pc += 2; break;
        }
        case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: { // sput*
            Field *f = resolve_field(pc[1]);
            if (f->cls->state < 4) { SAVE(); class_init(t, f->cls); CHECK_EXC(); }
            field_write(&r[AA], f->cls->statics + f->offset, f->type);
            pc += 2; break;
        }
        case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            uint8_t op = ins & 0xff;
            int range = op >= 0x74;
            int kind = range ? op - 0x74 : op - 0x6e; // 0 virt 1 super 2 direct 3 static 4 iface
            uint32_t midx = pc[1];
            int n; int regs[256];
            if (!range) {
                n = B4;
                uint16_t w = pc[2];
                regs[0] = w & 0xf; regs[1] = (w >> 4) & 0xf; regs[2] = (w >> 8) & 0xf; regs[3] = w >> 12; regs[4] = A4;
            } else {
                n = AA;
                for (int i = 0; i < n; i++) regs[i] = pc[2] + i;
            }
            Method *cm;
            if (kind == 3) {
                cm = resolve_method(midx);
                if (!cm) THROW("Ljava/lang/NoSuchMethodError;");
                if (cm->cls->state < 4) { SAVE(); class_init(t, cm->cls); CHECK_EXC(); }
            } else {
                Object *recv = r[regs[0]].l;
                if (!recv) {
                    const char *cn, *nm, *sg; dex_method_ref(g_dex, midx, &cn, &nm, &sg);
                    vm_log("NPE invoking %s->%s in %s.%s@%d", cn, nm, m->cls->name, m->name, (int)(pc - m->insns));
                    THROW("Ljava/lang/NullPointerException;");
                }
                if (kind == 0 || kind == 4) cm = resolve_virtual(midx, recv->cls);
                else if (kind == 2) cm = resolve_method(midx);
                else {
                    // super: lookup starting at referenced class
                    const char *cn; dex_method_ref(g_dex, midx, &cn, NULL, NULL);
                    Class *rc = find_class(cn);
                    cm = rc ? mtab_get(&rc->vt, g_dex->mref[midx].sel) : NULL;
                    if (!cm && rc) cm = class_find_method_sel(rc, g_dex->mref[midx].sel);
                    if (!cm) { const char *nm, *sg; dex_method_ref(g_dex, midx, NULL, &nm, &sg); log_missing("super", cn, nm, sg); }
                }
                if (!cm) THROW("Ljava/lang/NoSuchMethodError;");
            }
            SAVE();
            if (cm->native) {
                Reg argv[256];
                for (int i = 0; i < n; i++) argv[i] = r[regs[i]];
                Reg ret; ret.raw = 0;
                cm->native(t, argv, &ret);
                t->ret = ret;
                // natives may have triggered frame changes? (no) - reload r in case
                CHECK_EXC();
                pc += 3;
                if (t->yield_req) {
                    t->yield_req = 0;
                    if (t->green && t->nesting == 1) { fr->pc = pc; t->nesting--; g_cur = prev; return 1; }
                }
                break;
            }
            if (!cm->has_code) {
                vm_log("abstract call %s->%s%s", cm->cls->name, cm->name, cm->sig);
                THROW("Ljava/lang/AbstractMethodError;");
            }
            Frame *nf = push_frame(t, cm);
            if (!nf) goto exception;
            Reg *dst = nf->regs + (cm->regs - cm->ins);
            for (int i = 0; i < n; i++) dst[i] = r[regs[i]];
            LOAD();
            break;
        }

        /* unary ops */
        case 0x7b: R_SETI(r[A4], -r[B4].i); pc += 1; break;
        case 0x7c: R_SETI(r[A4], ~r[B4].i); pc += 1; break;
        case 0x7d: r[A4].j = -r[B4].j; pc += 1; break;
        case 0x7e: r[A4].j = ~r[B4].j; pc += 1; break;
        case 0x7f: R_SETF(r[A4], -RF(r[B4])); pc += 1; break;
        case 0x80: r[A4].d = -r[B4].d; pc += 1; break;
        case 0x81: r[A4].j = r[B4].i; pc += 1; break;
        case 0x82: R_SETF(r[A4], (float)r[B4].i); pc += 1; break;
        case 0x83: r[A4].d = (double)r[B4].i; pc += 1; break;
        case 0x84: R_SETI(r[A4], (int32_t)r[B4].j); pc += 1; break;
        case 0x85: R_SETF(r[A4], (float)r[B4].j); pc += 1; break;
        case 0x86: r[A4].d = (double)r[B4].j; pc += 1; break;
        case 0x87: R_SETI(r[A4], f2i(RF(r[B4]))); pc += 1; break;
        case 0x88: r[A4].j = f2l(RF(r[B4])); pc += 1; break;
        case 0x89: r[A4].d = (double)RF(r[B4]); pc += 1; break;
        case 0x8a: R_SETI(r[A4], d2i(r[B4].d)); pc += 1; break;
        case 0x8b: r[A4].j = f2l(r[B4].d); pc += 1; break;
        case 0x8c: R_SETF(r[A4], (float)r[B4].d); pc += 1; break;
        case 0x8d: R_SETI(r[A4], (int8_t)r[B4].i); pc += 1; break;
        case 0x8e: R_SETI(r[A4], (uint16_t)r[B4].i); pc += 1; break;
        case 0x8f: R_SETI(r[A4], (int16_t)r[B4].i); pc += 1; break;

        /* binary ops: 23x and 2addr */
#define BINOP_INT(opc, opc2, expr) \
        case opc: { int32_t a = r[pc[1] & 0xff].i, b = r[pc[1] >> 8].i; int dst = AA; pc += 2; expr; break; } \
        case opc2: { int32_t a = r[A4].i, b = r[B4].i; int dst = A4; pc += 1; expr; break; }
#define BINOP_LONG(opc, opc2, expr) \
        case opc: { int64_t a = r[pc[1] & 0xff].j, b = r[pc[1] >> 8].j; int dst = AA; pc += 2; expr; break; } \
        case opc2: { int64_t a = r[A4].j, b = r[B4].j; int dst = A4; pc += 1; expr; break; }
#define BINOP_LSH(opc, opc2, expr) \
        case opc: { int64_t a = r[pc[1] & 0xff].j; int32_t b = r[pc[1] >> 8].i; int dst = AA; pc += 2; expr; break; } \
        case opc2: { int64_t a = r[A4].j; int32_t b = r[B4].i; int dst = A4; pc += 1; expr; break; }
#define BINOP_F(opc, opc2, expr) \
        case opc: { float a = RF(r[pc[1] & 0xff]), b = RF(r[pc[1] >> 8]); int dst = AA; pc += 2; expr; break; } \
        case opc2: { float a = RF(r[A4]), b = RF(r[B4]); int dst = A4; pc += 1; expr; break; }
#define BINOP_D(opc, opc2, expr) \
        case opc: { double a = r[pc[1] & 0xff].d, b = r[pc[1] >> 8].d; int dst = AA; pc += 2; expr; break; } \
        case opc2: { double a = r[A4].d, b = r[B4].d; int dst = A4; pc += 1; expr; break; }

        BINOP_INT(0x90, 0xb0, R_SETI(r[dst], (int32_t)((uint32_t)a + (uint32_t)b)))
        BINOP_INT(0x91, 0xb1, R_SETI(r[dst], (int32_t)((uint32_t)a - (uint32_t)b)))
        BINOP_INT(0x92, 0xb2, R_SETI(r[dst], (int32_t)((uint32_t)a * (uint32_t)b)))
        BINOP_INT(0x93, 0xb3, if (!b) { pc -= ((ins & 0xff) >= 0xb0 ? 1 : 2); THROW("Ljava/lang/ArithmeticException;"); } R_SETI(r[dst], (a == INT32_MIN && b == -1) ? a : a / b))
        BINOP_INT(0x94, 0xb4, if (!b) { pc -= ((ins & 0xff) >= 0xb0 ? 1 : 2); THROW("Ljava/lang/ArithmeticException;"); } R_SETI(r[dst], (b == -1) ? 0 : a % b))
        BINOP_INT(0x95, 0xb5, R_SETI(r[dst], a & b))
        BINOP_INT(0x96, 0xb6, R_SETI(r[dst], a | b))
        BINOP_INT(0x97, 0xb7, R_SETI(r[dst], a ^ b))
        BINOP_INT(0x98, 0xb8, R_SETI(r[dst], (int32_t)((uint32_t)a << (b & 31))))
        BINOP_INT(0x99, 0xb9, R_SETI(r[dst], a >> (b & 31)))
        BINOP_INT(0x9a, 0xba, R_SETI(r[dst], (int32_t)((uint32_t)a >> (b & 31))))
        BINOP_LONG(0x9b, 0xbb, r[dst].j = (int64_t)((uint64_t)a + (uint64_t)b))
        BINOP_LONG(0x9c, 0xbc, r[dst].j = (int64_t)((uint64_t)a - (uint64_t)b))
        BINOP_LONG(0x9d, 0xbd, r[dst].j = (int64_t)((uint64_t)a * (uint64_t)b))
        BINOP_LONG(0x9e, 0xbe, if (!b) { pc -= ((ins & 0xff) >= 0xb0 ? 1 : 2); THROW("Ljava/lang/ArithmeticException;"); } r[dst].j = (a == INT64_MIN && b == -1) ? a : a / b)
        BINOP_LONG(0x9f, 0xbf, if (!b) { pc -= ((ins & 0xff) >= 0xb0 ? 1 : 2); THROW("Ljava/lang/ArithmeticException;"); } r[dst].j = (b == -1) ? 0 : a % b)
        BINOP_LONG(0xa0, 0xc0, r[dst].j = a & b)
        BINOP_LONG(0xa1, 0xc1, r[dst].j = a | b)
        BINOP_LONG(0xa2, 0xc2, r[dst].j = a ^ b)
        BINOP_LSH(0xa3, 0xc3, r[dst].j = (int64_t)((uint64_t)a << (b & 63)))
        BINOP_LSH(0xa4, 0xc4, r[dst].j = a >> (b & 63))
        BINOP_LSH(0xa5, 0xc5, r[dst].j = (int64_t)((uint64_t)a >> (b & 63)))
        BINOP_F(0xa6, 0xc6, R_SETF(r[dst], a + b))
        BINOP_F(0xa7, 0xc7, R_SETF(r[dst], a - b))
        BINOP_F(0xa8, 0xc8, R_SETF(r[dst], a * b))
        BINOP_F(0xa9, 0xc9, R_SETF(r[dst], a / b))
        BINOP_F(0xaa, 0xca, R_SETF(r[dst], fmodf(a, b)))
        BINOP_D(0xab, 0xcb, r[dst].d = a + b)
        BINOP_D(0xac, 0xcc, r[dst].d = a - b)
        BINOP_D(0xad, 0xcd, r[dst].d = a * b)
        BINOP_D(0xae, 0xce, r[dst].d = a / b)
        BINOP_D(0xaf, 0xcf, r[dst].d = fmod(a, b))

        /* lit16 */
        case 0xd0: R_SETI(r[A4], (int32_t)((uint32_t)r[B4].i + (uint32_t)(int16_t)pc[1])); pc += 2; break;
        case 0xd1: R_SETI(r[A4], (int32_t)((uint32_t)(int16_t)pc[1] - (uint32_t)r[B4].i)); pc += 2; break;
        case 0xd2: R_SETI(r[A4], (int32_t)((uint32_t)r[B4].i * (uint32_t)(int16_t)pc[1])); pc += 2; break;
        case 0xd3: { int32_t b = (int16_t)pc[1], a = r[B4].i; if (!b) THROW("Ljava/lang/ArithmeticException;"); R_SETI(r[A4], (a == INT32_MIN && b == -1) ? a : a / b); pc += 2; break; }
        case 0xd4: { int32_t b = (int16_t)pc[1], a = r[B4].i; if (!b) THROW("Ljava/lang/ArithmeticException;"); R_SETI(r[A4], b == -1 ? 0 : a % b); pc += 2; break; }
        case 0xd5: R_SETI(r[A4], r[B4].i & (int16_t)pc[1]); pc += 2; break;
        case 0xd6: R_SETI(r[A4], r[B4].i | (int16_t)pc[1]); pc += 2; break;
        case 0xd7: R_SETI(r[A4], r[B4].i ^ (int16_t)pc[1]); pc += 2; break;
        /* lit8 */
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: case 0xe0: case 0xe1: case 0xe2: {
            int32_t a = r[pc[1] & 0xff].i;
            int32_t b = (int8_t)(pc[1] >> 8);
            int32_t v;
            switch (ins & 0xff) {
            case 0xd8: v = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0xd9: v = (int32_t)((uint32_t)b - (uint32_t)a); break;
            case 0xda: v = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0xdb: if (!b) THROW("Ljava/lang/ArithmeticException;"); v = (a == INT32_MIN && b == -1) ? a : a / b; break;
            case 0xdc: if (!b) THROW("Ljava/lang/ArithmeticException;"); v = b == -1 ? 0 : a % b; break;
            case 0xdd: v = a & b; break;
            case 0xde: v = a | b; break;
            case 0xdf: v = a ^ b; break;
            case 0xe0: v = (int32_t)((uint32_t)a << (b & 31)); break;
            case 0xe1: v = a >> (b & 31); break;
            default: v = (int32_t)((uint32_t)a >> (b & 31)); break;
            }
            R_SETI(r[AA], v);
            pc += 2; break;
        }
        default:
            vm_fatal("bad opcode 0x%02x in %s.%s @%d", ins & 0xff, m->cls->name, m->name, (int)(pc - m->insns));
        }
        continue;

    do_return:
        t->sp -= m->regs;
        t->nframes--;
        if (t->nframes == base) { t->nesting--; g_cur = prev; return 0; }
        LOAD();
        pc += 3;
        continue;

    exception: {
            Object *ex = t->exception;
            for (;;) {
                int h = m->tries ? find_handler(m, (uint32_t)(pc - m->insns), ex->cls) : -1;
                if (h >= 0) {
                    pc = m->insns + h;
                    caught = ex;
                    t->exception = NULL;
                    break;
                }
                t->sp -= m->regs;
                t->nframes--;
                if (t->nframes == base) { t->nesting--; g_cur = prev; return 0; }
                LOAD();
            }
            continue;
        }
    }
}
