/*
 * rawc_compat.h — Pure C implementations of mypyc runtime functions.
 *
 * Strings are UCS-4: [int64_t len][uint32_t data[len]]
 * The intptr_t points to data (past the length prefix).
 * Character index == array index. No UTF-8 decoding needed.
 */

#ifndef RAWC_COMPAT_H
#define RAWC_COMPAT_H

#include "rawc_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <wctype.h>

/* ---- Shared state ---- */

typedef intptr_t CPyPtr;

#ifndef Py_OBJECT_H
typedef struct PyVarObject {
    intptr_t ob_refcnt; intptr_t ob_type; intptr_t ob_size;
} PyVarObject;
#endif

static intptr_t __rawc_void;

/*
 * String representation: UCS-4, length-prefixed.
 * Memory layout: [int64_t len][uint32_t data[len+1]]
 * The intptr_t points to data[0] (past the length prefix).
 * Length (in characters) is at ((int64_t *)ptr)[-1].
 * data[len] == 0 (null-terminated for convenience).
 */

/* Get string length (character count) in O(1) */
#define rawc_str_len(s) (((int64_t *)(s))[-1])

/* Get UCS-4 data pointer */
#define rawc_str_data(s) ((const uint32_t *)(s))

/* Allocate a new UCS-4 string from uint32_t array */
static inline intptr_t rawc_str_new_ucs4(const uint32_t *src, int64_t len) {
    int64_t *block = (int64_t *)rawc_alloc(sizeof(int64_t) + (len + 1) * sizeof(uint32_t));
    block[0] = len;
    uint32_t *data = (uint32_t *)(block + 1);
    if (src) memcpy(data, src, len * sizeof(uint32_t));
    data[len] = 0;
    return (intptr_t)data;
}

/* Allocate a new UCS-4 string from UTF-8 C string */
static inline intptr_t rawc_str_new(const char *src, int64_t byte_len) {
    /* Count characters */
    int64_t char_count = 0;
    for (int64_t i = 0; i < byte_len; ) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else i += 4;
        char_count++;
    }
    int64_t *block = (int64_t *)rawc_alloc(sizeof(int64_t) + (char_count + 1) * sizeof(uint32_t));
    block[0] = char_count;
    uint32_t *data = (uint32_t *)(block + 1);
    int64_t pos = 0;
    for (int64_t i = 0; i < char_count; i++) {
        unsigned char c = (unsigned char)src[pos];
        if (c < 0x80) { data[i] = c; pos += 1; }
        else if (c < 0xE0) { data[i] = ((c & 0x1F) << 6) | (src[pos+1] & 0x3F); pos += 2; }
        else if (c < 0xF0) { data[i] = ((c & 0x0F) << 12) | ((src[pos+1] & 0x3F) << 6) | (src[pos+2] & 0x3F); pos += 3; }
        else { data[i] = ((c & 0x07) << 18) | ((src[pos+1] & 0x3F) << 12) | ((src[pos+2] & 0x3F) << 6) | (src[pos+3] & 0x3F); pos += 4; }
    }
    data[char_count] = 0;
    return (intptr_t)data;
}

/* Pre-built single-char strings for ASCII (0-127) */
typedef struct { int64_t len; uint32_t data[2]; } RawcCharEntry;
extern RawcCharEntry rawc_char_entries[256];
extern uint32_t *rawc_char_strings[256]; /* pointers into rawc_char_entries[i].data */
#ifdef RAWC_COMPAT_IMPL
RawcCharEntry rawc_char_entries[256];
uint32_t *rawc_char_strings[256];
#endif

__attribute__((constructor))
static void rawc_init_char_strings(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    for (int i = 0; i < 256; i++) {
        rawc_char_entries[i].len = 1;
        rawc_char_entries[i].data[0] = (uint32_t)i;
        rawc_char_entries[i].data[1] = 0;
        rawc_char_strings[i] = rawc_char_entries[i].data;
    }
}

