// java.util.* natives: Vector, HashMap/Hashtable (Android libcore 2.3 semantics), iterators, Random, Collections.
#include "vm.h"
#include "objs.h"

/* ---------- equality / hashing ---------- */
static Class *C_Box_Integer, *C_Box_Float, *C_Box_Long, *C_Box_Bool;
static void ensure(void) {
    if (!C_Box_Integer) {
        C_Box_Integer = find_class("Ljava/lang/Integer;");
        C_Box_Float = find_class("Ljava/lang/Float;");
        C_Box_Long = find_class("Ljava/lang/Long;");
        C_Box_Bool = find_class("Ljava/lang/Boolean;");
    }
}
static int has_dex_override(Object *o, const char *name, const char *sig, Method **out) {
    Method *m = find_virtual(o->cls, name, sig);
    if (m && !m->native && m->has_code) { *out = m; return 1; }
    return 0;
}
int obj_equals(Thread *t, Object *a, Object *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    ensure();
    if (a->cls == C_String) return b->cls == C_String && jstr_eq((JString *)a, (JString *)b);
    if (a->cls == C_Box_Integer || a->cls == C_Box_Float || a->cls == C_Box_Long || a->cls == C_Box_Bool)
        return a->cls == b->cls && ((BoxObj *)a)->v.j == ((BoxObj *)b)->v.j;
    Method *m;
    if (has_dex_override(a, "equals", "(Ljava/lang/Object;)Z", &m)) {
        Reg args[2]; R_SETL(args[0], a); R_SETL(args[1], b);
        Reg r = call_method(t, m, args);
        return r.i != 0;
    }
    return 0;
}
int32_t obj_hash(Thread *t, Object *o) {
    ensure();
    if (o->cls == C_String) return jstr_hash((JString *)o);
    if (o->cls == C_Box_Integer || o->cls == C_Box_Float || o->cls == C_Box_Bool) {
        if (o->cls == C_Box_Bool) return ((BoxObj *)o)->v.i ? 1231 : 1237;
        return ((BoxObj *)o)->v.i;
    }
    if (o->cls == C_Box_Long) { int64_t v = ((BoxObj *)o)->v.j; return (int32_t)(v ^ ((uint64_t)v >> 32)); }
    Method *m;
    if (has_dex_override(o, "hashCode", "()I", &m)) {
        Reg args[1]; R_SETL(args[0], o);
        return call_method(t, m, args).i;
    }
    return (int32_t)obj_identity_hash(o);
}

