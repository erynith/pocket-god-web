// Minimal Dalvik VM for running a single Android game dex in WebAssembly.
#ifndef VM_H
#define VM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

typedef struct Class Class;
typedef struct Object Object;
typedef struct Method Method;
typedef struct Field Field;
typedef struct Thread Thread;

/* ---------- object model ---------- */
#define GC_MARK 1u
#define GC_PERM 2u

struct Object {
    Class *cls;
    uint32_t gcflags;
    uint32_t hash;   // identity hash (lazily assigned)
};

typedef struct Array {
    Object h;
    int32_t length;
    int32_t pad;
    // data follows, 8-aligned
} Array;
#define ARR_DATA_OFF ((sizeof(Array) + 7) & ~(size_t)7)
#define ARR_DATA(a) ((void *)((uint8_t *)(a) + ARR_DATA_OFF))

typedef struct JString {
    Object h;
    int32_t length;
    int32_t hash;
    int32_t hash_set;
    uint16_t *chars;   // points to inline storage, or malloc'd (constructor-built)
} JString;
#define JSTR_INLINE(s) ((uint16_t *)((uint8_t *)(s) + sizeof(JString)))

/* Registers are 64-bit so native (64-bit pointer) builds work too. */
typedef union Reg {
    int32_t i;
    uint32_t u;
    float f;
    int64_t j;
    double d;
    Object *l;
    uint64_t raw;
} Reg;

#define R_SETI(r, v) ((r).raw = (uint32_t)(int32_t)(v))
#define R_SETF(r, v) do { float _f = (v); uint32_t _u; memcpy(&_u, &_f, 4); (r).raw = _u; } while (0)
#define R_SETL(r, p) ((r).raw = (uint64_t)(uintptr_t)(p))
#define R_SETJ(r, v) ((r).j = (int64_t)(v))
#define R_SETD(r, v) ((r).d = (double)(v))

typedef void (*NativeFn)(Thread *t, Reg *args, Reg *ret);

/* ---------- classes ---------- */
#define ACC_PUBLIC 0x1
#define ACC_PRIVATE 0x2
#define ACC_STATIC 0x8
#define ACC_FINAL 0x10
#define ACC_INTERFACE 0x200
#define ACC_ABSTRACT 0x400
#define ACC_NATIVE 0x100
#define ACC_CONSTRUCTOR 0x10000

struct Field {
    Class *cls;
    const char *name;
    const char *type;
    uint32_t access;
    int offset;      // instance: byte offset in object; static: offset in statics
    int is_static;
};

struct Method {
    Class *cls;
    const char *name;
    const char *sig;     // "(II)V"
    const char *shorty;  // return + params, e.g. "VII"
    int sel;
    uint32_t access;
    int is_static;
    NativeFn native;
    int nargs;           // arg words incl this (wide = 2)
    // dex code
    int has_code;
    uint16_t regs, ins, outs, tries;
    const uint16_t *insns;
    uint32_t insns_size;
    const uint8_t *tries_ptr;
    const uint8_t *handlers_ptr;
};

typedef struct MTab {
    int cap, n;
    int *keys;
    Method **vals;
} MTab;

typedef void (*MarkFn)(Object *o);
typedef void (*FinFn)(Object *o);

struct Class {
    Object h;
    const char *name;     // descriptor
    Class *super;
    Class **ifaces;
    int nifaces;
    uint32_t access;
    int is_array;
    char prim;            // primitive type char for primitive classes, 0 otherwise
    Class *elem;          // array element class
    int elem_size;        // array element byte size
    int inst_size;
    Field *fields;
    int nfields;
    uint8_t *statics;
    int statics_size;
    int *ref_offs;        // instance ref field offsets (incl inherited)
    int nref_offs;
    int *sref_offs;       // static ref offsets
    int nsref_offs;
    Method *methods;      // declared
    int nmethods;
    MTab vt;              // virtual selector -> method (incl inherited)
    int state;            // 0 loaded,1 linking,2 linked,3 initializing,4 initialized
    int dex_idx;          // class def index or -1
    int builtin;
    MarkFn mark;
    FinFn fin;
    Thread *init_thread;
};

/* ---------- threads ---------- */
typedef struct Frame {
    Method *m;
    const uint16_t *pc;
    Reg *regs;
} Frame;

#define TH_RUNNABLE 0
#define TH_SLEEPING 1
#define TH_DEAD 2

struct Thread {
    Reg *stack;
    size_t stack_cap;
    size_t sp;
    Frame *frames;
    int nframes, frames_cap;
    Reg ret;
    Object *exception;
    Object *jthread;
    Object *runnable;
    int state;
    double wake_time;
    int yield_req;
    int nesting;       // number of active interp() invocations
    int green;         // can yield
    int id;
};