/* Empty string constant */
static int64_t rawc_empty_str_block[2] = {0, 0};
#define RAWC_EMPTY_STR ((intptr_t)((char *)rawc_empty_str_block + sizeof(int64_t)))

/* Get the first UCS-4 code point of a string */
static inline uint32_t rawc_str_first_char(intptr_t v) {
    if (!v || (uintptr_t)v < 4096) return 0;
    return ((const uint32_t *)v)[0];
}

/* Get length, handling all representations */
static inline int64_t rawc_strlen(intptr_t v) {
    if ((uintptr_t)v <= 4096) return 0;
    return rawc_str_len(v);
}

/* Generic Python truthiness for rawc objects.
 * Strings: [int64_t len][@ptr data...] — len at ptr[-1]
 * Dicts/Sets: {entries*, len, ...} — len at offset 8 = ptr[1]
 * Lists: {data*, len, cap} — len at offset 8 = ptr[1]
 * Check both: non-empty string OR non-empty container. */
static inline char rawc_is_truthy(intptr_t v) {
    if (!v) return 0;
    if ((uintptr_t)v < 4096) return 1; /* small integer, always truthy */
    /* String: len at [-1] */
    int64_t str_len = ((int64_t *)v)[-1];
    if (str_len > 0) return 1;
    /* Dict/Set/List: len at [1] (offset 8) */
    int64_t container_len = ((int64_t *)v)[1];
    if (container_len > 0) return 1;
    return 0;
}

/* Convert UCS-4 string to UTF-8 (arena-allocated) for dict hashing etc. */
static inline const char *rawc_to_utf8(intptr_t s, int64_t *out_len) {
    if (!s || (uintptr_t)s < 4096) { *out_len = 0; return ""; }
    int64_t len = rawc_str_len(s);
    const uint32_t *data = (const uint32_t *)s;
    /* Calculate UTF-8 byte length */
    int64_t byte_len = 0;
    for (int64_t i = 0; i < len; i++) {
        uint32_t ch = data[i];
        if (ch < 0x80) byte_len += 1;
        else if (ch < 0x800) byte_len += 2;
        else if (ch < 0x10000) byte_len += 3;
        else byte_len += 4;
    }
    char *buf = (char *)rawc_alloc(byte_len + 1);
    int64_t pos = 0;
    for (int64_t i = 0; i < len; i++) {
        uint32_t ch = data[i];
        if (ch < 0x80) { buf[pos++] = (char)ch; }
        else if (ch < 0x800) { buf[pos++] = 0xC0 | (ch >> 6); buf[pos++] = 0x80 | (ch & 0x3F); }
        else if (ch < 0x10000) { buf[pos++] = 0xE0 | (ch >> 12); buf[pos++] = 0x80 | ((ch >> 6) & 0x3F); buf[pos++] = 0x80 | (ch & 0x3F); }
        else { buf[pos++] = 0xF0 | (ch >> 18); buf[pos++] = 0x80 | ((ch >> 12) & 0x3F); buf[pos++] = 0x80 | ((ch >> 6) & 0x3F); buf[pos++] = 0x80 | (ch & 0x3F); }
    }
    buf[pos] = '\0';
    *out_len = byte_len;
    return buf;
}

/* ---- String operations ---- */

static inline int rawc_unicode_isspace(uint32_t ch) {
    if (ch < 128) return isspace((int)ch);
    /* Unicode space characters: Zs category + U+2028/U+2029 */
    if (ch == 0x00A0 || ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200A) ||
        ch == 0x2028 || ch == 0x2029 || ch == 0x202F || ch == 0x205F || ch == 0x3000)
        return 1;
    return 0;
}

static inline char CPyStr_IsSpace(intptr_t s) {
    uint32_t ch = rawc_str_first_char(s);
    return (ch && rawc_unicode_isspace(ch)) ? 1 : 0;
}