/* ---------- Vector ---------- */
#define VEC(n) ((VectorObj *)ARG_L(n))
static void vec_reserve(VectorObj *v, int n) {
    if (n > v->cap) {
        int nc = v->cap ? v->cap * 2 : 10;
        while (nc < n) nc *= 2;
        v->data = realloc(v->data, sizeof(Object *) * nc);
        v->cap = nc;
    }
}
static void vec_add(VectorObj *v, Object *o) { vec_reserve(v, v->size + 1); v->data[v->size++] = o; v->modcount++; }
static void Vec_mark(Object *o) { VectorObj *v = (VectorObj *)o; for (int i = 0; i < v->size; i++) gc_mark(v->data[i]); }
static void Vec_fin(Object *o) { free(((VectorObj *)o)->data); }
static int coll_to_array(Thread *t, Object *c, Object ***out);
static void Vec_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void Vec_initI(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; if (ARG_I(1) > 0) vec_reserve(VEC(0), ARG_I(1)); }
static void Vec_initC(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    Object **items; int n = coll_to_array(t, ARG_L(1), &items);
    for (int i = 0; i < n; i++) vec_add(VEC(0), items[i]);
    free(items);
}
static void Vec_add(Thread *t, Reg *args, Reg *ret) { (void)t; vec_add(VEC(0), ARG_L(1)); RET_I(1); }
static void Vec_addV(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; vec_add(VEC(0), ARG_L(1)); }
static void Vec_addAll(Thread *t, Reg *args, Reg *ret) {
    Object **items; int n = coll_to_array(t, ARG_L(1), &items);
    for (int i = 0; i < n; i++) vec_add(VEC(0), items[i]);
    free(items);
    RET_I(n > 0);
}
static void Vec_clear(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; VEC(0)->size = 0; VEC(0)->modcount++; }
static void Vec_clone(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    VectorObj *v = VEC(0);
    VectorObj *n = (VectorObj *)new_object(v->h.cls);
    vec_reserve(n, v->size);
    if (v->size) memcpy(n->data, v->data, sizeof(Object *) * v->size);
    n->size = v->size;
    RET_L(n);
}
static int vec_index(Thread *t, VectorObj *v, Object *o, int from) {
    for (int i = from; i < v->size; i++) if (obj_equals(t, o, v->data[i]) || (o == NULL && v->data[i] == NULL)) return i;
    return -1;
}
static void Vec_contains(Thread *t, Reg *args, Reg *ret) { RET_I(vec_index(t, VEC(0), ARG_L(1), 0) >= 0); }
static void Vec_indexOf(Thread *t, Reg *args, Reg *ret) { RET_I(vec_index(t, VEC(0), ARG_L(1), 0)); }
static void Vec_copyInto(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    VectorObj *v = VEC(0); Array *a = (Array *)ARG_L(1);
    NPE_CHECK(a);
    if (a->length < v->size) { throw_new(t, "Ljava/lang/ArrayIndexOutOfBoundsException;", NULL); return; }
    memcpy(ARR_DATA(a), v->data, sizeof(Object *) * v->size);
}
static void Vec_toArray(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    VectorObj *v = VEC(0);
    Array *a = new_array(C_arrObj, v->size);
    if (v->size) memcpy(ARR_DATA(a), v->data, sizeof(Object *) * v->size);
    RET_L(a);
}
static void Vec_toArrayA(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    VectorObj *v = VEC(0); Array *a = (Array *)ARG_L(1);
    if (!a || a->length < v->size) a = new_array(a ? a->h.cls : C_arrObj, v->size);
    if (v->size) memcpy(ARR_DATA(a), v->data, sizeof(Object *) * v->size);
    if (a->length > v->size) ((Object **)ARR_DATA(a))[v->size] = NULL;
    RET_L(a);
}
static void map_iter_seek(IterObj *it, int from);
static Class *C_Iter;
static Object *new_iter(Object *owner, int kind) {
    if (!C_Iter) C_Iter = find_class_or_die("Ljava/util/VMIterator;");
    IterObj *it = (IterObj *)new_object(C_Iter);
    it->owner = owner; it->kind = kind; it->idx = 0; it->bucket = -1;
    if (kind != 0) {
        MapObj *m = (MapObj *)owner;
        if (!m->table) { m->table = calloc(2, sizeof(MapEntry *)); m->cap = 2; m->threshold = -1; }
        if (m->nullent) { it->next = m->nullent; it->bucket = -1; }
        else map_iter_seek(it, 0);
    }
    return (Object *)it;
}
static void Vec_elements(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_iter(ARG_L(0), 0)); }
static int check_idx(Thread *t, VectorObj *v, int i, int incl) {
    if (i < 0 || i > v->size || (!incl && i == v->size)) {
        vm_log("Vector index %d out of range (size %d)", i, v->size);
        throw_new(t, "Ljava/lang/ArrayIndexOutOfBoundsException;", NULL);
        return 0;
    }
    return 1;
}
static void Vec_get(Thread *t, Reg *args, Reg *ret) { VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(1), 0)) return; RET_L(v->data[ARG_I(1)]); }
static void Vec_first(Thread *t, Reg *args, Reg *ret) { VectorObj *v = VEC(0); if (!v->size) { throw_new(t, "Ljava/util/NoSuchElementException;", NULL); return; } RET_L(v->data[0]); }
static void Vec_last(Thread *t, Reg *args, Reg *ret) { VectorObj *v = VEC(0); if (!v->size) { throw_new(t, "Ljava/util/NoSuchElementException;", NULL); return; } RET_L(v->data[v->size - 1]); }
static void vec_insert(VectorObj *v, int i, Object *o) {
    vec_reserve(v, v->size + 1);
    memmove(v->data + i + 1, v->data + i, sizeof(Object *) * (v->size - i));
    v->data[i] = o; v->size++; v->modcount++;
}
static void Vec_insertAt(Thread *t, Reg *args, Reg *ret) { (void)ret; VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(2), 1)) return; vec_insert(v, ARG_I(2), ARG_L(1)); }
static void Vec_addAt(Thread *t, Reg *args, Reg *ret) { (void)ret; VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(1), 1)) return; vec_insert(v, ARG_I(1), ARG_L(2)); }
static Object *vec_remove_at(VectorObj *v, int i) {
    Object *o = v->data[i];
    memmove(v->data + i, v->data + i + 1, sizeof(Object *) * (v->size - i - 1));
    v->size--; v->modcount++;
    return o;
}
static void Vec_removeI(Thread *t, Reg *args, Reg *ret) { VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(1), 0)) return; RET_L(vec_remove_at(v, ARG_I(1))); }
static void Vec_removeAtV(Thread *t, Reg *args, Reg *ret) { (void)ret; VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(1), 0)) return; vec_remove_at(v, ARG_I(1)); }
static void Vec_removeO(Thread *t, Reg *args, Reg *ret) {
    VectorObj *v = VEC(0);
    int i = vec_index(t, v, ARG_L(1), 0);
    if (i >= 0) vec_remove_at(v, i);
    RET_I(i >= 0);
}
static void Vec_set(Thread *t, Reg *args, Reg *ret) {
    VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(1), 0)) return;
    Object *old = v->data[ARG_I(1)]; v->data[ARG_I(1)] = ARG_L(2); RET_L(old);
}
static void Vec_setAt(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    VectorObj *v = VEC(0); if (!check_idx(t, v, ARG_I(2), 0)) return;
    v->data[ARG_I(2)] = ARG_L(1);
}
static void Vec_size(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(VEC(0)->size); }
static void Vec_isEmpty(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(VEC(0)->size == 0); }
static void Vec_iterator(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_iter(ARG_L(0), 0)); }
static void Vec_setSize(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    VectorObj *v = VEC(0); int n = ARG_I(1);
    vec_reserve(v, n);
    for (int i = v->size; i < n; i++) v->data[i] = NULL;
    v->size = n;
}
static NativeDef Vector_methods[] = {
    {"<init>", "()V", Vec_init, 0},
    {"<init>", "(I)V", Vec_initI, 0},
    {"<init>", "(Ljava/util/Collection;)V", Vec_initC, 0},
    {"add", "(Ljava/lang/Object;)Z", Vec_add, 0},
    {"add", "(ILjava/lang/Object;)V", Vec_addAt, 0},
    {"addElement", "(Ljava/lang/Object;)V", Vec_addV, 0},
    {"addAll", "(Ljava/util/Collection;)Z", Vec_addAll, 0},
    {"clear", "()V", Vec_clear, 0},
    {"removeAllElements", "()V", Vec_clear, 0},
    {"clone", "()Ljava/lang/Object;", Vec_clone, 0},
    {"contains", "(Ljava/lang/Object;)Z", Vec_contains, 0},
    {"copyInto", "([Ljava/lang/Object;)V", Vec_copyInto, 0},
    {"toArray", "()[Ljava/lang/Object;", Vec_toArray, 0},
    {"toArray", "([Ljava/lang/Object;)[Ljava/lang/Object;", Vec_toArrayA, 0},
    {"elements", "()Ljava/util/Enumeration;", Vec_elements, 0},
    {"get", "(I)Ljava/lang/Object;", Vec_get, 0},
    {"elementAt", "(I)Ljava/lang/Object;", Vec_get, 0},
    {"firstElement", "()Ljava/lang/Object;", Vec_first, 0},
    {"lastElement", "()Ljava/lang/Object;", Vec_last, 0},
    {"indexOf", "(Ljava/lang/Object;)I", Vec_indexOf, 0},
    {"insertElementAt", "(Ljava/lang/Object;I)V", Vec_insertAt, 0},
    {"iterator", "()Ljava/util/Iterator;", Vec_iterator, 0},
    {"remove", "(I)Ljava/lang/Object;", Vec_removeI, 0},
    {"removeElementAt", "(I)V", Vec_removeAtV, 0},
    {"remove", "(Ljava/lang/Object;)Z", Vec_removeO, 0},
    {"removeElement", "(Ljava/lang/Object;)Z", Vec_removeO, 0},
    {"set", "(ILjava/lang/Object;)Ljava/lang/Object;", Vec_set, 0},
    {"setElementAt", "(Ljava/lang/Object;I)V", Vec_setAt, 0},
    {"size", "()I", Vec_size, 0},
    {"isEmpty", "()Z", Vec_isEmpty, 0},
    {"setSize", "(I)V", Vec_setSize, 0},
    {0}};

