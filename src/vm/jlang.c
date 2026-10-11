// java.lang.* natives
#include "vm.h"
#include "dex.h"
#include "host.h"
#include "objs.h"

/* ---------- boxing ---------- */
static Class *C_Integer, *C_Float, *C_Boolean, *C_Long, *C_Double;
static Object *g_intcache[256];
static Object *g_true, *g_false;

static void ensure_box_classes(void) {
    if (!C_Integer) {
        C_Integer = find_class_or_die("Ljava/lang/Integer;");
        C_Float = find_class_or_die("Ljava/lang/Float;");
        C_Boolean = find_class_or_die("Ljava/lang/Boolean;");
        C_Long = find_class_or_die("Ljava/lang/Long;");
        C_Double = find_class_or_die("Ljava/lang/Double;");
    }
}
Object *box_int(int32_t v) {
    ensure_box_classes();
    if (v >= -128 && v < 128) {
        Object **slot = &g_intcache[v + 128];
        if (!*slot) { BoxObj *b = (BoxObj *)new_object(C_Integer); b->v.i = v; *slot = (Object *)b; gc_add_root(slot); }
        return *slot;
    }
    BoxObj *b = (BoxObj *)new_object(C_Integer); b->v.i = v; return (Object *)b;
}
Object *box_float(float v) { ensure_box_classes(); BoxObj *b = (BoxObj *)new_object(C_Float); b->v.f = v; return (Object *)b; }
Object *box_long(int64_t v) { ensure_box_classes(); BoxObj *b = (BoxObj *)new_object(C_Long); b->v.j = v; return (Object *)b; }
Object *box_double(double v) { ensure_box_classes(); BoxObj *b = (BoxObj *)new_object(C_Double); b->v.d = v; return (Object *)b; }
Object *box_bool(int v) {
    ensure_box_classes();
    Object **slot = v ? &g_true : &g_false;
    if (!*slot) { BoxObj *b = (BoxObj *)new_object(C_Boolean); b->v.i = v ? 1 : 0; *slot = (Object *)b; gc_add_root(slot); }
    return *slot;
}
int32_t unbox_int(Object *o) {
    if (!o) return 0;
    ensure_box_classes();
    BoxObj *b = (BoxObj *)o;
    if (o->cls == C_Float) return (int32_t)b->v.f;
    if (o->cls == C_Long) return (int32_t)b->v.j;
    if (o->cls == C_Double) return (int32_t)b->v.d;
    return b->v.i;
}
float unbox_float(Object *o) {
    if (!o) return 0;
    ensure_box_classes();
    BoxObj *b = (BoxObj *)o;
    if (o->cls == C_Float) return b->v.f;
    if (o->cls == C_Long) return (float)b->v.j;
    if (o->cls == C_Double) return (float)b->v.d;
    return (float)b->v.i;
}
static int64_t unbox_long(Object *o) {
    ensure_box_classes();
    BoxObj *b = (BoxObj *)o;
    if (o->cls == C_Long) return b->v.j;
    if (o->cls == C_Float) return (int64_t)b->v.f;
    if (o->cls == C_Double) return (int64_t)b->v.d;
    return b->v.i;
}
static double unbox_double(Object *o) {
    ensure_box_classes();
    BoxObj *b = (BoxObj *)o;
    if (o->cls == C_Double) return b->v.d;
    if (o->cls == C_Float) return b->v.f;
    if (o->cls == C_Long) return (double)b->v.j;
    return b->v.i;
}

/* ---------- number formatting (Java semantics) ---------- */
static void java_float_str(double v, int is_float, char *out) {
    if (v != v) { strcpy(out, "NaN"); return; }
    if (isinf(v)) { strcpy(out, v > 0 ? "Infinity" : "-Infinity"); return; }
    if (v == 0) { strcpy(out, signbit(v) ? "-0.0" : "0.0"); return; }
    char buf[64];
    int prec;
    int maxp = is_float ? 9 : 17;
    for (prec = 1; prec <= maxp; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (is_float) { if ((float)strtod(buf, NULL) == (float)v) break; }
        else if (strtod(buf, NULL) == v) break;
    }
    // buf like "-1.2345e+02"
    char digits[32]; int nd = 0; int neg = 0; const char *p = buf;
    if (*p == '-') { neg = 1; p++; }
    while (*p && *p != 'e') { if (*p >= '0' && *p <= '9') digits[nd++] = *p; p++; }
    int exp = atoi(p + 1);
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    char *o = out;
    if (neg) *o++ = '-';
    double av = fabs(v);
    if (av >= 1e-3 && av < 1e7) {
        if (exp >= 0) {
            for (int i = 0; i <= exp; i++) *o++ = i < nd ? digits[i] : '0';
            *o++ = '.';
            if (nd > exp + 1) for (int i = exp + 1; i < nd; i++) *o++ = digits[i];
            else *o++ = '0';
        } else {
            *o++ = '0'; *o++ = '.';
            for (int i = 0; i < -exp - 1; i++) *o++ = '0';
            for (int i = 0; i < nd; i++) *o++ = digits[i];
        }
        *o = 0;
    } else {
        *o++ = digits[0]; *o++ = '.';
        if (nd > 1) for (int i = 1; i < nd; i++) *o++ = digits[i];
        else *o++ = '0';
        sprintf(o, "E%d", exp);
    }
}

JString *obj_to_string(Thread *t, Object *o) {
    if (!o) return jstr_from_utf8("null");
    if (o->cls == C_String) return (JString *)o;
    Reg a[1];
    Reg r = call_virtual(t, o, "toString", "()Ljava/lang/String;", a);
    if (t->exception) return jstr_from_utf8("null");
    return r.l ? (JString *)r.l : jstr_from_utf8("null");
}

static void class_java_name(Class *c, char *out, size_t cap) {
    const char *n = c->name;
    if (n[0] == 'L') {
        size_t l = strlen(n);
        snprintf(out, cap, "%.*s", (int)(l - 2), n + 1);
    } else snprintf(out, cap, "%s", n);
    for (char *p = out; *p; p++) if (*p == '/') *p = '.';
}