static inline int rawc_unicode_isalnum(uint32_t ch) {
    if (ch < 128) return isalnum((int)ch);
    /* Exclude known non-alnum Unicode chars: whitespace and punctuation */
    if (rawc_unicode_isspace(ch)) return 0;
    /* Use wctype for proper Unicode letter/digit classification */
    return iswalnum((wint_t)ch);
}

static inline char CPyStr_IsAlnum(intptr_t s) {
    uint32_t ch = rawc_str_first_char(s);
    if (!ch) return 0;
    return rawc_unicode_isalnum(ch) ? 1 : 0;
}

static inline char CPyStr_IsIdentifier(intptr_t s) {
    /* For a single-character string: check if it's a valid Python identifier start.
       For multi-character strings: check all chars against identifier continue rules.
       ASCII: [a-zA-Z_] for start, [a-zA-Z0-9_] for continue; non-ASCII >= 128 accepted. */
    int64_t len = rawc_strlen(s);
    if (!len) return 0;
    const uint32_t *data = rawc_str_data(s);
    uint32_t ch = data[0];
    /* First char must be ID_Start: letter, underscore, or non-ASCII */
    if (ch < 128) {
        if (!isalpha((int)ch) && ch != '_') return 0;
    }
    /* Non-ASCII chars >= 128 are accepted (Unicode letters) */
    for (int64_t i = 1; i < len; i++) {
        ch = data[i];
        if (ch < 128) {
            if (!isalnum((int)ch) && ch != '_') return 0;
        }
        /* Non-ASCII >= 128 accepted */
    }
    return 1;
}

static inline char CPyStr_IsDigit(intptr_t s) {
    int64_t len = rawc_strlen(s);
    if (!len) return 0;
    const uint32_t *data = rawc_str_data(s);
    for (int64_t i = 0; i < len; i++)
        if (data[i] >= 128 || !isdigit((int)data[i])) return 0;
    return 1;
}

static inline char CPyStr_IsTrue(intptr_t s) {
    if (!s) return 0;
    return rawc_strlen(s) > 0 ? 1 : 0;
}

static inline char CPyStr_Equal(intptr_t a, intptr_t b) {
    if (a == b) return 1;
    int64_t la = rawc_strlen(a), lb = rawc_strlen(b);
    if (la != lb) return 0;
    return memcmp((const void *)a, (const void *)b, la * sizeof(uint32_t)) == 0 ? 1 : 0;
}

static inline intptr_t CPyStr_Upper(intptr_t s) {
    int64_t len = rawc_strlen(s);
    if (!len) return s;
    const uint32_t *src = rawc_str_data(s);
    if (len == 1 && src[0] < 128) return (intptr_t)rawc_char_strings[toupper((int)src[0])];
    intptr_t result = rawc_str_new_ucs4(src, len);
    uint32_t *dst = (uint32_t *)result;
    for (int64_t i = 0; i < len; i++)
        if (dst[i] < 128) dst[i] = toupper((int)dst[i]);
    return result;
}

static inline intptr_t CPyStr_Append(intptr_t a, intptr_t b) {
    int64_t la = rawc_strlen(a), lb = rawc_strlen(b);
    int64_t total = la + lb;
    intptr_t result = rawc_str_new_ucs4(NULL, total);
    uint32_t *dst = (uint32_t *)result;
    if (la) memcpy(dst, (const void *)a, la * sizeof(uint32_t));
    if (lb) memcpy(dst + la, (const void *)b, lb * sizeof(uint32_t));
    dst[total] = 0;
    return result;
}

static inline intptr_t PyUnicode_Concat(intptr_t a, intptr_t b) {
    return CPyStr_Append(a, b);
}

