/*
 * rawc_rt.h - Pure C runtime for mypyc raw C mode.
 *
 * No dependency on Python C API or external libraries.
 * Memory managed by a simple arena allocator — all allocations during
 * a function call are freed at once when the arena is reset.
 * Strings are represented as const char* (intptr_t in generated code).
 * Dicts are open-addressing hash tables with cached hashes.
 */

#ifndef MYPYC_RAWC_RT_H
#define MYPYC_RAWC_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

/* ---- Arena allocator ---- */
/*
 * Fast bump allocator. All allocations are O(1). Call rawc_arena_reset()
 * between function calls to free everything at once.
 * Falls back to malloc for oversized allocations.
 */

#define RAWC_ARENA_BLOCK_SIZE (1 << 20)  /* 1MB blocks */

typedef struct Rawc_ArenaBlock {
    struct Rawc_ArenaBlock *next;
    char *base;
    size_t used;
    size_t capacity;
} Rawc_ArenaBlock;

extern Rawc_ArenaBlock *rawc_arena_head;
extern Rawc_ArenaBlock *rawc_arena_current;

/* Watermark: saved after init, reset returns here instead of to zero */
extern Rawc_ArenaBlock *rawc_arena_mark_block;
extern size_t rawc_arena_mark_used;

#ifdef RAWC_RT_IMPL
Rawc_ArenaBlock *rawc_arena_head = NULL;
Rawc_ArenaBlock *rawc_arena_current = NULL;
Rawc_ArenaBlock *rawc_arena_mark_block = NULL;
size_t rawc_arena_mark_used = 0;
#endif

static Rawc_ArenaBlock *rawc_arena_new_block(size_t min_size) {
    size_t cap = min_size > RAWC_ARENA_BLOCK_SIZE ? min_size : RAWC_ARENA_BLOCK_SIZE;
    Rawc_ArenaBlock *b = (Rawc_ArenaBlock *)malloc(sizeof(Rawc_ArenaBlock) + cap);
    b->base = (char *)(b + 1);
    b->used = 0;
    b->capacity = cap;
    b->next = NULL;
    return b;
}

static inline void *rawc_arena_alloc(size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (!rawc_arena_current || rawc_arena_current->used + size > rawc_arena_current->capacity) {
        Rawc_ArenaBlock *b = rawc_arena_new_block(size);
        if (rawc_arena_current) rawc_arena_current->next = b;
        else rawc_arena_head = b;
        rawc_arena_current = b;
    }
    void *ptr = rawc_arena_current->base + rawc_arena_current->used;
    rawc_arena_current->used += size;
    return ptr;
}

/* Save current position as watermark — reset will return here, not to zero.
   Call after init to protect config allocations from being overwritten. */
static inline void rawc_arena_mark(void) {
    rawc_arena_mark_block = rawc_arena_current;
    rawc_arena_mark_used = rawc_arena_current ? rawc_arena_current->used : 0;
}

/* Reset arena to the watermark. Blocks before the mark keep their data.
   Blocks after the mark get their used counter reset for reuse. */
static inline void rawc_arena_reset(void) {
    if (rawc_arena_mark_block) {
        rawc_arena_mark_block->used = rawc_arena_mark_used;
        Rawc_ArenaBlock *b = rawc_arena_mark_block->next;
        while (b) { b->used = 0; b = b->next; }
        rawc_arena_current = rawc_arena_mark_block;
    } else {
        Rawc_ArenaBlock *b = rawc_arena_head;
        while (b) { b->used = 0; b = b->next; }
        rawc_arena_current = rawc_arena_head;
    }
}

/* Allocation interface */
#define rawc_alloc(size) rawc_arena_alloc(size)

static inline void Rawc_Init(void) {
    if (!rawc_arena_head)
        rawc_arena_head = rawc_arena_current = rawc_arena_new_block(RAWC_ARENA_BLOCK_SIZE);
}

/* ---- Error handling ---- */
/* setjmp/longjmp for exception unwinding from rawc back to the bridge. */
#ifdef RAWC_RT_IMPL
jmp_buf rawc_error_jmp;
int rawc_error_flag = 0;
intptr_t rawc_error_msg = 0;    /* UCS-4 error message string */
intptr_t rawc_error_type = 0;   /* UCS-4 exception type name (e.g. "TokenError") */
#else
extern jmp_buf rawc_error_jmp;
extern int rawc_error_flag;
extern intptr_t rawc_error_msg;
extern intptr_t rawc_error_type;
#endif

/* ---- Dict: open-addressing hash table ---- */

typedef struct {
    intptr_t key;    /* 0 = empty slot */
    intptr_t value;
    uint32_t hash;
} Rawc_DictEntry;

/*
 * Layout: first 8 bytes = pointer, next 8 bytes = int64_t len.
 * This matches Rawc_List layout at offset 8, so GetElementPtr
 * for PyVarObject.ob_size works for both lists and dicts/sets.
 */
typedef struct Rawc_Dict {
    Rawc_DictEntry *entries;  /* offset 0 */
    int64_t len;               /* offset 8: same position as Rawc_List.len */
    int32_t capacity;
    int32_t int0_set;
    intptr_t int0_value;
} Rawc_Dict;

#define RAWC_DICT_INITIAL_CAP 16