/* ---------- Object ---------- */
static void Object_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void Object_getClass(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(ARG_L(0)->cls); }
static void Object_hashCode(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I((int32_t)obj_identity_hash(ARG_L(0))); }
static void Object_equals(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ARG_L(0) == ARG_L(1)); }
static void Object_toString(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    char n[256], b[300];
    class_java_name(ARG_L(0)->cls, n, sizeof n);
    snprintf(b, sizeof b, "%s@%x", n, obj_identity_hash(ARG_L(0)));
    RET_L(jstr_from_utf8(b));
}
static void Object_clone(Thread *t, Reg *args, Reg *ret) {
    Object *o = ARG_L(0);
    if (o->cls->is_array) {
        Array *a = (Array *)o;
        Array *n = new_array(o->cls, a->length);
        memcpy(ARR_DATA(n), ARR_DATA(a), (size_t)a->length * o->cls->elem_size);
        RET_L(n);
        return;
    }
    if (o->cls->builtin) { throw_new(t, "Ljava/lang/CloneNotSupportedException;", NULL); return; }
    Object *n = new_object(o->cls);
    memcpy((uint8_t *)n + sizeof(Object), (uint8_t *)o + sizeof(Object), o->cls->inst_size - sizeof(Object));
    RET_L(n);
}
static void Object_notify(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static NativeDef Object_methods[] = {
    {"<init>", "()V", Object_init, 0},
    {"getClass", "()Ljava/lang/Class;", Object_getClass, 0},
    {"hashCode", "()I", Object_hashCode, 0},
    {"equals", "(Ljava/lang/Object;)Z", Object_equals, 0},
    {"toString", "()Ljava/lang/String;", Object_toString, 0},
    {"clone", "()Ljava/lang/Object;", Object_clone, 0},
    {"notify", "()V", Object_notify, 0},
    {"notifyAll", "()V", Object_notify, 0},
    {"wait", "()V", Object_notify, 0},
    {"wait", "(J)V", Object_notify, 0},
    {0}};

/* ---------- Class ---------- */
static void Class_forName(Thread *t, Reg *args, Reg *ret) {
    JString *s = (JString *)ARG_L(0);
    NPE_CHECK(s);
    char *u = jstr_to_utf8(s);
    char buf[512];
    if (u[0] == '[') snprintf(buf, sizeof buf, "%s", u); else snprintf(buf, sizeof buf, "L%s;", u);
    for (char *p = buf; *p; p++) if (*p == '.') *p = '/';
    free(u);
    Class *c = find_class(buf);
    if (!c) { throw_new(t, "Ljava/lang/ClassNotFoundException;", buf); return; }
    class_init(t, c);
    RET_L(c);
}
static void Class_getName(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    char n[256];
    class_java_name((Class *)ARG_L(0), n, sizeof n);
    RET_L(jstr_from_utf8(n));
}
static void Class_getSimpleName(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    char n[256];
    class_java_name((Class *)ARG_L(0), n, sizeof n);
    char *d = strrchr(n, '.');
    char *s = d ? d + 1 : n;
    char *dl = strrchr(s, '$'); if (dl) s = dl + 1;
    RET_L(jstr_from_utf8(s));
}
static Class *C_Method;
static void Class_getMethod(Thread *t, Reg *args, Reg *ret) {
    Class *c = (Class *)ARG_L(0);
    JString *name = (JString *)ARG_L(1);
    Array *types = (Array *)ARG_L(2);
    NPE_CHECK(name);
    char *n = jstr_to_utf8(name);
    char prefix[1024]; char *o = prefix; *o++ = '(';
    if (types) {
        Class **tc = ARR_DATA(types);
        for (int i = 0; i < types->length; i++) { size_t l = strlen(tc[i]->name); memcpy(o, tc[i]->name, l); o += l; }
    }
    *o++ = ')'; *o = 0;
    size_t pl = strlen(prefix);
    Method *found = NULL;
    for (Class *k = c; k && !found; k = k->super)
        for (int i = 0; i < k->nmethods; i++) {
            Method *m = &k->methods[i];
            if (!strcmp(m->name, n) && !strncmp(m->sig, prefix, pl) && (m->access & ACC_PUBLIC)) { found = m; break; }
        }
    if (!found) {
        char msg[1200]; snprintf(msg, sizeof msg, "%s%s", n, prefix);
        free(n);
        throw_new(t, "Ljava/lang/NoSuchMethodException;", msg);
        return;
    }
    free(n);
    if (!C_Method) C_Method = find_class_or_die("Ljava/lang/reflect/Method;");
    MethodObj *mo = (MethodObj *)new_object(C_Method);
    mo->m = found;
    RET_L(mo);
}
static NativeDef Class_methods[] = {
    {"forName", "(Ljava/lang/String;)Ljava/lang/Class;", Class_forName, 1},
    {"getName", "()Ljava/lang/String;", Class_getName, 0},
    {"getSimpleName", "()Ljava/lang/String;", Class_getSimpleName, 0},
    {"getMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;", Class_getMethod, 0},
    {0}};

/* ---------- reflect.Method ---------- */
static void Method_invoke(Thread *t, Reg *args, Reg *ret) {
    MethodObj *mo = (MethodObj *)ARG_L(0);
    Object *recv = ARG_L(1);
    Array *argarr = (Array *)ARG_L(2);
    Method *m = mo->m;
    Reg argv[64]; int n = 0;
    if (!m->is_static) {
        if (!recv) { throw_new(t, "Ljava/lang/NullPointerException;", NULL); return; }
        R_SETL(argv[n], recv); n++;
    }
    const char *p = m->sig + 1;
    int ai = 0;
    Object **av = argarr ? ARR_DATA(argarr) : NULL;
    int alen = argarr ? argarr->length : 0;
    while (*p != ')') {
        Object *a = ai < alen ? av[ai] : NULL;
        ai++;
        switch (*p) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': R_SETI(argv[n], unbox_int(a)); n++; p++; break;
        case 'F': R_SETF(argv[n], unbox_float(a)); n++; p++; break;
        case 'J': argv[n].j = a ? unbox_long(a) : 0; n += 2; p++; break;
        case 'D': argv[n].d = a ? unbox_double(a) : 0; n += 2; p++; break;
        case 'L': R_SETL(argv[n], a); n++; while (*p != ';') p++; p++; break;
        case '[': R_SETL(argv[n], a); n++; while (*p == '[') p++; if (*p == 'L') while (*p != ';') p++; p++; break;
        default: p++;
        }
    }
    Method *target = m;
    if (!m->is_static) {
        Method *v = mtab_get(&recv->cls->vt, m->sel);
        if (v) target = v;
    }
    Reg r = call_method(t, target, argv);
    if (t->exception) {
        // wrap like InvocationTargetException? keep original for simplicity
        return;
    }
    char rt = m->shorty[0];
    switch (rt) {
    case 'V': RET_L(NULL); break;
    case 'Z': RET_L(box_bool(r.i)); break;
    case 'I': case 'B': case 'S': case 'C': RET_L(box_int(r.i)); break;
    case 'F': RET_L(box_float(r.f)); break;
    case 'J': RET_L(box_long(r.j)); break;
    case 'D': RET_L(box_double(r.d)); break;
    default: RET_L(r.l);
    }
}
static NativeDef Method_methods[] = {
    {"invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", Method_invoke, 0},
    {0}};

/* ---------- String ---------- */
#define STR(n) ((JString *)ARG_L(n))
static JString *to_jstr(Thread *t, Object *o) { return obj_to_string(t, o); }