static inline intptr_t CPyStr_GetSlice(intptr_t s, int64_t start, int64_t end) {
    int64_t len = rawc_strlen(s);
    if (start < 0) { start += len; if (start < 0) start = 0; }
    if (end < 0) { end += len; if (end < 0) end = 0; }
    if (end > len) end = len;
    if (start >= end) return RAWC_EMPTY_STR;
    int64_t slen = end - start;
    const uint32_t *src = rawc_str_data(s);
    if (slen == 1 && src[start] < 256) return (intptr_t)rawc_char_strings[src[start]];
    return rawc_str_new_ucs4(src + start, slen);
}

static inline int64_t CPyStr_Find(intptr_t s, intptr_t sub, int64_t start, int64_t _dir) {
    int64_t hlen = rawc_strlen(s), nlen = rawc_strlen(sub);
    const uint32_t *h = rawc_str_data(s), *n = rawc_str_data(sub);
    if (start < 0) { start += hlen; if (start < 0) start = 0; }
    if (start >= hlen) return -1;
    if (nlen == 1) {
        uint32_t needle = n[0];
        for (int64_t i = start; i < hlen; i++)
            if (h[i] == needle) return i;
        return -1;
    }
    for (int64_t i = start; i + nlen <= hlen; i++)
        if (memcmp(h + i, n, nlen * sizeof(uint32_t)) == 0) return i;
    return -1;
}

static inline int64_t CPyStr_FindWithEnd(intptr_t s, intptr_t sub, int64_t start, int64_t end, int64_t _dir) {
    int64_t hlen = rawc_strlen(s), nlen = rawc_strlen(sub);
    const uint32_t *h = rawc_str_data(s), *n = rawc_str_data(sub);
    if (start < 0) start = 0;
    if (end > hlen) end = hlen;
    for (int64_t i = start; i + nlen <= end; i++)
        if (memcmp(h + i, n, nlen * sizeof(uint32_t)) == 0) return i;
    return -1;
}

static inline int64_t CPyStr_CountFull(intptr_t s, intptr_t sub, int64_t start, int64_t end) {
    int64_t hlen = rawc_strlen(s), nlen = rawc_strlen(sub);
    const uint32_t *h = rawc_str_data(s), *n = rawc_str_data(sub);
    if (start < 0) start = 0;
    if (end > hlen) end = hlen;
    int64_t count = 0;
    for (int64_t i = start; i + nlen <= end; i++)
        if (memcmp(h + i, n, nlen * sizeof(uint32_t)) == 0) { count++; i += nlen - 1; }
    return count; /* rawc: ints are not tagged */
}

static inline intptr_t CPyStr_Strip(intptr_t s, int _mode) {
    int64_t len = rawc_strlen(s), start = 0, end = len;
    const uint32_t *data = rawc_str_data(s);
    while (start < end && rawc_unicode_isspace(data[start])) start++;
    while (end > start && rawc_unicode_isspace(data[end-1])) end--;
    if (start == 0 && end == len) return s;
    return rawc_str_new_ucs4(data + start, end - start);
}

static inline intptr_t CPyStr_GetItemUnsafe(intptr_t s, int64_t i) {
    int64_t len = rawc_str_len(s);
    if (i < 0 || i >= len) {
        rawc_error_msg = rawc_str_new("string index out of range", 25);
        rawc_error_flag = 1;
        longjmp(rawc_error_jmp, 1);
        return RAWC_EMPTY_STR;
    }
    uint32_t ch = rawc_str_data(s)[i];
    if (ch < 256) return (intptr_t)rawc_char_strings[ch];
    return rawc_str_new_ucs4(&ch, 1);
}

static inline intptr_t CPyStr_Build(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        intptr_t part = va_arg(ap, intptr_t);
        if (part) total += rawc_strlen(part);
    }
    va_end(ap);
    intptr_t result = rawc_str_new_ucs4(NULL, total);
    uint32_t *dst = (uint32_t *)result;
    int64_t pos = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        intptr_t part = va_arg(ap, intptr_t);
        if (part) {
            int64_t l = rawc_strlen(part);
            memcpy(dst + pos, (const void *)part, l * sizeof(uint32_t));
            pos += l;
        }
    }
    va_end(ap);
    dst[pos] = 0;
    return result;
}

