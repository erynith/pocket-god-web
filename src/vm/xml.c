// Minimal SAX implementation (javax.xml.parsers / org.xml.sax)
#include "vm.h"
#include "objs.h"

typedef struct ReaderObj { Object h; Object *handler; } ReaderObj;
typedef struct InputSourceObj { Object h; Object *stream; } InputSourceObj;
typedef struct AttrsObj { Object h; int n; JString **names; JString **vals; } AttrsObj;

static void Reader_mark(Object *o) { gc_mark(((ReaderObj *)o)->handler); }
static void IS_mark(Object *o) { gc_mark(((InputSourceObj *)o)->stream); }
static void Attrs_mark(Object *o) {
    AttrsObj *a = (AttrsObj *)o;
    for (int i = 0; i < a->n; i++) { gc_mark((Object *)a->names[i]); gc_mark((Object *)a->vals[i]); }
}
static void Attrs_fin(Object *o) { AttrsObj *a = (AttrsObj *)o; free(a->names); free(a->vals); }

static void noop(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; (void)ret; }
static void SPF_newInstance(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_L(new_object(find_class("Ljavax/xml/parsers/SAXParserFactory;"))); }
static void SPF_newSAXParser(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_L(new_object(find_class("Ljavax/xml/parsers/SAXParser;"))); }
static void SP_getXMLReader(Thread *t, Reg *args, Reg *ret) { (void)t; (void)args; RET_L(new_object(find_class("Lorg/xml/sax/VMXMLReader;"))); }
static void XR_setContentHandler(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((ReaderObj *)ARG_L(0))->handler = ARG_L(1); }
static void ISrc_init(Thread *t, Reg *args, Reg *ret) { (void)t; (void)ret; ((InputSourceObj *)ARG_L(0))->stream = ARG_L(1); }

static void Attrs_getValueS(Thread *t, Reg *args, Reg *ret) {
    (void)t;
    AttrsObj *a = (AttrsObj *)ARG_L(0); JString *n = (JString *)ARG_L(1);
    for (int i = 0; i < a->n; i++) if (jstr_eq(a->names[i], n)) { RET_L(a->vals[i]); return; }
    RET_L(NULL);
}
static void Attrs_getValueI(Thread *t, Reg *args, Reg *ret) { (void)t; AttrsObj *a = (AttrsObj *)ARG_L(0); int i = ARG_I(1); RET_L(i >= 0 && i < a->n ? a->vals[i] : NULL); }
static void Attrs_getNameI(Thread *t, Reg *args, Reg *ret) { (void)t; AttrsObj *a = (AttrsObj *)ARG_L(0); int i = ARG_I(1); RET_L(i >= 0 && i < a->n ? a->names[i] : NULL); }
static void Attrs_getLength(Thread *t, Reg *args, Reg *ret) { (void)t; RET_I(((AttrsObj *)ARG_L(0))->n); }

/* ---- parser ---- */
typedef struct P { const uint8_t *s, *e; Thread *t; Object *h; } P;