static void str_set(JString *s, const uint16_t *c, int n) {
    s->chars = malloc(2 * (size_t)(n > 0 ? n : 1));
    if (n) memcpy(s->chars, c, 2 * (size_t)n);
    s->length = n; s->hash_set = 0;
}
static void String_fin(Object *o) { JString *s = (JString *)o; if (s->chars && s->chars != JSTR_INLINE(s)) free(s->chars); }
static void String_init0(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; str_set(STR(0), NULL, 0); }
static void String_initC(Thread *t, Reg *args, Reg *ret) {
    (void)ret; Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    str_set(STR(0), ARR_DATA(a), a->length);
}
static void String_initCII(Thread *t, Reg *args, Reg *ret) {
    (void)ret; Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), n = ARG_I(3);
    if (off < 0 || n < 0 || off + n > a->length) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    str_set(STR(0), (uint16_t *)ARR_DATA(a) + off, n);
}
static void String_initBII(Thread *t, Reg *args, Reg *ret) {
    (void)ret; Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    int off = ARG_I(2), n = ARG_I(3);
    if (off < 0 || n < 0 || off + n > a->length) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    char *tmp = malloc((size_t)n + 1); memcpy(tmp, (uint8_t *)ARR_DATA(a) + off, n); tmp[n] = 0;
    JString *j = jstr_from_utf8(tmp);
    free(tmp);
    str_set(STR(0), j->chars, j->length);
}
static void String_initB(Thread *t, Reg *args, Reg *ret) {
    Array *a = (Array *)ARG_L(1); NPE_CHECK(a);
    Reg a2[4] = {args[0], args[1]}; R_SETI(a2[2], 0); R_SETI(a2[3], a->length);
    String_initBII(t, a2, ret);
}
static void String_initS(Thread *t, Reg *args, Reg *ret) {
    (void)ret; JString *o = STR(1); NPE_CHECK(o);
    str_set(STR(0), o->chars, o->length);
}
static void String_charAt(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0); int i = ARG_I(1);
    if (i < 0 || i >= s->length) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    RET_I(s->chars[i]);
}
static void String_length(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(STR(0)->length); }
static void String_equals(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    Object *o = ARG_L(1);
    RET_I(o && o->cls == C_String && jstr_eq(STR(0), (JString *)o));
}
static void String_equalsIgnoreCase(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *a = STR(0), *b = (JString *)ARG_L(1);
    if (!b || a->length != b->length) { RET_I(0); return; }
    for (int i = 0; i < a->length; i++) {
        uint16_t x = a->chars[i], y = b->chars[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) { RET_I(0); return; }
    }
    RET_I(1);
}
static void String_hashCode(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(jstr_hash(STR(0))); }
static void String_toString(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(ARG_L(0)); }
static int str_index_of(JString *s, JString *sub, int from) {
    if (from < 0) from = 0;
    for (int i = from; i + sub->length <= s->length; i++)
        if (!memcmp(s->chars + i, sub->chars, 2 * (size_t)sub->length)) return i;
    return -1;
}
static void String_indexOfStr(Thread *t, Reg *args, Reg *ret) {
    JString *sub = STR(1); NPE_CHECK(sub);
    RET_I(str_index_of(STR(0), sub, 0));
}
static void String_indexOfStrI(Thread *t, Reg *args, Reg *ret) {
    JString *sub = STR(1); NPE_CHECK(sub);
    RET_I(str_index_of(STR(0), sub, ARG_I(2)));
}
static void String_indexOfCh(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0); int c = ARG_I(1);
    for (int i = 0; i < s->length; i++) if (s->chars[i] == c) { RET_I(i); return; }
    RET_I(-1);
}
static void String_indexOfChI(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0); int c = ARG_I(1); int from = ARG_I(2); if (from < 0) from = 0;
    for (int i = from; i < s->length; i++) if (s->chars[i] == c) { RET_I(i); return; }
    RET_I(-1);
}
static void String_lastIndexOfCh(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0); int c = ARG_I(1);
    for (int i = s->length - 1; i >= 0; i--) if (s->chars[i] == c) { RET_I(i); return; }
    RET_I(-1);
}
static void String_contains(Thread *t, Reg *args, Reg *ret) {
    JString *sub = to_jstr(t, ARG_L(1));
    RET_I(str_index_of(STR(0), sub, 0) >= 0);
}
static void String_startsWith(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0), *p = STR(1); NPE_CHECK(p);
    RET_I(p->length <= s->length && !memcmp(s->chars, p->chars, 2 * (size_t)p->length));
}
static void String_endsWith(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0), *p = STR(1); NPE_CHECK(p);
    RET_I(p->length <= s->length && !memcmp(s->chars + s->length - p->length, p->chars, 2 * (size_t)p->length));
}
static void String_substring2(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0); int b = ARG_I(1), e = ARG_I(2);
    if (b < 0 || e > s->length || b > e) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    RET_L(jstr_from_utf16(s->chars + b, e - b));
}
static void String_substring1(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0); int b = ARG_I(1);
    if (b < 0 || b > s->length) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    RET_L(jstr_from_utf16(s->chars + b, s->length - b));
}
static void String_trim(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    int b = 0, e = s->length;
    while (b < e && s->chars[b] <= ' ') b++;
    while (e > b && s->chars[e - 1] <= ' ') e--;
    if (b == 0 && e == s->length) { RET_L(s); return; }
    RET_L(jstr_from_utf16(s->chars + b, e - b));
}
static void String_toLowerCase(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    JString *n = jstr_from_utf16(s->chars, s->length);
    for (int i = 0; i < n->length; i++) if (n->chars[i] >= 'A' && n->chars[i] <= 'Z') n->chars[i] += 32;
    RET_L(n);
}
static void String_toUpperCase(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    JString *n = jstr_from_utf16(s->chars, s->length);
    for (int i = 0; i < n->length; i++) if (n->chars[i] >= 'a' && n->chars[i] <= 'z') n->chars[i] -= 32;
    RET_L(n);
}
static void String_replace(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0);
    JString *a = to_jstr(t, ARG_L(1)), *b = to_jstr(t, ARG_L(2));
    if (a->length == 0) { RET_L(s); return; }
    uint16_t *buf = malloc(2 * ((size_t)s->length * (b->length + 1) + 1));
    int n = 0, i = 0;
    while (i < s->length) {
        if (i + a->length <= s->length && !memcmp(s->chars + i, a->chars, 2 * (size_t)a->length)) {
            memcpy(buf + n, b->chars, 2 * (size_t)b->length); n += b->length; i += a->length;
        } else buf[n++] = s->chars[i++];
    }
    RET_L(jstr_from_utf16(buf, n));
    free(buf);
}
static void String_replaceCh(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    JString *n = jstr_from_utf16(s->chars, s->length);
    for (int i = 0; i < n->length; i++) if (n->chars[i] == (uint16_t)ARG_I(1)) n->chars[i] = (uint16_t)ARG_I(2);
    RET_L(n);
}
static void String_split(Thread *t, Reg *args, Reg *ret) {
    JString *s = STR(0), *re = STR(1);
    NPE_CHECK(re);
    // literal split (regex metacharacters other than escaped are not supported)
    uint16_t sep[64]; int sl = 0;
    for (int i = 0; i < re->length && sl < 64; i++) {
        if (re->chars[i] == '\\' && i + 1 < re->length) { sep[sl++] = re->chars[++i]; continue; }
        sep[sl++] = re->chars[i];
    }
    int cap = 8, n = 0;
    int *starts = malloc(sizeof(int) * cap), *ends = malloc(sizeof(int) * cap);
    int b = 0;
    for (int i = 0; sl && i + sl <= s->length;) {
        if (!memcmp(s->chars + i, sep, 2 * (size_t)sl)) {
            if (n + 1 >= cap) { cap *= 2; starts = realloc(starts, sizeof(int) * cap); ends = realloc(ends, sizeof(int) * cap); }
            starts[n] = b; ends[n] = i; n++;
            i += sl; b = i;
        } else i++;
    }
    if (n + 1 >= cap) { cap *= 2; starts = realloc(starts, sizeof(int) * cap); ends = realloc(ends, sizeof(int) * cap); }
    starts[n] = b; ends[n] = s->length; n++;
    if (n > 1) while (n > 0 && starts[n - 1] == ends[n - 1]) n--;  // remove trailing empties
    if (n == 0) { n = 0; }
    Array *a = new_array(C_arrString, n);
    for (int i = 0; i < n; i++) ((JString **)ARR_DATA(a))[i] = jstr_from_utf16(s->chars + starts[i], ends[i] - starts[i]);
    free(starts); free(ends);
    RET_L(a);
}
static void String_getChars(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    JString *s = STR(0); int sb = ARG_I(1), se = ARG_I(2); Array *d = (Array *)ARG_L(3); int db = ARG_I(4);
    NPE_CHECK(d);
    if (sb < 0 || se > s->length || sb > se || db < 0 || db + (se - sb) > d->length) { throw_new(t, "Ljava/lang/StringIndexOutOfBoundsException;", NULL); return; }
    memcpy((uint16_t *)ARR_DATA(d) + db, s->chars + sb, 2 * (size_t)(se - sb));
}
static void String_toCharArray(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    Array *a = new_array(C_arrC, s->length);
    memcpy(ARR_DATA(a), s->chars, 2 * (size_t)s->length);
    RET_L(a);
}
static void String_compareTo(Thread *t, Reg *args, Reg *ret) {
    JString *a = STR(0), *b = STR(1); NPE_CHECK(b);
    int n = a->length < b->length ? a->length : b->length;
    for (int i = 0; i < n; i++) if (a->chars[i] != b->chars[i]) { RET_I(a->chars[i] - b->chars[i]); return; }
    RET_I(a->length - b->length);
}
static void String_valueOfObj(Thread *t, Reg *args, Reg *ret) { RET_L(obj_to_string(t, ARG_L(0))); }
static void String_valueOfI(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%d", ARG_I(0)); RET_L(jstr_from_utf8(b)); }
static void String_valueOfF(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(ARG_F(0), 1, b); RET_L(jstr_from_utf8(b)); }

/* String.format */
JString *java_format(Thread *t, JString *fmt, Array *args) {
    size_t cap = 256, n = 0;
    char *out = malloc(cap);
#define PUTC(c) do { if (n + 1 >= cap) { cap *= 2; out = realloc(out, cap); } out[n++] = (c); } while (0)
#define PUTS(s) do { const char *_s = (s); while (*_s) PUTC(*_s++); } while (0)
    char *f = jstr_to_utf8(fmt);
    Object **av = args ? ARR_DATA(args) : NULL;
    int alen = args ? args->length : 0, ai = 0;
    for (char *p = f; *p; p++) {
        if (*p != '%') { PUTC(*p); continue; }
        p++;
        char spec[32]; int si = 0; spec[si++] = '%';
        while (*p && strchr("-+ 0#,", *p)) { if (*p != ',') spec[si++] = *p; p++; }
        while (*p >= '0' && *p <= '9') spec[si++] = *p++;
        if (*p == '.') { spec[si++] = *p++; while (*p >= '0' && *p <= '9') spec[si++] = *p++; }
        char conv = *p;
        if (!conv) break;
        char buf[512];
        Object *a = (conv != '%' && conv != 'n') ? (ai < alen ? av[ai++] : NULL) : NULL;
        switch (conv) {
        case 'd': spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = 0;
            snprintf(buf, sizeof buf, spec, a ? (long long)unbox_long(a) : 0LL); PUTS(buf); break;
        case 'x': case 'X': spec[si++] = conv; spec[si] = 0;
            snprintf(buf, sizeof buf, spec, a ? (unsigned)unbox_int(a) : 0u); PUTS(buf); break;
        case 'f': case 'e': case 'g': spec[si++] = conv; spec[si] = 0;
            snprintf(buf, sizeof buf, spec, a ? unbox_double(a) : 0.0); PUTS(buf); break;
        case 'c': PUTC((char)(a ? unbox_int(a) : '?')); break;
        case 's': case 'S': {
            JString *s = obj_to_string(t, a);
            char *u = jstr_to_utf8(s);
            if (conv == 'S') for (char *q = u; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
            spec[si++] = 's'; spec[si] = 0;
            char *tmp = malloc(strlen(u) + 256);
            sprintf(tmp, spec, u);
            PUTS(tmp);
            free(tmp); free(u);
            break;
        }
        case 'b': PUTS(a && unbox_int(a) ? "true" : "false"); break;
        case 'n': PUTC('\n'); break;
        case '%': PUTC('%'); break;
        default: PUTC('%'); PUTC(conv); break;
        }
    }
    out[n] = 0;
    JString *r = jstr_from_utf8(out);
    free(out); free(f);
    return r;
}
static void String_format(Thread *t, Reg *args, Reg *ret) {
    NPE_CHECK(ARG_L(0));
    RET_L(java_format(t, STR(0), (Array *)ARG_L(1)));
}

static NativeDef String_methods[] = {
    {"<init>", "()V", String_init0, 0},
    {"<init>", "([C)V", String_initC, 0},
    {"<init>", "([CII)V", String_initCII, 0},
    {"<init>", "([BII)V", String_initBII, 0},
    {"<init>", "([B)V", String_initB, 0},
    {"<init>", "(Ljava/lang/String;)V", String_initS, 0},
    {"charAt", "(I)C", String_charAt, 0},
    {"length", "()I", String_length, 0},
    {"equals", "(Ljava/lang/Object;)Z", String_equals, 0},
    {"equalsIgnoreCase", "(Ljava/lang/String;)Z", String_equalsIgnoreCase, 0},
    {"hashCode", "()I", String_hashCode, 0},
    {"toString", "()Ljava/lang/String;", String_toString, 0},
    {"indexOf", "(Ljava/lang/String;)I", String_indexOfStr, 0},
    {"indexOf", "(Ljava/lang/String;I)I", String_indexOfStrI, 0},
    {"indexOf", "(I)I", String_indexOfCh, 0},
    {"indexOf", "(II)I", String_indexOfChI, 0},
    {"lastIndexOf", "(I)I", String_lastIndexOfCh, 0},
    {"contains", "(Ljava/lang/CharSequence;)Z", String_contains, 0},
    {"startsWith", "(Ljava/lang/String;)Z", String_startsWith, 0},
    {"endsWith", "(Ljava/lang/String;)Z", String_endsWith, 0},
    {"substring", "(II)Ljava/lang/String;", String_substring2, 0},
    {"substring", "(I)Ljava/lang/String;", String_substring1, 0},
    {"trim", "()Ljava/lang/String;", String_trim, 0},
    {"toLowerCase", "()Ljava/lang/String;", String_toLowerCase, 0},
    {"toUpperCase", "()Ljava/lang/String;", String_toUpperCase, 0},
    {"replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;", String_replace, 0},
    {"replace", "(CC)Ljava/lang/String;", String_replaceCh, 0},
    {"split", "(Ljava/lang/String;)[Ljava/lang/String;", String_split, 0},
    {"getChars", "(II[CI)V", String_getChars, 0},
    {"toCharArray", "()[C", String_toCharArray, 0},
    {"compareTo", "(Ljava/lang/String;)I", String_compareTo, 0},
    {"compareTo", "(Ljava/lang/Object;)I", String_compareTo, 0},
    {"format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", String_format, 1},
    {"valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", String_valueOfObj, 1},
    {"valueOf", "(I)Ljava/lang/String;", String_valueOfI, 1},
    {"valueOf", "(F)Ljava/lang/String;", String_valueOfF, 1},
    {0}};

/* ---------- StringBuilder ---------- */
static void sb_reserve(SBObj *sb, int extra) {
    if (sb->len + extra > sb->cap) {
        int nc = sb->cap ? sb->cap * 2 : 32;
        while (nc < sb->len + extra) nc *= 2;
        sb->buf = realloc(sb->buf, 2 * (size_t)nc);
        sb->cap = nc;
    }
}
static void sb_append_u16(SBObj *sb, const uint16_t *c, int n) { if (n <= 0) return; sb_reserve(sb, n); memcpy(sb->buf + sb->len, c, 2 * (size_t)n); sb->len += n; }
static void sb_append_c(SBObj *sb, const char *s) {
    int n = (int)strlen(s); sb_reserve(sb, n);
    for (int i = 0; i < n; i++) sb->buf[sb->len++] = (uint8_t)s[i];
}
static void SB_fin(Object *o) { free(((SBObj *)o)->buf); }
static void SB_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void SB_initS(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    JString *s = obj_to_string(t, ARG_L(1));
    sb_append_u16((SBObj *)ARG_L(0), s->chars, s->length);
}
static void SB_appendS(Thread *t, Reg *args, Reg *ret) {
    JString *s = obj_to_string(t, ARG_L(1));
    sb_append_u16((SBObj *)ARG_L(0), s->chars, s->length);
    RET_L(ARG_L(0));
}
static void SB_appendI(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%d", ARG_I(1)); sb_append_c((SBObj *)ARG_L(0), b); RET_L(ARG_L(0)); }
static void SB_appendJ(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%lld", (long long)ARG_J(1)); sb_append_c((SBObj *)ARG_L(0), b); RET_L(ARG_L(0)); }
static void SB_appendF(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(ARG_F(1), 1, b); sb_append_c((SBObj *)ARG_L(0), b); RET_L(ARG_L(0)); }
static void SB_appendD(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(ARG_D(1), 0, b); sb_append_c((SBObj *)ARG_L(0), b); RET_L(ARG_L(0)); }
static void SB_appendZ(Thread *t, Reg *args, Reg *ret) { (void)t; sb_append_c((SBObj *)ARG_L(0), ARG_I(1) ? "true" : "false"); RET_L(ARG_L(0)); }
static void SB_appendC(Thread *t, Reg *args, Reg *ret) { (void)t; uint16_t c = (uint16_t)ARG_I(1); sb_append_u16((SBObj *)ARG_L(0), &c, 1); RET_L(ARG_L(0)); }
static void SB_toString(Thread *t, Reg *args, Reg *ret) { (void)t; SBObj *sb = (SBObj *)ARG_L(0); RET_L(jstr_from_utf16(sb->buf, sb->len)); }
static void SB_length(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((SBObj *)ARG_L(0))->len); }
static NativeDef SB_methods[] = {
    {"<init>", "()V", SB_init, 0},
    {"<init>", "(I)V", SB_init, 0},
    {"<init>", "(Ljava/lang/String;)V", SB_initS, 0},
    {"append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", SB_appendS, 0},
    {"append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", SB_appendS, 0},
    {"append", "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;", SB_appendS, 0},
    {"append", "(I)Ljava/lang/StringBuilder;", SB_appendI, 0},
    {"append", "(J)Ljava/lang/StringBuilder;", SB_appendJ, 0},
    {"append", "(F)Ljava/lang/StringBuilder;", SB_appendF, 0},
    {"append", "(D)Ljava/lang/StringBuilder;", SB_appendD, 0},
    {"append", "(Z)Ljava/lang/StringBuilder;", SB_appendZ, 0},
    {"append", "(C)Ljava/lang/StringBuilder;", SB_appendC, 0},
    {"toString", "()Ljava/lang/String;", SB_toString, 0},
    {"length", "()I", SB_length, 0},
    {0}};

/* ---------- boxes ---------- */
static void Box_initI(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((BoxObj *)ARG_L(0))->v.i = ARG_I(1); }
static void Box_initF(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((BoxObj *)ARG_L(0))->v.f = ARG_F(1); }
static void Box_intValue(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(unbox_int(ARG_L(0))); }
static void Box_floatValue(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(unbox_float(ARG_L(0))); }
static void Box_longValue(Thread *t, Reg *args, Reg *ret) { (void)t; RET_J(unbox_long(ARG_L(0))); }
static void Box_doubleValue(Thread *t, Reg *args, Reg *ret) { (void)t; RET_D(unbox_double(ARG_L(0))); }
static void Box_hashCode(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((BoxObj *)ARG_L(0))->v.i); }
static void Box_equals(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    Object *a = ARG_L(0), *b = ARG_L(1);
    RET_I(b && a->cls == b->cls && ((BoxObj *)a)->v.j == ((BoxObj *)b)->v.j);
}
static void Int_toString(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%d", ((BoxObj *)ARG_L(0))->v.i); RET_L(jstr_from_utf8(b)); }
static void Int_toStringS(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%d", ARG_I(0)); RET_L(jstr_from_utf8(b)); }
static void Int_valueOfI(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(box_int(ARG_I(0))); }
static int parse_long(JString *s, int radix, int64_t *out) {
    if (!s || s->length == 0) return 0;
    int i = 0, neg = 0;
    if (s->chars[0] == '-' || s->chars[0] == '+') { neg = s->chars[0] == '-'; i = 1; if (s->length == 1) return 0; }
    int64_t v = 0;
    for (; i < s->length; i++) {
        int c = s->chars[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return 0;
        if (d >= radix) return 0;
        v = v * radix + d;
    }
    *out = neg ? -v : v;
    return 1;
}
static void Int_parse(Thread *t, Reg *args, Reg *ret) {
    int64_t v;
    if (!parse_long(STR(0), 10, &v)) { char *u = STR(0) ? jstr_to_utf8(STR(0)) : strdup("null"); throw_new(t, "Ljava/lang/NumberFormatException;", u); free(u); return; }
    RET_I((int32_t)v);
}
static void Int_valueOfS(Thread *t, Reg *args, Reg *ret) {
    int64_t v;
    if (!parse_long(STR(0), 10, &v)) { char *u = STR(0) ? jstr_to_utf8(STR(0)) : strdup("null"); throw_new(t, "Ljava/lang/NumberFormatException;", u); free(u); return; }
    RET_L(box_int((int32_t)v));
}
static void Long_valueOfSI(Thread *t, Reg *args, Reg *ret) {
    int64_t v;
    if (!parse_long(STR(0), ARG_I(1), &v)) { throw_new(t, "Ljava/lang/NumberFormatException;", NULL); return; }
    RET_L(box_long(v));
}
static void Long_parseSI(Thread *t, Reg *args, Reg *ret) {
    int64_t v;
    if (!parse_long(STR(0), ARG_I(1), &v)) { throw_new(t, "Ljava/lang/NumberFormatException;", NULL); return; }
    RET_J(v);
}
static void Long_toString(Thread *t, Reg *args, Reg *ret) { (void)t; char b[32]; snprintf(b, sizeof b, "%lld", (long long)((BoxObj *)ARG_L(0))->v.j); RET_L(jstr_from_utf8(b)); }
static int parse_double(JString *s, double *out) {
    if (!s) return 0;
    char *u = jstr_to_utf8(s);
    char *b = u; while (*b && (uint8_t)*b <= ' ') b++;
    char *e = b + strlen(b); while (e > b && (uint8_t)e[-1] <= ' ') *--e = 0;
    if (e > b && (e[-1] == 'f' || e[-1] == 'F' || e[-1] == 'd' || e[-1] == 'D')) *--e = 0;
    char *end;
    double v = strtod(b, &end);
    int ok = *b && *end == 0;
    free(u);
    if (ok) *out = v;
    return ok;
}
static void Float_valueOfS(Thread *t, Reg *args, Reg *ret) {
    double v;
    if (!parse_double(STR(0), &v)) { char *u = STR(0) ? jstr_to_utf8(STR(0)) : strdup("null"); throw_new(t, "Ljava/lang/NumberFormatException;", u); free(u); return; }
    RET_L(box_float((float)v));
}
static void Float_parse(Thread *t, Reg *args, Reg *ret) {
    double v;
    if (!parse_double(STR(0), &v)) { throw_new(t, "Ljava/lang/NumberFormatException;", NULL); return; }
    RET_F((float)v);
}
static void Float_valueOfF(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(box_float(ARG_F(0))); }
static void Float_toStringS(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(ARG_F(0), 1, b); RET_L(jstr_from_utf8(b)); }
static void Float_toString(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(((BoxObj *)ARG_L(0))->v.f, 1, b); RET_L(jstr_from_utf8(b)); }
static void Float_isNaN(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ARG_F(0) != ARG_F(0)); }
static void Float_floatToIntBits(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(args[0].i); }
static void Float_intBitsToFloat(Thread *t, Reg *args, Reg *ret) { (void)t; ret->raw = (uint32_t)args[0].i; }
static void Double_toString(Thread *t, Reg *args, Reg *ret) { (void)t; char b[64]; java_float_str(((BoxObj *)ARG_L(0))->v.d, 0, b); RET_L(jstr_from_utf8(b)); }
static void Bool_valueOfS(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    JString *s = STR(0);
    int v = 0;
    if (s && s->length == 4) {
        char b[5]; for (int i = 0; i < 4; i++) { int c = s->chars[i]; if (c >= 'A' && c <= 'Z') c += 32; b[i] = (char)c; } b[4] = 0;
        v = !strcmp(b, "true");
    }
    RET_L(box_bool(v));
}
static void Bool_parse(Thread *t, Reg *args, Reg *ret) {
    Reg r; Bool_valueOfS(t, args, &r); RET_I(((BoxObj *)r.l)->v.i);
}
static void Bool_valueOfZ(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(box_bool(ARG_I(0))); }
static void Bool_toString(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(jstr_from_utf8(((BoxObj *)ARG_L(0))->v.i ? "true" : "false")); }
static void Bool_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    Class *c = find_class("Ljava/lang/Boolean;");
    Object *tr = box_bool(1), *fa = box_bool(0);
    Field *f;
    f = class_find_field(c, "TRUE", NULL); memcpy(c->statics + f->offset, &tr, sizeof tr);
    f = class_find_field(c, "FALSE", NULL); memcpy(c->statics + f->offset, &fa, sizeof fa);
    Class *z = prim_class('Z');
    f = class_find_field(c, "TYPE", NULL); memcpy(c->statics + f->offset, &z, sizeof z);
}
static void Int_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    Class *c = find_class("Ljava/lang/Integer;");
    Class *z = prim_class('I');
    Field *f = class_find_field(c, "TYPE", NULL); memcpy(c->statics + f->offset, &z, sizeof z);
}
static void Float_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    Class *c = find_class("Ljava/lang/Float;");
    Class *z = prim_class('F');
    Field *f = class_find_field(c, "TYPE", NULL); memcpy(c->statics + f->offset, &z, sizeof z);
}

static NativeDef Integer_methods[] = {
    {"<clinit>", "()V", Int_clinit, 1},
    {"<init>", "(I)V", Box_initI, 0},
    {"intValue", "()I", Box_intValue, 0},
    {"floatValue", "()F", Box_floatValue, 0},
    {"longValue", "()J", Box_longValue, 0},
    {"doubleValue", "()D", Box_doubleValue, 0},
    {"hashCode", "()I", Box_hashCode, 0},
    {"equals", "(Ljava/lang/Object;)Z", Box_equals, 0},
    {"toString", "()Ljava/lang/String;", Int_toString, 0},
    {"toString", "(I)Ljava/lang/String;", Int_toStringS, 1},
    {"valueOf", "(I)Ljava/lang/Integer;", Int_valueOfI, 1},
    {"valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", Int_valueOfS, 1},
    {"parseInt", "(Ljava/lang/String;)I", Int_parse, 1},
    {0}};
static NativeDef Float_methods[] = {
    {"<clinit>", "()V", Float_clinit, 1},
    {"<init>", "(F)V", Box_initF, 0},
    {"floatValue", "()F", Box_floatValue, 0},
    {"intValue", "()I", Box_intValue, 0},
    {"doubleValue", "()D", Box_doubleValue, 0},
    {"hashCode", "()I", Box_hashCode, 0},
    {"equals", "(Ljava/lang/Object;)Z", Box_equals, 0},
    {"toString", "()Ljava/lang/String;", Float_toString, 0},
    {"toString", "(F)Ljava/lang/String;", Float_toStringS, 1},
    {"valueOf", "(F)Ljava/lang/Float;", Float_valueOfF, 1},
    {"valueOf", "(Ljava/lang/String;)Ljava/lang/Float;", Float_valueOfS, 1},
    {"parseFloat", "(Ljava/lang/String;)F", Float_parse, 1},
    {"isNaN", "(F)Z", Float_isNaN, 1},
    {"floatToIntBits", "(F)I", Float_floatToIntBits, 1},
    {"floatToRawIntBits", "(F)I", Float_floatToIntBits, 1},
    {"intBitsToFloat", "(I)F", Float_intBitsToFloat, 1},
    {0}};
static NativeDef Long_methods[] = {
    {"longValue", "()J", Box_longValue, 0},
    {"intValue", "()I", Box_intValue, 0},
    {"hashCode", "()I", Box_hashCode, 0},
    {"equals", "(Ljava/lang/Object;)Z", Box_equals, 0},
    {"toString", "()Ljava/lang/String;", Long_toString, 0},
    {"valueOf", "(Ljava/lang/String;I)Ljava/lang/Long;", Long_valueOfSI, 1},
    {"parseLong", "(Ljava/lang/String;I)J", Long_parseSI, 1},
    {0}};
static NativeDef Double_methods[] = {
    {"doubleValue", "()D", Box_doubleValue, 0},
    {"floatValue", "()F", Box_floatValue, 0},
    {"intValue", "()I", Box_intValue, 0},
    {"toString", "()Ljava/lang/String;", Double_toString, 0},
    {0}};
static NativeDef Boolean_methods[] = {
    {"<clinit>", "()V", Bool_clinit, 1},
    {"<init>", "(Z)V", Box_initI, 0},
    {"booleanValue", "()Z", Box_intValue, 0},
    {"hashCode", "()I", Box_hashCode, 0},
    {"equals", "(Ljava/lang/Object;)Z", Box_equals, 0},
    {"toString", "()Ljava/lang/String;", Bool_toString, 0},
    {"valueOf", "(Ljava/lang/String;)Ljava/lang/Boolean;", Bool_valueOfS, 1},
    {"valueOf", "(Z)Ljava/lang/Boolean;", Bool_valueOfZ, 1},
    {"parseBoolean", "(Ljava/lang/String;)Z", Bool_parse, 1},
    {0}};

/* ---------- Math / FloatMath ---------- */
#define MATH_D1(nm, fn) static void Math_##nm(Thread *t, Reg *args, Reg *ret) { (void)t; RET_D(fn(ARG_D(0))); }
MATH_D1(absD, fabs) MATH_D1(ceil, ceil) MATH_D1(cos, cos) MATH_D1(exp, exp) MATH_D1(floor, floor)
MATH_D1(sin, sin) MATH_D1(sqrt, sqrt) MATH_D1(tan, tan) MATH_D1(acos, acos) MATH_D1(asin, asin) MATH_D1(atan, atan) MATH_D1(log, log)
static void Math_absF(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(fabsf(ARG_F(0))); }
static void Math_absI(Thread *t, Reg *args, Reg *ret) { (void)t; int v = ARG_I(0); RET_I(v < 0 ? (int32_t)(0u - (uint32_t)v) : v); }
static void Math_atan2(Thread *t, Reg *args, Reg *ret) { (void)t; RET_D(atan2(ARG_D(0), ARG_D(2))); }
static void Math_pow(Thread *t, Reg *args, Reg *ret) { (void)t; RET_D(pow(ARG_D(0), ARG_D(2))); }
static void Math_maxI(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ARG_I(0) > ARG_I(1) ? ARG_I(0) : ARG_I(1)); }
static void Math_minI(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ARG_I(0) < ARG_I(1) ? ARG_I(0) : ARG_I(1)); }
static void Math_maxF(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(fmaxf(ARG_F(0), ARG_F(1))); }
static void Math_minF(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(fminf(ARG_F(0), ARG_F(1))); }
static void Math_round(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I((int32_t)floorf(ARG_F(0) + 0.5f)); }
static void Math_random(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_D((double)rand() / ((double)RAND_MAX + 1)); }
static NativeDef Math_methods[] = {
    {"abs", "(D)D", Math_absD, 1}, {"abs", "(F)F", Math_absF, 1}, {"abs", "(I)I", Math_absI, 1},
    {"atan2", "(DD)D", Math_atan2, 1}, {"ceil", "(D)D", Math_ceil, 1}, {"cos", "(D)D", Math_cos, 1},
    {"exp", "(D)D", Math_exp, 1}, {"floor", "(D)D", Math_floor, 1}, {"sin", "(D)D", Math_sin, 1},
    {"sqrt", "(D)D", Math_sqrt, 1}, {"tan", "(D)D", Math_tan, 1}, {"acos", "(D)D", Math_acos, 1},
    {"asin", "(D)D", Math_asin, 1}, {"atan", "(D)D", Math_atan, 1}, {"log", "(D)D", Math_log, 1},
    {"pow", "(DD)D", Math_pow, 1}, {"max", "(II)I", Math_maxI, 1}, {"min", "(II)I", Math_minI, 1},
    {"max", "(FF)F", Math_maxF, 1}, {"min", "(FF)F", Math_minF, 1}, {"round", "(F)I", Math_round, 1},
    {"random", "()D", Math_random, 1},
    {0}};
