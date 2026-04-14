#ifndef MYPYC_UTIL_H
#define MYPYC_UTIL_H

#include <Python.h>
#include <frameobject.h>
#include <assert.h>

#if defined(__clang__) || defined(__GNUC__)
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#define CPy_Unreachable() __builtin_unreachable()
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#define CPy_Unreachable() abort()
#endif

#if defined(__clang__) || defined(__GNUC__)
#define CPy_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define CPy_NOINLINE __declspec(noinline)
#else
#define CPy_NOINLINE
#endif

#ifndef Py_GIL_DISABLED

// Everything is running in the same thread, so no need for thread locals
#define CPyThreadLocal

#else

// 1. Use C11 standard thread_local storage, if available
#if defined(__STDC_VERSION__)  && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define CPyThreadLocal _Thread_local

// 2. Microsoft Visual Studio fallback
#elif defined(_MSC_VER)
#define CPyThreadLocal __declspec(thread)

// 3. GNU thread local storage for GCC/Clang targets that still need it
#elif defined(__GNUC__) || defined(__clang__)
#define CPyThreadLocal __thread

#else
#error "Can't define CPyThreadLocal for this compiler/target (consider using a non-free-threaded Python build)"
#endif

#endif // Py_GIL_DISABLED

// Helper macro for stringification in _Pragma
#define CPY_STRINGIFY(x) #x

#if defined(__clang__)
    #define CPY_UNROLL_LOOP_IMPL(x) _Pragma(CPY_STRINGIFY(x))
    #define CPY_UNROLL_LOOP(n) CPY_UNROLL_LOOP_IMPL(unroll n)
#elif defined(__GNUC__) && __GNUC__ >= 8
    #define CPY_UNROLL_LOOP_IMPL(x) _Pragma(CPY_STRINGIFY(x))
    #define CPY_UNROLL_LOOP(n) CPY_UNROLL_LOOP_IMPL(GCC unroll n)
#else
    #define CPY_UNROLL_LOOP(n)
#endif

// INCREF and DECREF that assert the pointer is not NULL.
// asserts are disabled in release builds so there shouldn't be a perf hit.
// I'm honestly kind of surprised that this isn't done by default.
#define CPy_INCREF(p) do { assert(p); Py_INCREF(p); } while (0)
#define CPy_DECREF(p) do { assert(p); Py_DECREF(p); } while (0)
// Here just for consistency
#define CPy_XDECREF(p) Py_XDECREF(p)

#ifndef Py_GIL_DISABLED

// The *_NO_IMM operations below perform refcount manipulation for
// non-immortal objects (Python 3.12 and later).
//
// Py_INCREF and other CPython operations check for immortality. This
// can be expensive when we know that an object cannot be immortal.
//
// This optimization cannot be performed in free-threaded mode so we
// fall back to just calling the normal incref/decref operations.

static inline void CPy_INCREF_NO_IMM(PyObject *op)
{
    op->ob_refcnt++;
}

static inline void CPy_DECREF_NO_IMM(PyObject *op)
{
    if (--op->ob_refcnt == 0) {
        _Py_Dealloc(op);
    }
}

static inline void CPy_XDECREF_NO_IMM(PyObject *op)
{
    if (op != NULL && --op->ob_refcnt == 0) {
        _Py_Dealloc(op);
    }
}

#define CPy_INCREF_NO_IMM(op) CPy_INCREF_NO_IMM((PyObject *)(op))
#define CPy_DECREF_NO_IMM(op) CPy_DECREF_NO_IMM((PyObject *)(op))
#define CPy_XDECREF_NO_IMM(op) CPy_XDECREF_NO_IMM((PyObject *)(op))

#else

#define CPy_INCREF_NO_IMM(op) CPy_INCREF(op)
#define CPy_DECREF_NO_IMM(op) CPy_DECREF(op)
#define CPy_XDECREF_NO_IMM(op) CPy_XDECREF(op)

#endif

// Tagged integer -- our representation of Python 'int' objects.
// Small enough integers are represented as unboxed integers (shifted
// left by 1); larger integers (larger than 63 bits on a 64-bit
// platform) are stored as a tagged pointer (PyObject *)
// representing a Python int object, with the lowest bit set.
// Tagged integers are always normalized. A small integer *must not*
// have the tag bit set.
typedef size_t CPyTagged;

typedef size_t CPyPtr;

#define CPY_INT_BITS (CHAR_BIT * sizeof(CPyTagged))

#define CPY_TAGGED_MAX (((Py_ssize_t)1 << (CPY_INT_BITS - 2)) - 1)
#define CPY_TAGGED_MIN (-((Py_ssize_t)1 << (CPY_INT_BITS - 2)))
#define CPY_TAGGED_ABS_MIN (0-(size_t)CPY_TAGGED_MIN)