static int decode_text(const uint8_t *s, const uint8_t *e, uint16_t *out) {
    // UTF-8 + entities -> UTF-16; returns length
    int n = 0;
    while (s < e) {
        uint32_t c = *s;
        if (c == '&') {
            const uint8_t *semi = s;
            while (semi < e && *semi != ';' && semi - s < 12) semi++;
            if (semi < e && *semi == ';') {
                size_t l = semi - s - 1; const char *nm = (const char *)s + 1;
                uint32_t v = 0; int ok = 1;
                if (l == 3 && !strncmp(nm, "amp", 3)) v = '&';
                else if (l == 2 && !strncmp(nm, "lt", 2)) v = '<';
                else if (l == 2 && !strncmp(nm, "gt", 2)) v = '>';
                else if (l == 4 && !strncmp(nm, "quot", 4)) v = '"';
                else if (l == 4 && !strncmp(nm, "apos", 4)) v = '\'';
                else if (l > 1 && nm[0] == '#') v = (nm[1] == 'x') ? (uint32_t)strtoul(nm + 2, NULL, 16) : (uint32_t)strtoul(nm + 1, NULL, 10);
                else ok = 0;
                if (ok) { out[n++] = (uint16_t)v; s = semi + 1; continue; }
            }
        }
        s++;
        if (c < 0x80) {}
        else if ((c & 0xe0) == 0xc0 && s < e) { c = ((c & 0x1f) << 6) | (*s++ & 0x3f); }
        else if ((c & 0xf0) == 0xe0 && s + 1 < e) { c = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f); s += 2; }
        else if ((c & 0xf8) == 0xf0 && s + 2 < e) {
            c = ((c & 0x07) << 18) | ((s[0] & 0x3f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); s += 3;
            c -= 0x10000; out[n++] = 0xd800 + (c >> 10); out[n++] = 0xdc00 + (c & 0x3ff); continue;
        }
        out[n++] = (uint16_t)c;
    }
    return n;
}
static JString *mkstr(const uint8_t *s, const uint8_t *e) {
    uint16_t *buf = malloc(2 * (size_t)(e - s) + 2);
    int n = decode_text(s, e, buf);
    JString *j = jstr_from_utf16(buf, n);
    free(buf);
    return j;
}
static int is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_name(uint8_t c) { return c && !is_ws(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '?'; }

static int emit_chars(P *p, const uint8_t *s, const uint8_t *e) {
    if (s >= e) return 1;
    uint16_t *buf = malloc(2 * (size_t)(e - s) + 2);
    int n = decode_text(s, e, buf);
    Array *a = new_array(C_arrC, n);
    memcpy(ARR_DATA(a), buf, 2 * (size_t)n);
    free(buf);
    Reg args[4]; R_SETL(args[1], a); R_SETI(args[2], 0); R_SETI(args[3], n);
    call_virtual(p->t, p->h, "characters", "([CII)V", args);
    return !p->t->exception;
}

static void sax_parse(P *p) {
    Thread *t = p->t;
    Reg args[5];
    call_virtual(t, p->h, "startDocument", "()V", args);
    if (t->exception) return;
    const uint8_t *s = p->s, *e = p->e;
    if (e - s >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) s += 3;
    Class *ac = find_class("Lorg/xml/sax/VMAttributes;");
    JString *empty = jstr_from_utf8("");
    const uint8_t *text = s;
    while (s < e) {
        if (*s != '<') { s++; continue; }
        // flush text
        if (!emit_chars(p, text, s)) return;
        if (s + 4 <= e && !memcmp(s, "<!--", 4)) {
            const uint8_t *q = s + 4;
            while (q + 3 <= e && memcmp(q, "-->", 3)) q++;
            s = q + 3; text = s; continue;
        }
        if (s + 9 <= e && !memcmp(s, "<![CDATA[", 9)) {
            const uint8_t *q = s + 9;
            while (q + 3 <= e && memcmp(q, "]]>", 3)) q++;
            // CDATA: raw text, no entity decoding (approximate by emitting directly)
            if (!emit_chars(p, s + 9, q)) return;
            s = q + 3; text = s; continue;
        }
        if (s[1] == '?' || s[1] == '!') {
            while (s < e && *s != '>') s++;
            s++; text = s; continue;
        }
        if (s[1] == '/') {
            const uint8_t *n0 = s + 2, *n1 = n0;
            while (n1 < e && is_name(*n1)) n1++;
            JString *name = mkstr(n0, n1);
            while (s < e && *s != '>') s++;
            s++; text = s;
            R_SETL(args[1], empty); R_SETL(args[2], name); R_SETL(args[3], name);
            call_virtual(t, p->h, "endElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", args);
            if (t->exception) return;
            continue;
        }
        // start tag
        const uint8_t *n0 = s + 1, *n1 = n0;
        while (n1 < e && is_name(*n1)) n1++;
        JString *name = mkstr(n0, n1);
        AttrsObj *at = (AttrsObj *)new_object(ac);
        int cap = 0;
        const uint8_t *q = n1;
        int selfclose = 0;
        for (;;) {
            while (q < e && is_ws(*q)) q++;
            if (q >= e) break;
            if (*q == '/') { selfclose = 1; q++; continue; }
            if (*q == '>') { q++; break; }
            const uint8_t *a0 = q;
            while (q < e && is_name(*q)) q++;
            const uint8_t *a1 = q;
            if (a1 == a0) { q++; continue; }
            while (q < e && is_ws(*q)) q++;
            const uint8_t *v0 = q, *v1 = q;
            if (q < e && *q == '=') {
                q++;
                while (q < e && is_ws(*q)) q++;
                uint8_t quote = *q;
                if (quote == '"' || quote == '\'') {
                    q++; v0 = q;
                    while (q < e && *q != quote) q++;
                    v1 = q; q++;
                } else { v0 = q; while (q < e && !is_ws(*q) && *q != '>') q++; v1 = q; }
            }
            if (at->n == cap) {
                cap = cap ? cap * 2 : 8;
                at->names = realloc(at->names, sizeof(JString *) * cap);
                at->vals = realloc(at->vals, sizeof(JString *) * cap);
            }
            at->names[at->n] = mkstr(a0, a1);
            at->vals[at->n] = mkstr(v0, v1);
            at->n++;
        }
        s = q; text = s;
        R_SETL(args[1], empty); R_SETL(args[2], name); R_SETL(args[3], name); R_SETL(args[4], at);
        call_virtual(t, p->h, "startElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lorg/xml/sax/Attributes;)V", args);
        if (t->exception) return;
        if (selfclose) {
            R_SETL(args[1], empty); R_SETL(args[2], name); R_SETL(args[3], name);
            call_virtual(t, p->h, "endElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", args);
            if (t->exception) return;
        }
    }
    call_virtual(t, p->h, "endDocument", "()V", args);
}

static void XR_parse(Thread *t, Reg *args, Reg *ret) {
    (void)ret;
    ReaderObj *r = (ReaderObj *)ARG_L(0);
    InputSourceObj *src = (InputSourceObj *)ARG_L(1);
    NPE_CHECK(src);
    StreamObj *s = (StreamObj *)src->stream;
    int guard = 0;
    while (s && !s->data && s->inner && guard++ < 16) s = (StreamObj *)s->inner;
    if (!s || !s->data) { throw_new(t, "Lorg/xml/sax/SAXException;", "no input"); return; }
    P p = {s->data + s->pos, s->data + s->len, t, r->handler};
    if (!r->handler) return;
    sax_parse(&p);
    s->pos = s->len;
}

static NativeDef SPF_methods[] = {
    {"newInstance", "()Ljavax/xml/parsers/SAXParserFactory;", SPF_newInstance, 1},
    {"newSAXParser", "()Ljavax/xml/parsers/SAXParser;", SPF_newSAXParser, 0},
    {"setNamespaceAware", "(Z)V", noop, 0},
    {"setValidating", "(Z)V", noop, 0},
    {0}};
static NativeDef SP_methods[] = {
    {"getXMLReader", "()Lorg/xml/sax/XMLReader;", SP_getXMLReader, 0},
    {0}};
static NativeDef XR_methods[] = {
    {"setContentHandler", "(Lorg/xml/sax/ContentHandler;)V", XR_setContentHandler, 0},
    {"parse", "(Lorg/xml/sax/InputSource;)V", XR_parse, 0},
    {0}};
static NativeDef ISrc_methods[] = {
    {"<init>", "(Ljava/io/InputStream;)V", ISrc_init, 0},
    {0}};
static NativeDef DH_methods[] = {
    {"<init>", "()V", noop, 0},
    {"characters", "([CII)V", noop, 0},
    {"startDocument", "()V", noop, 0},
    {"endDocument", "()V", noop, 0},
    {"startElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lorg/xml/sax/Attributes;)V", noop, 0},
    {"endElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", noop, 0},
    {0}};
static NativeDef Attrs_methods[] = {
    {"getValue", "(Ljava/lang/String;)Ljava/lang/String;", Attrs_getValueS, 0},
    {"getValue", "(I)Ljava/lang/String;", Attrs_getValueI, 0},
    {"getQName", "(I)Ljava/lang/String;", Attrs_getNameI, 0},
    {"getLocalName", "(I)Ljava/lang/String;", Attrs_getNameI, 0},
    {"getLength", "()I", Attrs_getLength, 0},
    {0}};
static NativeDef none[] = {{0}};

BuiltinDef xml_builtins[] = {
    {"Ljavax/xml/parsers/SAXParserFactory;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, SPF_methods, NULL, NULL, {0}},
    {"Ljavax/xml/parsers/SAXParser;", "Ljava/lang/Object;", {0}, sizeof(Object), 0, SP_methods, NULL, NULL, {0}},
    {"Lorg/xml/sax/XMLReader;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Lorg/xml/sax/ContentHandler;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Lorg/xml/sax/ErrorHandler;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Lorg/xml/sax/Attributes;", NULL, {0}, 0, ACC_INTERFACE | ACC_ABSTRACT, none, NULL, NULL, {0}},
    {"Lorg/xml/sax/VMXMLReader;", "Ljava/lang/Object;", {"Lorg/xml/sax/XMLReader;"}, sizeof(ReaderObj), 0, XR_methods, Reader_mark, NULL, {0}},
    {"Lorg/xml/sax/InputSource;", "Ljava/lang/Object;", {0}, sizeof(InputSourceObj), 0, ISrc_methods, IS_mark, NULL, {0}},
    {"Lorg/xml/sax/helpers/DefaultHandler;", "Ljava/lang/Object;", {"Lorg/xml/sax/ContentHandler;", "Lorg/xml/sax/ErrorHandler;"}, sizeof(Object), 0, DH_methods, NULL, NULL, {0}},
    {"Lorg/xml/sax/VMAttributes;", "Ljava/lang/Object;", {"Lorg/xml/sax/Attributes;"}, sizeof(AttrsObj), 0, Attrs_methods, Attrs_mark, Attrs_fin, {0}},
    {0}};
