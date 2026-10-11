#ifndef DEX_H
#define DEX_H
#include "vm.h"

typedef struct MRef { const char *cls, *name, *sig, *shorty; int sel; } MRef;

struct Dex {
    const uint8_t *b;
    size_t len;
    uint32_t nstrings, string_ids_off;
    uint32_t ntypes, type_ids_off;
    uint32_t nprotos, proto_ids_off;
    uint32_t nfields, field_ids_off;
    uint32_t nmethods, method_ids_off;
    uint32_t nclassdefs, class_defs_off;
    JString **jstrs;   // interned const strings
    Class **types;     // resolved type cache
    Field **fres;      // resolved fields
    Method **mres;     // resolved direct/static methods
    MRef *mref;
    Class **vcache_cls; // inline cache for virtual calls
    Method **vcache_m;
};

uint32_t uleb(const uint8_t **pp);
int32_t sleb(const uint8_t **pp);
const char *dex_string(Dex *d, uint32_t idx);
const char *dex_type(Dex *d, uint32_t idx);
void dex_method_ref(Dex *d, uint32_t idx, const char **cls, const char **name, const char **sig);
void dex_field_ref(Dex *d, uint32_t idx, const char **cls, const char **name, const char **type);
void dex_boot(const uint8_t *b, size_t len);
void for_each_class(void (*fn)(Class *c));
#endif