typedef PyObject CPyModule;

// Tag bit used for long integers
#define CPY_INT_TAG 1

// Error value for signed fixed-width (low-level) integers
#define CPY_LL_INT_ERROR -113

// Error value for unsigned fixed-width (low-level) integers
#define CPY_LL_UINT_ERROR 239

// Error value for floats
#define CPY_FLOAT_ERROR -113.0

// Value for 'None' primitive type
#define CPY_NONE_ERROR 2
#define CPY_NONE 1

typedef void (*CPyVTableItem)(void);

static inline CPyTagged CPyTagged_ShortFromInt(int x) {
    return x << 1;
}

static inline CPyTagged CPyTagged_ShortFromSsize_t(Py_ssize_t x) {
    return x << 1;
}

// Are we targeting Python 3.X or newer?
#define CPY_3_11_FEATURES (PY_VERSION_HEX >= 0x030b0000)
#define CPY_3_12_FEATURES (PY_VERSION_HEX >= 0x030c0000)

#if CPY_3_12_FEATURES

// Same as macros in CPython internal/pycore_long.h, but with a CPY_ prefix
#define CPY_NON_SIZE_BITS 3
#define CPY_SIGN_ZERO 1
#define CPY_SIGN_NEGATIVE 2
#define CPY_SIGN_MASK 3

#define CPY_LONG_DIGIT(o, n) ((o)->long_value.ob_digit[n])

// Only available on Python 3.12 and later
#define CPY_LONG_TAG(o) ((o)->long_value.lv_tag)
#define CPY_LONG_IS_NEGATIVE(o) (((o)->long_value.lv_tag & CPY_SIGN_MASK) == CPY_SIGN_NEGATIVE)
// Only available on Python 3.12 and later
#define CPY_LONG_SIZE(o) ((o)->long_value.lv_tag >> CPY_NON_SIZE_BITS)
// Number of digits; negative for negative ints
#define CPY_LONG_SIZE_SIGNED(o) (CPY_LONG_IS_NEGATIVE(o) ? -CPY_LONG_SIZE(o) : CPY_LONG_SIZE(o))
// Number of digits, assuming int is non-negative
#define CPY_LONG_SIZE_UNSIGNED(o) CPY_LONG_SIZE(o)

#else

#define CPY_LONG_DIGIT(o, n) ((o)->ob_digit[n])
#define CPY_LONG_IS_NEGATIVE(o) (((o)->ob_base.ob_size < 0)
#define CPY_LONG_SIZE_SIGNED(o) ((o)->ob_base.ob_size)
#define CPY_LONG_SIZE_UNSIGNED(o) ((o)->ob_base.ob_size)

#endif

// Are we targeting Python 3.13 or newer?
#define CPY_3_13_FEATURES (PY_VERSION_HEX >= 0x030d0000)

// Are we targeting Python 3.14 or newer?
#define CPY_3_14_FEATURES (PY_VERSION_HEX >= 0x030e0000)

// ---------------------------------------------------------------------
// Arena (@mypyc_attr(arena=True))
//
// All PyObject_Malloc / PyMem_Malloc calls inside an @arena function body
// route to a thread-local bump allocator installed at module init. Bump
// pointers live only for the arena scope — @arena functions must return
// None (see _CPy_arena_exit_obj below).
//
// Bodies bracket themselves with _CPy_arena_enter / _exit. At outermost
// enter we swap gen-0 to a private empty list, snapshot+block the
// freelists, and disable auto-gc. At exit we restore everything, reset
// chunks to the pool, and re-enable gc. Full rationale in arena.c.
// ---------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif
extern CPyThreadLocal int _CPy_arena_active;
extern CPyThreadLocal int _CPy_arena_saved_gc_enabled;
void _CPy_arena_install(void);
void _CPy_arena_reset(void);
void _CPy_arena_swap_in(void);
void _CPy_arena_swap_out(void);
void _CPy_arena_freelists_save_and_block(void);
void _CPy_arena_freelists_restore(void);
PyObject *_CPy_arena_stamp_alloc(PyTypeObject *tp, Py_ssize_t nitems);
#ifdef __cplusplus
}
#endif

static inline void _CPy_arena_enter(void) {
    if (_CPy_arena_active == 0) {
        _CPy_arena_swap_in();
        _CPy_arena_freelists_save_and_block();
    }
    _CPy_arena_active++;
}

static inline void _CPy_arena_exit(void) {
    if (--_CPy_arena_active == 0) {
        _CPy_arena_freelists_restore();
        _CPy_arena_swap_out();
        _CPy_arena_reset();
        if (_CPy_arena_saved_gc_enabled) PyGC_Enable();
    }
}