/* ---------- HashMap (libcore Gingerbread) ---------- */
#define MAP(n) ((MapObj *)ARG_L(n))
static Class *C_Entry;
static int32_t secondary_hash(int32_t hh) {
    uint32_t h = (uint32_t)hh;
    h ^= (h >> 20) ^ (h >> 12);
    return (int32_t)(h ^ (h >> 7) ^ (h >> 4));
}
static void make_table(MapObj *m, int cap) {
    m->table = calloc(cap, sizeof(MapEntry *));
    m->cap = cap;
    m->threshold = (cap >> 1) + (cap >> 2);
}
static void map_init(MapObj *m, int cap) {
    free(m->table);
    if (cap <= 0) { m->table = calloc(2, sizeof(MapEntry *)); m->cap = 2; m->threshold = -1; return; }
    if (cap < 4) cap = 4;
    else { int c = 1; while (c < cap) c <<= 1; cap = c; }
    make_table(m, cap);
}
static void double_capacity(MapObj *m) {
    MapEntry **old = m->table; int oc = m->cap;
    int nc = oc * 2;
    make_table(m, nc);
    MapEntry **nt = m->table;
    if (m->size == 0) { free(old); return; }
    for (int j = 0; j < oc; j++) {
        MapEntry *e = old[j];
        if (!e) continue;
        int high = e->hash & oc;
        MapEntry *broken = NULL;
        nt[j | high] = e;
        for (MapEntry *n = e->next; n; e = n, n = n->next) {
            int nh = n->hash & oc;
            if (nh != high) {
                if (!broken) nt[j | nh] = n; else broken->next = n;
                broken = e;
                high = nh;
            }
        }
        if (broken) broken->next = NULL;
    }
    free(old);
}
static MapEntry *map_find(Thread *t, MapObj *m, Object *key) {
    if (!m->table) map_init(m, 0);
    if (!key) return m->nullent;
    int32_t h = secondary_hash(obj_hash(t, key));
    for (MapEntry *e = m->table[h & (m->cap - 1)]; e; e = e->next)
        if (e->key == key || (e->hash == h && obj_equals(t, key, e->key))) return e;
    return NULL;
}
static Object *map_put(Thread *t, MapObj *m, Object *key, Object *val) {
    if (!m->table) map_init(m, 0);
    if (!C_Entry) C_Entry = find_class_or_die("Ljava/util/HashMap$HashMapEntry;");
    if (!key) {
        if (m->nullent) { Object *o = m->nullent->value; m->nullent->value = val; return o; }
        MapEntry *e = (MapEntry *)new_object(C_Entry);
        e->value = val; m->nullent = e; m->size++; m->modcount++;
        return NULL;
    }
    int32_t h = secondary_hash(obj_hash(t, key));
    int idx = h & (m->cap - 1);
    for (MapEntry *e = m->table[idx]; e; e = e->next)
        if (e->key == key || (e->hash == h && obj_equals(t, key, e->key))) { Object *o = e->value; e->value = val; return o; }
    m->modcount++;
    if (m->size++ > m->threshold) { double_capacity(m); idx = h & (m->cap - 1); }
    MapEntry *e = (MapEntry *)new_object(C_Entry);
    e->key = key; e->value = val; e->hash = h; e->next = m->table[idx];
    m->table[idx] = e;
    return NULL;
}
static Object *map_remove(Thread *t, MapObj *m, Object *key) {
    if (!m->table) map_init(m, 0);
    if (!key) {
        if (!m->nullent) return NULL;
        Object *o = m->nullent->value; m->nullent = NULL; m->size--; m->modcount++; return o;
    }
    int32_t h = secondary_hash(obj_hash(t, key));
    int idx = h & (m->cap - 1);
    for (MapEntry *e = m->table[idx], *prev = NULL; e; prev = e, e = e->next) {
        if (e->key == key || (e->hash == h && obj_equals(t, key, e->key))) {
            if (prev) prev->next = e->next; else m->table[idx] = e->next;
            m->size--; m->modcount++;
            return e->value;
        }
    }
    return NULL;
}
static void Map_mark(Object *o) {
    MapObj *m = (MapObj *)o;
    for (int i = 0; i < m->cap; i++) gc_mark((Object *)m->table[i]);
    gc_mark((Object *)m->nullent);
}
static void Map_fin(Object *o) { free(((MapObj *)o)->table); }
static void Entry_mark(Object *o) { MapEntry *e = (MapEntry *)o; gc_mark(e->key); gc_mark(e->value); gc_mark((Object *)e->next); }
static void Map_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; map_init(MAP(0), 0); }
static void Map_initI(Thread *t, Reg *args, Reg *ret) { (void)ret; if (ARG_I(1) < 0) { throw_new(t, "Ljava/lang/IllegalArgumentException;", NULL); return; } map_init(MAP(0), ARG_I(1)); }
static void Map_clear(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    MapObj *m = MAP(0);
    if (!m->table) map_init(m, 0);
    if (m->size) { memset(m->table, 0, sizeof(MapEntry *) * m->cap); m->nullent = NULL; m->size = 0; m->modcount++; }
}
static void Map_containsKey(Thread *t, Reg *args, Reg *ret) { RET_I(map_find(t, MAP(0), ARG_L(1)) != NULL); }
static void Map_get(Thread *t, Reg *args, Reg *ret) { MapEntry *e = map_find(t, MAP(0), ARG_L(1)); RET_L(e ? e->value : NULL); }
static void Map_put(Thread *t, Reg *args, Reg *ret) { RET_L(map_put(t, MAP(0), ARG_L(1), ARG_L(2))); }
static void Map_remove(Thread *t, Reg *args, Reg *ret) { RET_L(map_remove(t, MAP(0), ARG_L(1))); }
static void Map_size(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(MAP(0)->size); }
static void Map_isEmpty(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(MAP(0)->size == 0); }
static Class *C_View;
static Object *new_view(Object *owner, int kind) {
    if (!C_View) C_View = find_class_or_die("Ljava/util/VMMapView;");
    ViewObj *v = (ViewObj *)new_object(C_View);
    v->owner = owner; v->kind = kind;
    return (Object *)v;
}
static void Map_keySet(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_view(ARG_L(0), 1)); }
static void Map_entrySet(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_view(ARG_L(0), 2)); }
static void Map_values(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_view(ARG_L(0), 3)); }
static void Map_keys(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_iter(ARG_L(0), 1)); }
static void Map_elements(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(new_iter(ARG_L(0), 3)); }
static void Map_containsValue(Thread *t, Reg *args, Reg *ret) {
    MapObj *m = MAP(0);
    if (m->nullent && obj_equals(t, m->nullent->value, ARG_L(1))) { RET_I(1); return; }
    for (int i = 0; i < m->cap; i++) for (MapEntry *e = m->table[i]; e; e = e->next) if (obj_equals(t, e->value, ARG_L(1))) { RET_I(1); return; }
    RET_I(0);
}
static NativeDef HashMap_methods[] = {
    {"<init>", "()V", Map_init, 0},
    {"<init>", "(I)V", Map_initI, 0},
    {"clear", "()V", Map_clear, 0},
    {"containsKey", "(Ljava/lang/Object;)Z", Map_containsKey, 0},
    {"containsValue", "(Ljava/lang/Object;)Z", Map_containsValue, 0},
    {"get", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_get, 0},
    {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", Map_put, 0},
    {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_remove, 0},
    {"size", "()I", Map_size, 0},
    {"isEmpty", "()Z", Map_isEmpty, 0},
    {"keySet", "()Ljava/util/Set;", Map_keySet, 0},
    {"entrySet", "()Ljava/util/Set;", Map_entrySet, 0},
    {"values", "()Ljava/util/Collection;", Map_values, 0},
    {"keys", "()Ljava/util/Enumeration;", Map_keys, 0},
    {"elements", "()Ljava/util/Enumeration;", Map_elements, 0},
    {0}};

static void Entry_getKey(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(((MapEntry *)ARG_L(0))->key); }
static void Entry_getValue(Thread *t, Reg *args, Reg *ret) { (void)t; RET_L(((MapEntry *)ARG_L(0))->value); }
static void Entry_setValue(Thread *t, Reg *args, Reg *ret) { (void)t; MapEntry *e = (MapEntry *)ARG_L(0); Object *o = e->value; e->value = ARG_L(1); RET_L(o); }
static NativeDef Entry_methods[] = {
    {"getKey", "()Ljava/lang/Object;", Entry_getKey, 0},
    {"getValue", "()Ljava/lang/Object;", Entry_getValue, 0},
    {"setValue", "(Ljava/lang/Object;)Ljava/lang/Object;", Entry_setValue, 0},
    {0}};

/* map views */
static void View_mark(Object *o) { gc_mark(((ViewObj *)o)->owner); }
static void View_iterator(Thread *t, Reg *args, Reg *ret) { (void)t; ViewObj *v = (ViewObj *)ARG_L(0); RET_L(new_iter(v->owner, v->kind)); }
static void View_size(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((MapObj *)((ViewObj *)ARG_L(0))->owner)->size); }
static void View_contains(Thread *t, Reg *args, Reg *ret) {
    ViewObj *v = (ViewObj *)ARG_L(0);
    if (v->kind == 1) { RET_I(map_find(t, (MapObj *)v->owner, ARG_L(1)) != NULL); return; }
    RET_I(0);
}
static void View_toArray(Thread *t, Reg *args, Reg *ret);
static NativeDef View_methods[] = {
    {"iterator", "()Ljava/util/Iterator;", View_iterator, 0},
    {"size", "()I", View_size, 0},
    {"contains", "(Ljava/lang/Object;)Z", View_contains, 0},
    {"toArray", "()[Ljava/lang/Object;", View_toArray, 0},
    {0}};