static void FM_cos(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(cosf(ARG_F(0))); }
static void FM_sin(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(sinf(ARG_F(0))); }
static void FM_sqrt(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(sqrtf(ARG_F(0))); }
static void FM_floor(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(floorf(ARG_F(0))); }
static void FM_ceil(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(ceilf(ARG_F(0))); }
static NativeDef FloatMath_methods[] = {
    {"cos", "(F)F", FM_cos, 1}, {"sin", "(F)F", FM_sin, 1}, {"sqrt", "(F)F", FM_sqrt, 1},
    {"floor", "(F)F", FM_floor, 1}, {"ceil", "(F)F", FM_ceil, 1},
    {0}};

/* ---------- System / Runtime / PrintStream ---------- */
static void System_arraycopy(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    Array *s = (Array *)ARG_L(0), *d = (Array *)ARG_L(2);
    int sp = ARG_I(1), dp = ARG_I(3), n = ARG_I(4);
    if (!s || !d) { throw_new(t, "Ljava/lang/NullPointerException;", NULL); return; }
    if (n < 0 || sp < 0 || dp < 0 || sp + n > s->length || dp + n > d->length) {
        vm_log("arraycopy OOB sp=%d dp=%d n=%d slen=%d dlen=%d", sp, dp, n, s->length, d->length);
        throw_new(t, "Ljava/lang/ArrayIndexOutOfBoundsException;", NULL); return;
    }
    int es = s->h.cls->elem_size;
    memmove((uint8_t *)ARR_DATA(d) + (size_t)dp * es, (uint8_t *)ARR_DATA(s) + (size_t)sp * es, (size_t)n * es);
}
static void System_currentTimeMillis(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_J((int64_t)vm_now_ms()); }
static void System_nanoTime(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_J((int64_t)(vm_now_ms() * 1e6)); }
static void System_gc(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void System_identityHashCode(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(ARG_L(0) ? (int32_t)obj_identity_hash(ARG_L(0)) : 0); }
static void System_clinit(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)args; (void)ret;
    Class *c = find_class("Ljava/lang/System;");
    Class *ps = find_class_or_die("Ljava/io/PrintStream;");
    Object *out = new_object(ps), *err = new_object(ps);
    Field *f;
    f = class_find_field(c, "out", NULL); memcpy(c->statics + f->offset, &out, sizeof out);
    f = class_find_field(c, "err", NULL); memcpy(c->statics + f->offset, &err, sizeof err);
}
static NativeDef System_methods[] = {
    {"<clinit>", "()V", System_clinit, 1},
    {"arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", System_arraycopy, 1},
    {"currentTimeMillis", "()J", System_currentTimeMillis, 1},
    {"nanoTime", "()J", System_nanoTime, 1},
    {"gc", "()V", System_gc, 1},
    {"identityHashCode", "(Ljava/lang/Object;)I", System_identityHashCode, 1},
    {0}};