/* ---- List ---- */

typedef struct Rawc_List {
    intptr_t *data;
    int64_t len;
    int64_t cap;
} Rawc_List;

static inline intptr_t PyList_New(int64_t size) {
    Rawc_List *l = (Rawc_List *)rawc_alloc(sizeof(Rawc_List));
    l->len = size;
    l->cap = size > 0 ? size : 8;
    l->data = (intptr_t *)rawc_alloc(sizeof(intptr_t) * l->cap);
    memset(l->data, 0, sizeof(intptr_t) * l->cap);
    return (intptr_t)l;
}

static inline int32_t PyList_Append(intptr_t list, intptr_t item) {
    if (!list) return -1;
    Rawc_List *l = (Rawc_List *)list;
    if (l->len >= l->cap) {
        int64_t nc = l->cap * 2;
        intptr_t *ndata = (intptr_t *)rawc_alloc(sizeof(intptr_t) * nc);
        if (l->data) memcpy(ndata, l->data, sizeof(intptr_t) * l->len);
        l->data = ndata; l->cap = nc;
    }
    l->data[l->len++] = item;
    return 0;
}

static inline intptr_t CPyList_GetSlice(intptr_t list, int64_t start, int64_t end) {
    Rawc_List *l = (Rawc_List *)list;
    if (end > l->len) end = l->len;
    if (start >= end) return PyList_New(0);
    int64_t n = end - start;
    intptr_t nl = PyList_New(n);
    memcpy(((Rawc_List *)nl)->data, l->data + start, sizeof(intptr_t) * n);
    return nl;
}

static inline intptr_t CPyList_Extend(intptr_t list, intptr_t other) {
    Rawc_List *o = (Rawc_List *)other;
    for (int64_t i = 0; i < o->len; i++) PyList_Append(list, o->data[i]);
    return 0;
}

static inline intptr_t CPyList_SetItemUnsafe(intptr_t list, int64_t i, intptr_t val) {
    ((Rawc_List *)list)->data[i] = val;
    return 0;
}

static inline intptr_t CPyList_GetItemInt64Borrow(intptr_t list, int64_t index) {
    Rawc_List *l = (Rawc_List *)list;
    if (index < 0) index += l->len;
    return (index >= 0 && index < l->len) ? l->data[index] : 0;
}

static inline intptr_t CPySequenceTuple_GetItemUnsafe(intptr_t tuple, int64_t i) {
    return ((Rawc_List *)tuple)->data[i];
}

/* ---- Set (dict-based, O(1) contains) ---- */

static inline intptr_t PySet_New(intptr_t _iterable) {
    return (intptr_t)Rawc_Dict_New();
}

static inline int32_t PySet_Add(intptr_t set, intptr_t item) {
    if (!set) return -1;
    if (item == 0) { Rawc_Dict_SetInt0((Rawc_Dict *)set, 1); return 0; }
    Rawc_Dict_Set((Rawc_Dict *)set, item, 1);
    return 0;
}

static inline int32_t PySet_Contains(intptr_t set, intptr_t item) {
    if (!set) return 0;
    if (item == 0) return Rawc_Dict_ContainsInt0((Rawc_Dict *)set);
    return Rawc_Dict_Contains((Rawc_Dict *)set, item);
}

static inline intptr_t PyNumber_Or(intptr_t a, intptr_t b) {
    Rawc_Dict *da = (Rawc_Dict *)a, *db = (Rawc_Dict *)b;
    Rawc_Dict *r = Rawc_Dict_New();
    if (da) {
        if (da->int0_set) Rawc_Dict_SetInt0(r, da->int0_value);
        for (int32_t i = 0; i < da->capacity; i++)
            if (da->entries[i].key) Rawc_Dict_Set(r, da->entries[i].key, da->entries[i].value);
    }
    if (db) {
        if (db->int0_set) Rawc_Dict_SetInt0(r, db->int0_value);
        for (int32_t i = 0; i < db->capacity; i++)
            if (db->entries[i].key) Rawc_Dict_Set(r, db->entries[i].key, db->entries[i].value);
    }
    return (intptr_t)r;
}