/* ---------- iterator (vector, map) ---------- */
static void Iter_mark(Object *o) { IterObj *i = (IterObj *)o; gc_mark(i->owner); gc_mark((Object *)i->next); gc_mark((Object *)i->last); }
static void map_iter_seek(IterObj *it, int from) {
    MapObj *m = (MapObj *)it->owner;
    it->next = NULL;
    for (int b = from; b < m->cap; b++) if (m->table[b]) { it->next = m->table[b]; it->bucket = b; return; }
    it->bucket = m->cap;
}
static void Iter_hasNext(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    IterObj *it = (IterObj *)ARG_L(0);
    if (it->kind == 0) { RET_I(it->idx < ((VectorObj *)it->owner)->size); return; }
    RET_I(it->next != NULL);
}
static void Iter_next(Thread *t, Reg *args, Reg *ret) {
    IterObj *it = (IterObj *)ARG_L(0);
    if (it->kind == 0) {
        VectorObj *v = (VectorObj *)it->owner;
        if (it->idx >= v->size) { throw_new(t, "Ljava/util/NoSuchElementException;", NULL); return; }
        RET_L(v->data[it->idx++]);
        return;
    }
    MapEntry *e = it->next;
    if (!e) { throw_new(t, "Ljava/util/NoSuchElementException;", NULL); return; }
    it->last = e;
    if (it->bucket < 0) map_iter_seek(it, 0);
    else if (e->next) it->next = e->next;
    else map_iter_seek(it, it->bucket + 1);
    if (it->kind == 1) RET_L(e->key);
    else if (it->kind == 2) RET_L(e);
    else RET_L(e->value);
}
static void Iter_remove(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    IterObj *it = (IterObj *)ARG_L(0);
    if (it->kind == 0) { VectorObj *v = (VectorObj *)it->owner; if (it->idx > 0) { vec_remove_at(v, it->idx - 1); it->idx--; } return; }
    if (it->last) { map_remove(t, (MapObj *)it->owner, it->last->key); it->last = NULL; }
}
static NativeDef Iter_methods[] = {
    {"hasNext", "()Z", Iter_hasNext, 0},
    {"next", "()Ljava/lang/Object;", Iter_next, 0},
    {"remove", "()V", Iter_remove, 0},
    {"hasMoreElements", "()Z", Iter_hasNext, 0},
    {"nextElement", "()Ljava/lang/Object;", Iter_next, 0},
    {0}};

