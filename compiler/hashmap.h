#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

// Open-addressed string-keyed hashmap.
//
// Used by the compiler's hot-path lookups (struct/field name resolution
// in irgen, function lookup in checker, alias rewrites in main, string-
// pool intern in iremit, monomorph worklist + template lookup, regalloc
// label-block lookup). Key invariants:
//
//   - Keys are NUL-terminated strings; the hashmap takes ownership of a
//     duplicate (`strdup`) at `put` time. Callers do not need to keep
//     the original key buffer alive.
//   - Values are opaque `void *` pointers. The hashmap does not own the
//     pointed-to memory; the caller is responsible for keeping values
//     alive for the lifetime of the map and freeing them after
//     `hashmap_free`.
//   - A NULL value is legal storage. Distinguish "key not present" from
//     "key present with NULL value" via `hashmap_contains`.
//   - Collision strategy: open addressing with linear probing.
//   - Resize threshold: 75% load factor; capacity doubles each resize.
//     `O(1)` amortised on put/get/contains.
//   - Iteration order is deterministic per (insertion sequence, capacity)
//     but is NOT insertion order. If you need stable ordering, keep a
//     separate index array alongside the map.

typedef struct Hashmap Hashmap;

// Create a new hashmap with the given initial capacity. Capacity is
// rounded up to a power of two; pass 0 for a sensible default (16).
Hashmap *hashmap_new(size_t initial_cap);

// Free the hashmap and its internal storage. Does NOT free the values
// stored in it; the caller owns those.
void hashmap_free(Hashmap *m);

// Insert or replace `value` under `key`. The map keeps its own copy of
// the key string. Replacing an existing entry returns the previously-
// stored value via `*old_value` (if non-NULL); otherwise `*old_value`
// is set to NULL. Pass NULL for `old_value` if you don't care.
void hashmap_put(Hashmap *m, const char *key, void *value);

// Get the value stored under `key`, or NULL if no such key. Cannot
// distinguish "missing" from "present with NULL value" — use
// `hashmap_contains` for that.
void *hashmap_get(const Hashmap *m, const char *key);

// Returns 1 if `key` is present in the map, 0 otherwise.
int hashmap_contains(const Hashmap *m, const char *key);

// Number of entries currently stored.
size_t hashmap_size(const Hashmap *m);

// Iteration. Usage:
//   for (HashmapIter it = hashmap_begin(m); it.valid; it = hashmap_next(it)) {
//       const char *k = it.key;
//       void *v = it.value;
//       ...
//   }
typedef struct {
    const Hashmap *map;
    size_t bucket;
    int valid;
    const char *key;
    void *value;
} HashmapIter;

HashmapIter hashmap_begin(const Hashmap *m);
HashmapIter hashmap_next(HashmapIter it);

#endif
