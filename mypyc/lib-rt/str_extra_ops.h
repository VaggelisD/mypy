#ifndef MYPYC_STR_EXTRA_OPS_H
#define MYPYC_STR_EXTRA_OPS_H

#include <Python.h>
#include <stdint.h>
#include "CPy.h"

// Optimized str indexing for ord(s[i])

// If index is negative, convert to non-negative index (no range checking)
static inline int64_t CPyStr_AdjustIndex(PyObject *obj, int64_t index) {
    if (index < 0) {
        return index + PyUnicode_GET_LENGTH(obj);
    }
    return index;
}

// Check if index is in valid range [0, len)
static inline bool CPyStr_RangeCheck(PyObject *obj, int64_t index) {
    return index >= 0 && index < PyUnicode_GET_LENGTH(obj);
}

// Get character at index as int (ord value) - no bounds checking, returns as CPyTagged
static inline CPyTagged CPyStr_GetItemUnsafeAsInt(PyObject *obj, int64_t index) {
    int kind = PyUnicode_KIND(obj);
    return PyUnicode_READ(kind, PyUnicode_DATA(obj), index) << 1;
}

// Bounds-checked codepoint read returning int32. Error sentinel -113 on
// out-of-range / non-short index. Used by char_str_index_fold to avoid the
// 1-char PyObject alloc when the result is immediately unboxed to char.
static inline int32_t CPyStr_GetCharAt(PyObject *s, CPyTagged index_tagged) {
    Py_ssize_t i;
    if (likely(CPyTagged_CheckShort(index_tagged))) {
        i = CPyTagged_ShortAsSsize_t(index_tagged);
    } else {
        PyObject *c = CPyStr_GetItem(s, index_tagged);
        if (c == NULL) return -113;
        int32_t cp = (int32_t)PyUnicode_READ_CHAR(c, 0);
        Py_DECREF(c);
        return cp;
    }
    Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    if (i < 0) i += n;
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -113;
    }
    return (int32_t)PyUnicode_READ(PyUnicode_KIND(s), PyUnicode_DATA(s), i);
}

// Unsafe variant: no bounds check, takes a raw Py_ssize_t index (matching
// CPyStr_GetItemUnsafe's signature). Used by char_str_index_fold when the
// caller — typically a str iteration — already guarantees the bounds.
static inline int32_t CPyStr_GetCharAtUnsafe(PyObject *s, Py_ssize_t i) {
    return (int32_t)PyUnicode_READ(PyUnicode_KIND(s), PyUnicode_DATA(s), i);
}

// char-codepoint classification. Each helper takes a raw int32 codepoint;
// negative values (empty sentinel, invalid input) return false. The
// Py_UNICODE_IS* macros have their own ASCII fast paths.

static inline bool CPyChar_IsSpace(int32_t c) {
    return c >= 0 && Py_UNICODE_ISSPACE((Py_UCS4)c);
}

static inline bool CPyChar_IsDigit(int32_t c) {
    return c >= 0 && Py_UNICODE_ISDIGIT((Py_UCS4)c);
}

static inline bool CPyChar_IsAlnum(int32_t c) {
    return c >= 0 && Py_UNICODE_ISALNUM((Py_UCS4)c);
}

static inline bool CPyChar_IsAlpha(int32_t c) {
    return c >= 0 && Py_UNICODE_ISALPHA((Py_UCS4)c);
}

// .isidentifier(): ASCII check matches XID_Start exactly; non-ASCII delegates
// to CPython for correct XID_Start handling.
static inline bool CPyChar_IsIdentifier(int32_t c) {
    if (c < 0) return false;
    if (c < 128) return Py_ISALPHA((unsigned char)c) || c == (int32_t)'_';
    PyObject *s = PyUnicode_FromOrdinal((int)c);
    if (s == NULL) { PyErr_Clear(); return false; }
    int r = PyUnicode_IsIdentifier(s);
    Py_DECREF(s);
    return r == 1;
}

