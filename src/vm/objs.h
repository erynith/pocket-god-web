// Layouts of builtin (native-backed) objects.
#ifndef OBJS_H
#define OBJS_H
#include "vm.h"

typedef struct BoxObj { Object h; union { int32_t i; float f; int64_t j; double d; } v; } BoxObj;
typedef struct SBObj { Object h; uint16_t *buf; int len, cap; } SBObj;
typedef struct MethodObj { Object h; Method *m; } MethodObj;
typedef struct ThreadObj { Object h; Object *runnable; } ThreadObj;
typedef struct EnumObj { Object h; JString *name; int32_t ordinal; } EnumObj;

/* java.util */
typedef struct VectorObj { Object h; Object **data; int size, cap; int modcount; } VectorObj;
typedef struct MapEntry { Object h; Object *key; Object *value; int32_t hash; struct MapEntry *next; } MapEntry;
typedef struct MapObj { Object h; MapEntry **table; int cap; int size; int threshold; MapEntry *nullent; int modcount; } MapObj;
typedef struct ViewObj { Object h; Object *owner; int kind; } ViewObj;            // keySet/entrySet/values views
typedef struct IterObj { Object h; Object *owner; int kind; int idx; MapEntry *next; int bucket; MapEntry *last; } IterObj;
typedef struct RandomObj { Object h; int64_t seed; double nextNextGaussian; int haveNext; } RandomObj;

/* java.io / nio */
typedef struct StreamObj {
    Object h;
    Object *inner;      // wrapped stream (Data*Stream) or backing array
    uint8_t *data;      // for byte sources
    int len, pos, cap;
    int owned;          // free data on finalize
    int closed;
    char *fname;        // FileOutputStream name
} StreamObj;
typedef struct BufferObj {
    Object h;
    Object *backing;    // array or parent ByteBuffer
    uint8_t *base;      // element 0
    int capacity, limit, position, mark;
    int esize;          // element size
    int big_endian;
    int owned;          // free base on finalize (allocateDirect)
} BufferObj;

#endif