int g_verbose_println = 1;
static void PS_println(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    if (!g_verbose_println) return;
    JString *s = obj_to_string(t, ARG_L(1));
    char *u = jstr_to_utf8(s);
    vm_log("[java] %s", u);
    free(u);
}
static void PS_printlnV(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static NativeDef PrintStream_methods[] = {
    {"println", "(Ljava/lang/String;)V", PS_println, 0},
    {"println", "(Ljava/lang/Object;)V", PS_println, 0},
    {"print", "(Ljava/lang/String;)V", PS_println, 0},
    {"println", "()V", PS_printlnV, 0},
    {0}};
static void Runtime_get(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_L(new_object(find_class("Ljava/lang/Runtime;"))); }
static void Runtime_maxMemory(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_J(64LL * 1024 * 1024); }
static void Runtime_freeMemory(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_J(32LL * 1024 * 1024); }
static NativeDef Runtime_methods[] = {
    {"getRuntime", "()Ljava/lang/Runtime;", Runtime_get, 1},
    {"maxMemory", "()J", Runtime_maxMemory, 0},
    {"freeMemory", "()J", Runtime_freeMemory, 0},
    {"totalMemory", "()J", Runtime_maxMemory, 0},
    {0}};

/* ---------- Thread ---------- */
void green_start(Object *jthread, Object *runnable);
static void Thread_initR(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((ThreadObj *)ARG_L(0))->runnable = ARG_L(1); }
static void Thread_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; (void)args; }
static void Thread_start(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    ThreadObj *th = (ThreadObj *)ARG_L(0);
    green_start((Object *)th, th->runnable ? th->runnable : (Object *)th);
}
static void Thread_run(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    ThreadObj *th = (ThreadObj *)ARG_L(0);
    if (th->runnable) { Reg a[1]; call_virtual(t, th->runnable, "run", "()V", a); }
}
static void Thread_sleep(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    if (t->green) {
        t->wake_time = vm_now_ms() + (double)ARG_J(0);
        t->state = TH_SLEEPING;
        t->yield_req = 1;
    }
}
static void Thread_yield(Thread *t, Reg *args, Reg *ret) { (void)args; (void)ret; if (t->green) t->yield_req = 1; }
static void Thread_current(Thread *t, Reg *args, Reg *ret) {
    (void)args;
    if (!t->jthread) { t->jthread = new_object(find_class("Ljava/lang/Thread;")); }
    RET_L(t->jthread);
}
static void Thread_setPriority(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static NativeDef Thread_methods[] = {
    {"<init>", "(Ljava/lang/Runnable;)V", Thread_initR, 0},
    {"<init>", "()V", Thread_init, 0},
    {"start", "()V", Thread_start, 0},
    {"run", "()V", Thread_run, 0},
    {"sleep", "(J)V", Thread_sleep, 1},
    {"yield", "()V", Thread_yield, 1},
    {"currentThread", "()Ljava/lang/Thread;", Thread_current, 1},
    {"setPriority", "(I)V", Thread_setPriority, 0},
    {0}};
static void Thread_mark(Object *o) { gc_mark(((ThreadObj *)o)->runnable); }

/* ---------- Enum ---------- */
static void Enum_init(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    EnumObj *e = (EnumObj *)ARG_L(0);
    e->name = (JString *)ARG_L(1);
    e->ordinal = ARG_I(2);
}
static void Enum_ordinal(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((EnumObj *)ARG_L(0))->ordinal); }
static void Enum_name(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(((EnumObj *)ARG_L(0))->name); }
static void Enum_hashCode(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I((int32_t)obj_identity_hash(ARG_L(0))); }
static void Enum_valueOf(Thread *t, Reg *args, Reg *ret) {
    Class *c = (Class *)ARG_L(0);
    JString *n = (JString *)ARG_L(1);
    NPE_CHECK(c); NPE_CHECK(n);
    class_init(t, c);
    for (int i = 0; i < c->nfields; i++) {
        Field *f = &c->fields[i];
        if (!f->is_static || strcmp(f->type, c->name)) continue;
        EnumObj *e; memcpy(&e, c->statics + f->offset, sizeof e);
        if (e && jstr_eq(e->name, n)) { RET_L(e); return; }
    }
    throw_new(t, "Ljava/lang/IllegalArgumentException;", "No enum constant");
}
static void Enum_compareTo(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    RET_I(((EnumObj *)ARG_L(0))->ordinal - ((EnumObj *)ARG_L(1))->ordinal);
}
static void Enum_mark(Object *o) { gc_mark((Object *)((EnumObj *)o)->name); }
static NativeDef Enum_methods[] = {
    {"<init>", "(Ljava/lang/String;I)V", Enum_init, 0},
    {"ordinal", "()I", Enum_ordinal, 0},
    {"name", "()Ljava/lang/String;", Enum_name, 0},
    {"toString", "()Ljava/lang/String;", Enum_name, 0},
    {"hashCode", "()I", Enum_hashCode, 0},
    {"compareTo", "(Ljava/lang/Enum;)I", Enum_compareTo, 0},
    {"valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", Enum_valueOf, 1},
    {0}};