// Latin-1 case-conversion tables for codepoints 128..255. Generated from
// Python's str.upper() / str.lower(). Codepoints whose conversion produces
// multiple codepoints (only U+00DF 'ß' for upper, none for lower) are
// mapped to themselves — the caller's multi-char fallback would do the
// same, so we preserve the single-codepoint contract. Non-letters are
// mapped to themselves (identity). Used by CPyChar_Upper/Lower to avoid
// a PyObject allocation + PyObject_CallMethod round-trip for the common
// Latin-1 supplement range.
static const int32_t CPY_LATIN1_UPPER[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009a, 0x009b, 0x009c, 0x009d, 0x009e, 0x009f,
    0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
    0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af,
    0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x00b4, 0x039c, 0x00b6, 0x00b7,  // 0xB5 µ -> Μ
    0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf,
    0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7,
    0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd, 0x00ce, 0x00cf,
    0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00d7,
    0x00d8, 0x00d9, 0x00da, 0x00db, 0x00dc, 0x00dd, 0x00de, 0x00df,  // 0xDF ß -> ß (multi-char)
    0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7,
    0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd, 0x00ce, 0x00cf,
    0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00f7,
    0x00d8, 0x00d9, 0x00da, 0x00db, 0x00dc, 0x00dd, 0x00de, 0x0178,  // 0xFF ÿ -> Ÿ
};

static const int32_t CPY_LATIN1_LOWER[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009a, 0x009b, 0x009c, 0x009d, 0x009e, 0x009f,
    0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
    0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af,
    0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x00b4, 0x00b5, 0x00b6, 0x00b7,
    0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf,
    0x00e0, 0x00e1, 0x00e2, 0x00e3, 0x00e4, 0x00e5, 0x00e6, 0x00e7,
    0x00e8, 0x00e9, 0x00ea, 0x00eb, 0x00ec, 0x00ed, 0x00ee, 0x00ef,
    0x00f0, 0x00f1, 0x00f2, 0x00f3, 0x00f4, 0x00f5, 0x00f6, 0x00d7,
    0x00f8, 0x00f9, 0x00fa, 0x00fb, 0x00fc, 0x00fd, 0x00fe, 0x00df,
    0x00e0, 0x00e1, 0x00e2, 0x00e3, 0x00e4, 0x00e5, 0x00e6, 0x00e7,
    0x00e8, 0x00e9, 0x00ea, 0x00eb, 0x00ec, 0x00ed, 0x00ee, 0x00ef,
    0x00f0, 0x00f1, 0x00f2, 0x00f3, 0x00f4, 0x00f5, 0x00f6, 0x00f7,
    0x00f8, 0x00f9, 0x00fa, 0x00fb, 0x00fc, 0x00fd, 0x00fe, 0x00ff,
};

// Shared helper for non-ASCII case conversion. ASCII is handled by the
// caller. For Latin-1 (128..255) we do a direct table lookup. For higher
// codepoints we delegate to CPython. Returns -113 on OOM / CPython error,
// with the exception set.
static inline int32_t CPyChar_ChangeCase(int32_t c, const char *method, const int32_t *latin1_tbl) {
    if (c < 256) return latin1_tbl[c - 128];
    PyObject *s = PyUnicode_FromOrdinal((int)c);
    if (s == NULL) return -113;
    PyObject *u = PyObject_CallMethod(s, method, NULL);
    Py_DECREF(s);
    if (u == NULL) return -113;
    int32_t result = c;
    if (PyUnicode_GET_LENGTH(u) == 1) {
        result = (int32_t)PyUnicode_READ_CHAR(u, 0);
    }
    Py_DECREF(u);
    return result;
}

// .upper() / .lower(): ASCII fast path; Latin-1 uses a table; higher
// codepoints delegate to the str method. If the result is multi-codepoint
// (e.g. ß -> SS), the original codepoint is returned since char holds
// one codepoint. Propagates OOM / errors from CPython (-113 + exception).
static inline int32_t CPyChar_Upper(int32_t c) {
    if (c >= (int32_t)'a' && c <= (int32_t)'z') return c - 32;
    if (c < 128) return c;
    return CPyChar_ChangeCase(c, "upper", CPY_LATIN1_UPPER);
}

static inline int32_t CPyChar_Lower(int32_t c) {
    if (c >= (int32_t)'A' && c <= (int32_t)'Z') return c + 32;
    if (c < 128) return c;
    return CPyChar_ChangeCase(c, "lower", CPY_LATIN1_LOWER);
}

#endif