/* collect any collection into a C array (Vector, map view, or generic iterable) */
static int coll_to_array(Thread *t, Object *c, Object ***out) {
    *out = NULL;
    if (!c) { throw_new(t, "Ljava/lang/NullPointerException;", NULL); return 0; }
    Class *vc = find_class("Ljava/util/Vector;");
    if (is_subclass(c->cls, vc) || is_subclass(c->cls, find_class("Ljava/util/ArrayList;"))) {
        VectorObj *v = (VectorObj *)c;
        *out = malloc(sizeof(Object *) * (v->size + 1));
        if (v->size) memcpy(*out, v->data, sizeof(Object *) * v->size);
        return v->size;
    }
    // generic: use iterator()
    Reg a[1];
    Reg it = call_virtual(t, c, "iterator", "()Ljava/util/Iterator;", a);
    if (t->exception || !it.l) return 0;
    int n = 0, cap = 16;
    *out = malloc(sizeof(Object *) * cap);
    for (;;) {
        Reg h = call_virtual(t, it.l, "hasNext", "()Z", a);
        if (t->exception || !h.i) break;
        Reg x = call_virtual(t, it.l, "next", "()Ljava/lang/Object;", a);
        if (t->exception) break;
        if (n == cap) { cap *= 2; *out = realloc(*out, sizeof(Object *) * cap); }
        (*out)[n++] = x.l;
    }
    return n;
}
static void View_toArray(Thread *t, Reg *args, Reg *ret) {
    Object **items; int n = coll_to_array(t, ARG_L(0), &items);
    Array *a = new_array(C_arrObj, n);
    if (n) memcpy(ARR_DATA(a), items, sizeof(Object *) * n);
    free(items);
    RET_L(a);
}