/* ---------- Throwable ---------- */
typedef struct ThrowableObj { Object h; JString *message; Object *cause; } ThrowableObj;
static void Thr_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void Thr_initS(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((ThrowableObj *)ARG_L(0))->message = (JString *)ARG_L(1); }
static void Thr_initSC(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((ThrowableObj *)ARG_L(0))->message = (JString *)ARG_L(1); ((ThrowableObj *)ARG_L(0))->cause = ARG_L(2); }
static void Thr_getMessage(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(((ThrowableObj *)ARG_L(0))->message); }
static void Thr_toString(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    ThrowableObj *e = (ThrowableObj *)ARG_L(0);
    char n[256]; class_java_name(e->h.cls, n, sizeof n);
    if (e->message) {
        char *m = jstr_to_utf8(e->message);
        char *b = malloc(strlen(n) + strlen(m) + 4);
        sprintf(b, "%s: %s", n, m);
        RET_L(jstr_from_utf8(b));
        free(b); free(m);
    } else RET_L(jstr_from_utf8(n));
}
void dump_stack(Thread *t);
static void Thr_printStackTrace(Thread *t, Reg *args, Reg *ret) {
    Reg r; Thr_toString(t, args, &r);
    char *u = jstr_to_utf8((JString *)r.l);
    vm_log("[java exception] %s", u);
    free(u);
    (void)ret;
}
static void Thr_mark(Object *o) { ThrowableObj *e = (ThrowableObj *)o; gc_mark((Object *)e->message); gc_mark(e->cause); }
static NativeDef Throwable_methods[] = {
    {"<init>", "()V", Thr_init, 0},
    {"<init>", "(Ljava/lang/String;)V", Thr_initS, 0},
    {"<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", Thr_initSC, 0},
    {"getMessage", "()Ljava/lang/String;", Thr_getMessage, 0},
    {"toString", "()Ljava/lang/String;", Thr_toString, 0},
    {"printStackTrace", "()V", Thr_printStackTrace, 0},
    {"printStackTrace", "(Ljava/io/PrintStream;)V", Thr_printStackTrace, 0},
    {0}};