static inline int64_t PyDict_Size(intptr_t dict) {
    if (!dict) return 0;
    Rawc_Dict *d = (Rawc_Dict *)dict;
    return (int64_t)(d->len + d->int0_set);
}

/* ---- Dict get with default ---- */

static inline intptr_t CPyDict_Get_Default(intptr_t dict, intptr_t key, intptr_t def) {
    if (!dict) return def;
    Rawc_Dict *d = (Rawc_Dict *)dict;
    intptr_t val = (key == 0) ? Rawc_Dict_GetInt0(d) : Rawc_Dict_Get(d, key);
    return val ? val : def;
}
#define CPyDict_Get(d, k, def) CPyDict_Get_Default(d, k, def)

static inline intptr_t CPyDict_GetWithNone(intptr_t dict, intptr_t key) {
    return CPyDict_Get_Default(dict, key, 0);
}

/* ---- Object attribute lookup ---- */

static inline intptr_t CPyObject_GetAttr(intptr_t obj, intptr_t name) {
    if (obj) return Rawc_Dict_Get((Rawc_Dict *)obj, name);
    return 0;
}

/* ---- Object allocation ---- */

static inline intptr_t Rawc_AllocInstance(int nslots) {
    intptr_t *obj = (intptr_t *)rawc_alloc(sizeof(intptr_t) * nslots);
    memset(obj, 0, sizeof(intptr_t) * nslots);
    return (intptr_t)obj;
}

/* ---- Exception stubs ---- */

#ifndef MYPYC_DECLARED_tuple_T3OOO
#define MYPYC_DECLARED_tuple_T3OOO
typedef struct tuple_T3OOO { intptr_t f0; intptr_t f1; intptr_t f2; } tuple_T3OOO;
#endif

/* Exception handling uses setjmp/longjmp defined in rawc_rt.h */

static inline tuple_T3OOO CPy_CatchError(void) { return (tuple_T3OOO){0, 0, 0}; }
static inline char CPy_ExceptionMatches(intptr_t _t) { return 1; /* always match in rawc */ }
static inline intptr_t CPy_GetExcValue(void) { return 0; }
static inline intptr_t CPy_Raise(intptr_t _e) {
    rawc_error_flag = 1;
    longjmp(rawc_error_jmp, 1);
    return 0; /* unreachable */
}
static inline intptr_t CPy_Reraise(void) { return 0; }
static inline intptr_t CPy_RestoreExcInfo(tuple_T3OOO _i) { return 0; }
static inline intptr_t CPy_KeepPropagating(void) { return 0; }
static inline char CPy_NoErrOccurred(void) { return 1; }

/* ---- Misc stubs ---- */