/* ---------- Collections ---------- */
static Thread *g_sort_t; static Object *g_sort_cmp; static int g_sort_err;
static int sort_cmp(Object *a, Object *b) {
    if (g_sort_err) return 0;
    Reg args[3]; R_SETL(args[1], a); R_SETL(args[2], b);
    if (!g_sort_cmp) {
        Reg r = call_virtual(g_sort_t, a, "compareTo", "(Ljava/lang/Object;)I", args);
        if (g_sort_t->exception) { g_sort_err = 1; return 0; }
        return r.i;
    }
    Reg r = call_virtual(g_sort_t, g_sort_cmp, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", args);
    if (g_sort_t->exception) { g_sort_err = 1; return 0; }
    return r.i;
}
static void merge_sort(Object **a, Object **tmp, int n) {
    if (n < 2) return;
    if (n < 7) { // insertion sort (stable)
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && sort_cmp(a[j - 1], a[j]) > 0; j--) { Object *x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; }
        return;
    }
    int h = n / 2;
    merge_sort(a, tmp, h);
    merge_sort(a + h, tmp, n - h);
    if (sort_cmp(a[h - 1], a[h]) <= 0) return;
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = (sort_cmp(a[i], a[j]) <= 0) ? a[i++] : a[j++];
    while (i < h) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, sizeof(Object *) * n);
}
static void Coll_sort(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    VectorObj *v = (VectorObj *)ARG_L(0);
    NPE_CHECK(v);
    Object **tmp = malloc(sizeof(Object *) * (v->size + 1));
    Thread *ps = g_sort_t; Object *pc = g_sort_cmp; int pe = g_sort_err;
    g_sort_t = t; g_sort_cmp = ARG_L(1); g_sort_err = 0;
    merge_sort(v->data, tmp, v->size);
    g_sort_t = ps; g_sort_cmp = pc; g_sort_err = pe;
    free(tmp);
}
static void Coll_sort1(Thread *t, Reg *args, Reg *ret) {
    Reg a2[2] = {args[0]}; a2[1].raw = 0;
    Coll_sort(t, a2, ret);
}
static NativeDef Collections_methods[] = {
    {"sort", "(Ljava/util/List;Ljava/util/Comparator;)V", Coll_sort, 1},
    {"sort", "(Ljava/util/List;)V", Coll_sort1, 1},
    {0}};