// Stub. @arena functions returning a refcounted value are unsupported —
// the pointer points into soon-to-be-reset bump memory. Codegen still
// emits this for typed returns (see mypyc/codegen/emitfunc.py); we do
// the normal teardown and return retval verbatim. Caller access is UB.
static inline PyObject *_CPy_arena_exit_obj(PyObject *retval) {
    _CPy_arena_exit();
    return retval;
}

// Stamp an allocation's refcount immortal when the arena is active, so
// Py_DECREF never reaches tp_dealloc and the object never hits our free
// hook. Three variants avoid a PyObject_IS_GC call on the hot path:
//   _gc     — type is always GC-tracked (list/dict/tuple/set)
//   _nongc  — type is never GC-tracked (int/float/str/bytes)
//   _any    — mixed (generic tp_alloc); checks at runtime
// mypyc classes get the same treatment via `_CPy_arena_stamp_alloc`
// installed on tp_alloc; the macros below cover libpython allocators
// called from mypyc-compiled code.
static inline PyObject *_CPy_arena_stamp_gc(PyObject *op) {
    if (op != NULL && _CPy_arena_active) {
        Py_SET_REFCNT(op, _Py_IMMORTAL_INITIAL_REFCNT);
        PyObject_GC_UnTrack(op);
    }
    return op;
}
static inline PyObject *_CPy_arena_stamp_nongc(PyObject *op) {
    if (op != NULL && _CPy_arena_active) {
        Py_SET_REFCNT(op, _Py_IMMORTAL_INITIAL_REFCNT);
    }
    return op;
}
static inline PyObject *_CPy_arena_stamp_any(PyObject *op) {
    if (op != NULL && _CPy_arena_active) {
        Py_SET_REFCNT(op, _Py_IMMORTAL_INITIAL_REFCNT);
        if (PyObject_IS_GC(op)) PyObject_GC_UnTrack(op);
    }
    return op;
}

// Wrap libpython's common allocators. C preprocessor's blue-paint rule
// means the macro doesn't re-expand inside its own body, so the inner
// call resolves to the real function.
#define PyList_New(n)                       _CPy_arena_stamp_gc(PyList_New(n))
#define PyDict_New()                        _CPy_arena_stamp_gc(PyDict_New())
#define PyTuple_New(n)                      _CPy_arena_stamp_gc(PyTuple_New(n))
#define PySet_New(it)                       _CPy_arena_stamp_gc(PySet_New(it))
#define PyFrozenSet_New(it)                 _CPy_arena_stamp_gc(PyFrozenSet_New(it))

#define PyLong_FromLong(v)                  _CPy_arena_stamp_nongc(PyLong_FromLong(v))
#define PyLong_FromLongLong(v)              _CPy_arena_stamp_nongc(PyLong_FromLongLong(v))
#define PyLong_FromSsize_t(v)               _CPy_arena_stamp_nongc(PyLong_FromSsize_t(v))
#define PyLong_FromUnsignedLong(v)          _CPy_arena_stamp_nongc(PyLong_FromUnsignedLong(v))
#define PyLong_FromUnsignedLongLong(v)      _CPy_arena_stamp_nongc(PyLong_FromUnsignedLongLong(v))
#define PyLong_FromDouble(v)                _CPy_arena_stamp_nongc(PyLong_FromDouble(v))
#define PyFloat_FromDouble(d)               _CPy_arena_stamp_nongc(PyFloat_FromDouble(d))
#define PyBytes_FromString(s)               _CPy_arena_stamp_nongc(PyBytes_FromString(s))
#define PyBytes_FromStringAndSize(s, n)     _CPy_arena_stamp_nongc(PyBytes_FromStringAndSize(s, n))
#define PyUnicode_FromString(s)             _CPy_arena_stamp_nongc(PyUnicode_FromString(s))
#define PyUnicode_FromStringAndSize(s, n)   _CPy_arena_stamp_nongc(PyUnicode_FromStringAndSize(s, n))
#define PyUnicode_FromFormat(...)           _CPy_arena_stamp_nongc(PyUnicode_FromFormat(__VA_ARGS__))
#define PyUnicode_Concat(a, b)              _CPy_arena_stamp_nongc(PyUnicode_Concat(a, b))
#define PyUnicode_Substring(s, i, j)        _CPy_arena_stamp_nongc(PyUnicode_Substring(s, i, j))
#define PyUnicode_Join(sep, seq)            _CPy_arena_stamp_nongc(PyUnicode_Join(sep, seq))
#define PyUnicode_FromOrdinal(c)            _CPy_arena_stamp_nongc(PyUnicode_FromOrdinal(c))
#define PyUnicode_New(size, maxchar)        _CPy_arena_stamp_nongc(PyUnicode_New(size, maxchar))

// Generic: the result may or may not be GC-tracked depending on `t`.
#define PyType_GenericAlloc(t, n)           _CPy_arena_stamp_any(PyType_GenericAlloc(t, n))

#endif