static NativeDef Exc_methods[] = {
    {"<init>", "()V", Thr_init, 0},
    {"<init>", "(Ljava/lang/String;)V", Thr_initS, 0},
    {0}};

static NativeDef Iface_none[] = {{0}};
static NativeDef Runnable_methods[] = {{"run", "()V", NULL, 0}, {0}};
static NativeDef Comparator_methods[] = {{"compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", NULL, 0}, {0}};
static NativeDef CharSeq_methods[] = {{"toString", "()Ljava/lang/String;", NULL, 0}, {"length", "()I", NULL, 0}, {"charAt", "(I)C", NULL, 0}, {0}};

#define EXC_DEF(n, s) {n, s, {0}, sizeof(ThrowableObj), 0, Exc_methods, NULL, NULL, {0}}

BuiltinDef lang_builtins[] = {
    {"Ljava/lang/Object;", NULL, {0}, sizeof(Object), 0, Object_methods, NULL, NULL, {0}},
    {"Ljava/lang/Class;", "Ljava/lang/Object;", {0}, sizeof(Object), ACC_FINAL, Class_methods, NULL, NULL, {0}},
    {"Ljava/lang/reflect/Method;", "Ljava/lang/Object;", {0}, sizeof(MethodObj), ACC_FINAL, Method_methods, NULL, NULL, {0}},
    {"Ljava/lang/CharSequence;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, CharSeq_methods, NULL, NULL, {0}},
    {"Ljava/lang/Comparable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Iface_none, NULL, NULL, {0}},
    {"Ljava/lang/Runnable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Runnable_methods, NULL, NULL, {0}},
    {"Ljava/lang/Cloneable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Iface_none, NULL, NULL, {0}},
    {"Ljava/io/Serializable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Iface_none, NULL, NULL, {0}},
    {"Ljava/lang/Iterable;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Iface_none, NULL, NULL, {0}},
    {"Ljava/util/Comparator;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Comparator_methods, NULL, NULL, {0}},
    {"Ljava/lang/String;", "Ljava/lang/Object;", {"Ljava/lang/CharSequence;", "Ljava/lang/Comparable;"}, sizeof(JString), ACC_FINAL, String_methods, NULL, String_fin, {0}},
    {"Ljava/lang/StringBuilder;", "Ljava/lang/Object;", {"Ljava/lang/CharSequence;"}, sizeof(SBObj), ACC_FINAL, SB_methods, NULL, SB_fin, {0}},
    {"Ljava/lang/StringBuffer;", "Ljava/lang/Object;", {"Ljava/lang/CharSequence;"}, sizeof(SBObj), ACC_FINAL, SB_methods, NULL, SB_fin, {0}},
    {"Ljava/lang/Number;", "Ljava/lang/Object;", {0}, sizeof(BoxObj), ACC_ABSTRACT, Iface_none, NULL, NULL, {0}},
    {"Ljava/lang/Integer;", "Ljava/lang/Number;", {"Ljava/lang/Comparable;"}, sizeof(BoxObj), ACC_FINAL, Integer_methods, NULL, NULL, {"TYPE:Ljava/lang/Class;"}},
    {"Ljava/lang/Float;", "Ljava/lang/Number;", {"Ljava/lang/Comparable;"}, sizeof(BoxObj), ACC_FINAL, Float_methods, NULL, NULL, {"TYPE:Ljava/lang/Class;"}},
    {"Ljava/lang/Long;", "Ljava/lang/Number;", {"Ljava/lang/Comparable;"}, sizeof(BoxObj), ACC_FINAL, Long_methods, NULL, NULL, {0}},
    {"Ljava/lang/Double;", "Ljava/lang/Number;", {"Ljava/lang/Comparable;"}, sizeof(BoxObj), ACC_FINAL, Double_methods, NULL, NULL, {0}},
    {"Ljava/lang/Boolean;", "Ljava/lang/Object;", {0}, sizeof(BoxObj), ACC_FINAL, Boolean_methods, NULL, NULL, {"TRUE:Ljava/lang/Boolean;", "FALSE:Ljava/lang/Boolean;", "TYPE:Ljava/lang/Class;"}},
    {"Ljava/lang/Math;", "Ljava/lang/Object;", {0}, sizeof(Object), ACC_FINAL, Math_methods, NULL, NULL, {0}},
    {"Landroid/util/FloatMath;", "Ljava/lang/Object;", {0}, sizeof(Object), ACC_FINAL, FloatMath_methods, NULL, NULL, {0}},
    {"Ljava/lang/System;", "Ljava/lang/Object;", {0}, sizeof(Object), ACC_FINAL, System_methods, NULL, NULL, {"out:Ljava/io/PrintStream;", "err:Ljava/io/PrintStream;"}},
    {"Ljava/io/PrintStream;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, PrintStream_methods, NULL, NULL, {0}},
    {"Ljava/lang/Runtime;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, Runtime_methods, NULL, NULL, {0}},
    {"Ljava/lang/Thread;", "Ljava/lang/Object;", {"Ljava/lang/Runnable;"}, sizeof(ThreadObj), 0, Thread_methods, Thread_mark, NULL, {0}},
    {"Ljava/lang/Enum;", "Ljava/lang/Object;", {"Ljava/lang/Comparable;"}, sizeof(EnumObj), ACC_ABSTRACT, Enum_methods, Enum_mark, NULL, {0}},
    {"Ljava/lang/Throwable;", "Ljava/lang/Object;", {0}, sizeof(ThrowableObj), 0, Throwable_methods, Thr_mark, NULL, {0}},
    EXC_DEF("Ljava/lang/Exception;", "Ljava/lang/Throwable;"),
    EXC_DEF("Ljava/lang/Error;", "Ljava/lang/Throwable;"),
    EXC_DEF("Ljava/lang/RuntimeException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/NullPointerException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/ArithmeticException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/ClassCastException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/IllegalArgumentException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/IllegalStateException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/NumberFormatException;", "Ljava/lang/IllegalArgumentException;"),
    EXC_DEF("Ljava/lang/IndexOutOfBoundsException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/ArrayIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;"),
    EXC_DEF("Ljava/lang/StringIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;"),
    EXC_DEF("Ljava/lang/NegativeArraySizeException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/UnsupportedOperationException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/CloneNotSupportedException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/InterruptedException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/ClassNotFoundException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/NoSuchMethodException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/IllegalAccessException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/lang/reflect/InvocationTargetException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/util/ConcurrentModificationException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/util/NoSuchElementException;", "Ljava/lang/RuntimeException;"),
    EXC_DEF("Ljava/lang/StackOverflowError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/OutOfMemoryError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/NoSuchMethodError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/NoSuchFieldError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/NoClassDefFoundError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/AbstractMethodError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/lang/ExceptionInInitializerError;", "Ljava/lang/Error;"),
    EXC_DEF("Ljava/io/IOException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljava/io/EOFException;", "Ljava/io/IOException;"),
    EXC_DEF("Ljava/io/FileNotFoundException;", "Ljava/io/IOException;"),
    EXC_DEF("Lorg/xml/sax/SAXException;", "Ljava/lang/Exception;"),
    EXC_DEF("Ljavax/xml/parsers/ParserConfigurationException;", "Ljava/lang/Exception;"),
    {0}};