/*
 * Hash function. Keys are either:
 *   - String pointers (>= 4096): FNV-1a on string content
 *   - Small integers (< 4096): Knuth multiplicative hash
 */
static inline uint32_t rawc_dict_hash(intptr_t key) {
    if ((uintptr_t)key < 4096)
        return (uint32_t)key * 2654435761u;
    /* UCS-4 string: hash the uint32_t code points */
    int64_t len = ((int64_t *)key)[-1];
    const uint32_t *data = (const uint32_t *)key;
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < len; i++) {
        h = (h ^ (data[i] & 0xFF)) * 16777619u;
        h = (h ^ ((data[i] >> 8) & 0xFF)) * 16777619u;
        h = (h ^ ((data[i] >> 16) & 0xFF)) * 16777619u;
        h = (h ^ ((data[i] >> 24) & 0xFF)) * 16777619u;
    }
    return h;
}

static inline int rawc_dict_keys_equal(intptr_t a, intptr_t b) {
    if (a == b) return 1;
    if ((uintptr_t)a < 4096 || (uintptr_t)b < 4096) return 0;
    /* UCS-4 string comparison: compare length then content */
    int64_t la = ((int64_t *)a)[-1], lb = ((int64_t *)b)[-1];
    if (la != lb) return 0;
    return memcmp((const void *)a, (const void *)b, la * sizeof(uint32_t)) == 0;
}

static inline Rawc_Dict *Rawc_Dict_New(void) {
    Rawc_Dict *d = (Rawc_Dict *)rawc_alloc(sizeof(Rawc_Dict));
    d->capacity = RAWC_DICT_INITIAL_CAP;
    d->len = 0;
    d->int0_set = 0;
    d->int0_value = 0;
    d->entries = (Rawc_DictEntry *)rawc_alloc(sizeof(Rawc_DictEntry) * d->capacity);
    memset(d->entries, 0, sizeof(Rawc_DictEntry) * d->capacity);
    return d;
}

static inline void Rawc_Dict_Grow(Rawc_Dict *d) {
    int32_t old_cap = d->capacity;
    Rawc_DictEntry *old = d->entries;
    d->capacity = old_cap * 2;
    d->entries = (Rawc_DictEntry *)rawc_alloc(sizeof(Rawc_DictEntry) * d->capacity);
    memset(d->entries, 0, sizeof(Rawc_DictEntry) * d->capacity);
    d->len = 0;
    for (int32_t i = 0; i < old_cap; i++) {
        if (old[i].key) {
            uint32_t idx = old[i].hash & (d->capacity - 1);
            while (d->entries[idx].key) idx = (idx + 1) & (d->capacity - 1);
            d->entries[idx] = old[i];
            d->len++;
        }
    }
}

static inline void Rawc_Dict_Set(Rawc_Dict *d, intptr_t key, intptr_t val) {
    if (d->len * 4 >= d->capacity * 3) Rawc_Dict_Grow(d);  /* 75% load */
    uint32_t h = rawc_dict_hash(key);
    uint32_t idx = h & (d->capacity - 1);
    while (d->entries[idx].key) {
        if (d->entries[idx].hash == h && rawc_dict_keys_equal(d->entries[idx].key, key)) {
            d->entries[idx].value = val;
            return;
        }
        idx = (idx + 1) & (d->capacity - 1);
    }
    d->len++;
    d->entries[idx].key = key;
    d->entries[idx].value = val;
    d->entries[idx].hash = h;
}

static inline intptr_t Rawc_Dict_Get(Rawc_Dict *d, intptr_t key) {
    uint32_t h = rawc_dict_hash(key);
    uint32_t idx = h & (d->capacity - 1);
    while (d->entries[idx].key) {
        if (d->entries[idx].hash == h && rawc_dict_keys_equal(d->entries[idx].key, key))
            return d->entries[idx].value;
        idx = (idx + 1) & (d->capacity - 1);
    }
    return 0;
}

static inline int Rawc_Dict_Contains(Rawc_Dict *d, intptr_t key) {
    /* Probe the hash table directly — can't use Get since 0 is a valid value (None) */
    uint32_t h = rawc_dict_hash(key);
    uint32_t idx = h & (d->capacity - 1);
    while (d->entries[idx].key) {
        if (d->entries[idx].hash == h && rawc_dict_keys_equal(d->entries[idx].key, key))
            return 1;
        idx = (idx + 1) & (d->capacity - 1);
    }
    return 0;
}

/* Integer key 0: special-cased because 0 is the empty-slot marker */
static inline void Rawc_Dict_SetInt0(Rawc_Dict *d, intptr_t val) {
    d->int0_set = 1; d->int0_value = val;
}
static inline intptr_t Rawc_Dict_GetInt0(Rawc_Dict *d) {
    return d->int0_set ? d->int0_value : 0;
}
static inline int Rawc_Dict_ContainsInt0(Rawc_Dict *d) {
    return d->int0_set;
}

static inline Rawc_Dict *Rawc_Dict_SetDefault(Rawc_Dict *d, intptr_t key) {
    intptr_t existing = Rawc_Dict_Get(d, key);
    if (existing) return (Rawc_Dict *)existing;
    Rawc_Dict *child = Rawc_Dict_New();
    Rawc_Dict_Set(d, key, (intptr_t)child);
    return child;
}

#endif /* MYPYC_RAWC_RT_H */
