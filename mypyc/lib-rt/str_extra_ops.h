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

// Buffer-access variant: read kind + data once outside a loop, then use these
// per-iteration helpers to avoid re-reading string metadata on every char access.
// Safe because Python strings are immutable.

static inline int32_t CPyStr_LoadKind(PyObject *obj) {
    return (int32_t)PyUnicode_KIND(obj);
}

static inline CPyPtr CPyStr_LoadData(PyObject *obj) {
    return (CPyPtr)PyUnicode_DATA(obj);
}

static inline CPyTagged CPyStr_GetItemFromBuffer(int32_t kind, CPyPtr data, int64_t index) {
    return PyUnicode_READ(kind, (void *)data, index) << 1;
}

// Bounds-checked read of a codepoint as a raw int32. Mirrors the error
// semantics of CPyStr_GetItem (sets IndexError and returns -113, mypyc's
// int32 error sentinel, on out-of-range indices). Used by the
// char_str_index_fold pass to avoid allocating a 1-char PyObject when the
// result is immediately unboxed to char.
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

static inline int32_t CPyStr_GetCharAtUnsafe(PyObject *s, CPyTagged index_tagged) {
    Py_ssize_t i = CPyTagged_ShortAsSsize_t(index_tagged);
    return (int32_t)PyUnicode_READ(PyUnicode_KIND(s), PyUnicode_DATA(s), i);
}

// char-codepoint classification. Each helper takes a raw int32 codepoint
// (0..0x10FFFF, or negative for the empty sentinel or invalid input, in
// which case we return false). ASCII is dispatched through the ctype
// bitmap (Py_ISDIGIT etc.) — a cheap table lookup — while non-ASCII falls
// through to the Py_UNICODE_IS* path. Reached via the `char` type's
// method_op primitives for ``.isspace()`` / ``.isdigit()`` / etc.

static inline bool CPyChar_IsSpace(int32_t c) {
    // Py_UNICODE_ISSPACE already has its own ASCII fast path.
    return c >= 0 && Py_UNICODE_ISSPACE((Py_UCS4)c);
}

static inline bool CPyChar_IsDigit(int32_t c) {
    if (c < 0) return false;
    if (c < 128) return Py_ISDIGIT((unsigned char)c);
    return Py_UNICODE_ISDIGIT((Py_UCS4)c);
}

static inline bool CPyChar_IsAlnum(int32_t c) {
    if (c < 0) return false;
    if (c < 128) return Py_ISALNUM((unsigned char)c);
    return Py_UNICODE_ISALNUM((Py_UCS4)c);
}

static inline bool CPyChar_IsAlpha(int32_t c) {
    if (c < 0) return false;
    if (c < 128) return Py_ISALPHA((unsigned char)c);
    return Py_UNICODE_ISALPHA((Py_UCS4)c);
}

// For a 1-char string, ``.isidentifier()`` is true only if the char is a
// valid identifier START character (XID_Start) or underscore — notably,
// digits return false (``"0".isidentifier() == False``). Approximated here
// with ISALPHA | '_'; this is exact for ASCII and covers the vast majority
// of Unicode identifier starts.
static inline bool CPyChar_IsIdentifier(int32_t c) {
    if (c < 0) return false;
    if (c < 128) return Py_ISALPHA((unsigned char)c) || c == (int32_t)'_';
    return Py_UNICODE_ISALPHA((Py_UCS4)c);
}

// ASCII-only case conversion. Matches the semantics of the common
// ``{chr(i): chr(i).upper() for i in range(97, 123)}`` idiom — non-ASCII
// chars (including locale-sensitive ones like German ß) pass through
// unchanged. Returns int32 codepoint directly, no allocation. Callers
// that need full Unicode casing should fall back to ``str(ch).upper()``.
static inline int32_t CPyChar_Upper(int32_t c) {
    if (c >= (int32_t)'a' && c <= (int32_t)'z') return c - 32;
    return c;
}

#endif