/* ---------- Random (java.util.Random LCG) ---------- */
#define RND(n) ((RandomObj *)ARG_L(n))
static const int64_t MULT = 0x5DEECE66DLL, MASK = (1LL << 48) - 1;
static int64_t g_uniq = 8682522807148012LL;
static void rnd_set(RandomObj *r, int64_t s) { r->seed = (s ^ MULT) & MASK; r->haveNext = 0; }
static int32_t rnd_next(RandomObj *r, int bits) {
    r->seed = (int64_t)(((uint64_t)r->seed * (uint64_t)MULT + 0xBULL) & (uint64_t)MASK);
    return (int32_t)((uint64_t)r->seed >> (48 - bits));
}
static void Rnd_init(Thread *t, Reg *args, Reg *ret) {
    (void)t; (void)ret;
    g_uniq = (int64_t)((uint64_t)g_uniq * 181783497276652981ULL);
    rnd_set(RND(0), g_uniq ^ (int64_t)(vm_now_ms() * 1000000.0));
}
static void Rnd_initJ(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; rnd_set(RND(0), ARG_J(1)); }
static void Rnd_setSeed(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; rnd_set(RND(0), ARG_J(1)); }
static void Rnd_nextInt(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(rnd_next(RND(0), 32)); }
static void Rnd_nextIntN(Thread *t, Reg *args, Reg *ret) {
    RandomObj *r = RND(0); int32_t n = ARG_I(1);
    if (n <= 0) { throw_new(t, "Ljava/lang/IllegalArgumentException;", "n <= 0"); return; }
    if ((n & -n) == n) { RET_I((int32_t)(((int64_t)n * (int64_t)rnd_next(r, 31)) >> 31)); return; }
    int32_t bits, val;
    do { bits = rnd_next(r, 31); val = bits % n; } while ((int32_t)((uint32_t)bits - (uint32_t)val + (uint32_t)(n - 1)) < 0);
    RET_I(val);
}
static void Rnd_nextDouble(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    RandomObj *r = RND(0);
    int64_t a = rnd_next(r, 26), b = rnd_next(r, 27);
    RET_D((double)((a << 27) + b) / (double)(1LL << 53));
}
static void Rnd_nextFloat(Thread *t, Reg *args, Reg *ret) { (void)t; RET_F(rnd_next(RND(0), 24) / (float)(1 << 24)); }
static void Rnd_nextBoolean(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(rnd_next(RND(0), 1) != 0); }
static NativeDef Random_methods[] = {
    {"<init>", "()V", Rnd_init, 0},
    {"<init>", "(J)V", Rnd_initJ, 0},
    {"setSeed", "(J)V", Rnd_setSeed, 0},
    {"nextInt", "()I", Rnd_nextInt, 0},
    {"nextInt", "(I)I", Rnd_nextIntN, 0},
    {"nextDouble", "()D", Rnd_nextDouble, 0},
    {"nextFloat", "()F", Rnd_nextFloat, 0},
    {"nextBoolean", "()Z", Rnd_nextBoolean, 0},
    {0}};