static inline intptr_t PyMethod_New(intptr_t func, intptr_t self) { return func; }
static inline intptr_t PyObject_Vectorcall(intptr_t func, intptr_t *args, int64_t n, intptr_t kw) {
    /* In rawc mode, the only use of PyObject_Vectorcall is to construct exception
       objects: ExcType(message). Save the message for the bridge to use. */
    int64_t nargs = n & 0x7FFFFFFF;  /* mask off PY_VECTORCALL_ARGUMENTS_OFFSET */
    if (nargs >= 1 && args) {
        rawc_error_msg = args[0];  /* save the message string */
    }
    return 0;
}
static inline intptr_t PyObject_VectorcallMethod(intptr_t name, intptr_t *args, uint64_t n, intptr_t kw) { return 0; }
static inline intptr_t PyObject_Str(intptr_t obj) { return rawc_str_new("<object>", 8); }
static inline intptr_t PyUnicode_Join(intptr_t sep, intptr_t list) {
    if (!list) return RAWC_EMPTY_STR;
    Rawc_List *l = (Rawc_List *)list;
    if (l->len == 0) return RAWC_EMPTY_STR;
    int64_t sep_len = rawc_strlen(sep);
    int64_t total = 0;
    for (int64_t i = 0; i < l->len; i++) {
        total += rawc_strlen(l->data[i]);
        if (i > 0) total += sep_len;
    }
    intptr_t result = rawc_str_new_ucs4(NULL, total);
    uint32_t *dst = (uint32_t *)result;
    int64_t pos = 0;
    for (int64_t i = 0; i < l->len; i++) {
        if (i > 0 && sep_len) { memcpy(dst + pos, rawc_str_data(sep), sep_len * sizeof(uint32_t)); pos += sep_len; }
        int64_t sl = rawc_strlen(l->data[i]);
        if (sl) { memcpy(dst + pos, rawc_str_data(l->data[i]), sl * sizeof(uint32_t)); pos += sl; }
    }
    dst[pos] = 0;
    return result;
}
static inline intptr_t PyUnicode_Replace(intptr_t s, intptr_t old, intptr_t new_, int64_t n) {
    int64_t slen = rawc_strlen(s), olen = rawc_strlen(old), nlen = rawc_strlen(new_);
    if (!slen || !olen) return s;
    const uint32_t *sd = rawc_str_data(s), *od = rawc_str_data(old), *nwd = rawc_str_data(new_);
    /* Count replacements */
    int64_t count = 0;
    for (int64_t i = 0; i + olen <= slen; i++)
        if (memcmp(sd + i, od, olen * sizeof(uint32_t)) == 0) { count++; i += olen - 1; }
    if (!count) return s;
    if (n >= 0 && count > n) count = n;
    int64_t rlen = slen + count * (nlen - olen);
    intptr_t result = rawc_str_new_ucs4(NULL, rlen);
    uint32_t *dst = (uint32_t *)result;
    int64_t di = 0, replacements = 0;
    for (int64_t i = 0; i < slen; ) {
        if (i + olen <= slen && memcmp(sd + i, od, olen * sizeof(uint32_t)) == 0 && (n < 0 || replacements < count)) {
            memcpy(dst + di, nwd, nlen * sizeof(uint32_t));
            di += nlen; i += olen; replacements++;
        } else { dst[di++] = sd[i++]; }
    }
    dst[di] = 0;
    return result;
}
static inline intptr_t CPyTagged_Str(int64_t val) {
    char buf[32];
    /* In rawc mode, ints are int64_t (not tagged), so no shift needed */
    int len = snprintf(buf, 32, "%lld", (long long)val);
    return rawc_str_new(buf, len);
}
static inline intptr_t CPyLong_FromStrWithBase(intptr_t s, int64_t base) {
    /* Convert string to integer with given base.
       In rawc, ints are int64_t (not tagged). Returns value or sets rawc_error_flag. */
    int64_t len = rawc_strlen(s);
    if (!len) { rawc_error_flag = 1; return -113; }
    const uint32_t *data = rawc_str_data(s);
    int64_t result = 0;
    int64_t b = base;
    for (int64_t i = 0; i < len; i++) {
        uint32_t ch = data[i];
        int digit;
        if (ch == '_') continue; /* skip underscores */
        else if (i <= 1 && (ch == 'x' || ch == 'X' || ch == 'b' || ch == 'B' || ch == 'o' || ch == 'O')) continue; /* skip prefix at start */
        else if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else { rawc_error_flag = 1; return -113; /* error sentinel */ }
        if (digit >= b) { rawc_error_flag = 1; return -113; /* error sentinel */ }
        result = result * b + digit;
    }
    return result; /* rawc: ints are not tagged */
}
static inline int64_t CPyLong_AsInt64(intptr_t val) { return (int64_t)val; }

#endif /* RAWC_COMPAT_H */