/* ---------- globals ---------- */
typedef struct Dex Dex;
extern Dex *g_dex;
extern Thread *g_cur;
extern Class *C_Object, *C_String, *C_Class, *C_Throwable;
extern Class *C_arrI, *C_arrB, *C_arrC, *C_arrF, *C_arrS, *C_arrZ, *C_arrJ, *C_arrD, *C_arrObj, *C_arrString, *C_arrClass;

/* util */
void vm_log(const char *fmt, ...);
void vm_fatal(const char *fmt, ...);
int sel_intern(const char *name, const char *sig);
const char *str_intern(const char *s);

/* method table */
void mtab_put(MTab *t, int key, Method *m);
Method *mtab_get(MTab *t, int key);

/* classes */
Class *find_class(const char *desc);       // loads if needed; NULL if unknown
Class *find_class_or_die(const char *desc);
int class_link(Class *c);
int class_init(Thread *t, Class *c);       // returns 0 on exception
int is_subclass(Class *c, Class *sup);     // instanceof check (c instanceof sup)
Method *class_find_method(Class *c, const char *name, const char *sig); // declared, walking supers
Method *class_find_method_sel(Class *c, int sel);
Field *class_find_field(Class *c, const char *name, const char *type);
Class *array_class_of(Class *elem);
Class *prim_class(char p);

/* builtin registration */
typedef struct NativeDef {
    const char *name;
    const char *sig;
    NativeFn fn;
    int is_static;
} NativeDef;
typedef struct BuiltinDef {
    const char *name;
    const char *super;
    const char *ifaces[6];
    int inst_size;
    uint32_t access;
    NativeDef *methods;
    MarkFn mark;
    FinFn fin;
    const char *sfields[8]; // "name:type" static fields
} BuiltinDef;
void register_builtins(BuiltinDef *defs);
BuiltinDef *find_builtin(const char *desc);

/* heap */
void *gc_alloc(Class *c, size_t size);
Object *new_object(Class *c);
Array *new_array(Class *arrcls, int len);
Array *new_array_desc(const char *desc, int len);
void gc_mark(Object *o);
void gc_add_root(Object **slot);
void gc_collect(void);
size_t gc_bytes_since(void);
int gc_is_heap_ptr(void *p);
uint32_t obj_identity_hash(Object *o);

/* strings */
JString *jstr_new(int len);
JString *jstr_from_utf8(const char *s);
JString *jstr_from_utf16(const uint16_t *c, int n);
JString *jstr_from_mutf8(const char *s);
char *jstr_to_utf8(JString *s);  // malloc'd
int jstr_eq(JString *a, JString *b);
int jstr_eq_c(JString *a, const char *c);
int32_t jstr_hash(JString *s);
JString *jstr_concat(JString *a, JString *b);
JString *obj_to_string(Thread *t, Object *o);
JString *const_string(int idx);

/* interpreter / calls */
Thread *thread_new(int green);
int interp(Thread *t, int base);
Reg call_method(Thread *t, Method *m, Reg *args);
Reg call_virtual(Thread *t, Object *recv, const char *name, const char *sig, Reg *args);
Method *find_virtual(Class *c, const char *name, const char *sig);
void throw_new(Thread *t, const char *cls, const char *msg);
void throw_obj(Thread *t, Object *ex);
#define EXC (t->exception != NULL)

/* boxing */
Object *box_int(int32_t v);
Object *box_float(float v);
Object *box_bool(int v);
int32_t unbox_int(Object *o);
float unbox_float(Object *o);

/* Generic helpers for builtins */
#define ARG_I(n) (args[n].i)
#define ARG_F(n) (args[n].f)
#define ARG_L(n) (args[n].l)
#define ARG_J(n) (args[n].j)
#define ARG_D(n) (args[n].d)
#define RET_I(v) R_SETI(*ret, v)
#define RET_F(v) R_SETF(*ret, v)
#define RET_L(v) R_SETL(*ret, v)
#define RET_J(v) R_SETJ(*ret, v)
#define RET_D(v) R_SETD(*ret, v)

#define NPE_CHECK(o) do { if (!(o)) { throw_new(t, "Ljava/lang/NullPointerException;", NULL); return; } } while (0)

/* registries from modules */
extern BuiltinDef lang_builtins[];
extern BuiltinDef util_builtins[];
extern BuiltinDef io_builtins[];
extern BuiltinDef android_builtins[];
extern BuiltinDef xml_builtins[];

/* resources (R$type) */
const char *res_name_for_id(int id, const char **type);

/* VM entry */

double vm_now_ms(void);

#endif