static NativeDef none[] = {{0}};
static NativeDef Iterator_iface[] = {{"hasNext", "()Z", NULL, 0}, {"next", "()Ljava/lang/Object;", NULL, 0}, {0}};

BuiltinDef util_builtins[] = {
    {"Ljava/util/Collection;", NULL, {"Ljava/lang/Iterable;"}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/List;", NULL, {"Ljava/util/Collection;"}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Set;", NULL, {"Ljava/util/Collection;"}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Map;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Map$Entry;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Iterator;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, Iterator_iface, NULL, NULL, {0}},
    {"Ljava/util/Enumeration;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/RandomAccess;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/AbstractList;", "Ljava/lang/Object;", {"Ljava/util/List;"}, sizeof(VectorObj), ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Vector;", "Ljava/util/AbstractList;", {"Ljava/util/List;", "Ljava/util/RandomAccess;", "Ljava/lang/Cloneable;"}, sizeof(VectorObj), 0, Vector_methods, Vec_mark, Vec_fin, {0}},
    {"Ljava/util/ArrayList;", "Ljava/util/AbstractList;", {"Ljava/util/List;", "Ljava/util/RandomAccess;", "Ljava/lang/Cloneable;"}, sizeof(VectorObj), 0, Vector_methods, Vec_mark, Vec_fin, {0}},
    {"Ljava/util/AbstractMap;", "Ljava/lang/Object;", {"Ljava/util/Map;"}, sizeof(MapObj), ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/HashMap;", "Ljava/util/AbstractMap;", {"Ljava/util/Map;", "Ljava/lang/Cloneable;"}, sizeof(MapObj), 0, HashMap_methods, Map_mark, Map_fin, {0}},
    {"Ljava/util/Dictionary;", "Ljava/lang/Object;", {0}, sizeof(MapObj), ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Ljava/util/Hashtable;", "Ljava/util/Dictionary;", {"Ljava/util/Map;", "Ljava/lang/Cloneable;"}, sizeof(MapObj), 0, HashMap_methods, Map_mark, Map_fin, {0}},
    {"Ljava/util/HashMap$HashMapEntry;", "Ljava/lang/Object;", {"Ljava/util/Map$Entry;"}, sizeof(MapEntry), 0, Entry_methods, Entry_mark, NULL, {0}},
    {"Ljava/util/VMMapView;", "Ljava/lang/Object;", {"Ljava/util/Set;"}, sizeof(ViewObj), 0, View_methods, View_mark, NULL, {0}},
    {"Ljava/util/VMIterator;", "Ljava/lang/Object;", {"Ljava/util/Iterator;", "Ljava/util/Enumeration;"}, sizeof(IterObj), 0, Iter_methods, Iter_mark, NULL, {0}},
    {"Ljava/util/Collections;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, Collections_methods, NULL, NULL, {0}},
    {"Ljava/util/Random;", "Ljava/lang/Object;", {0}, sizeof(RandomObj), 0, Random_methods, NULL, NULL, {0}},
    {0}};
