// Open-addressed string-keyed hashmap. See compiler/hashmap.h for the
// contract. Implementation notes:
//
//   - FNV-1a 64-bit for the hash. Good distribution on short
//     identifier-shaped strings, no library dependency, branch-free.
//   - Empty slots are marked by a NULL `key`; tombstones are NOT used.
//     This codebase doesn't need delete (a Potato compiler run is
//     one-shot — every map is built up and never shrinks), so the
//     simpler invariant ("key == NULL iff empty") holds.
//   - Resize at 75% load factor. New capacity = 2 * old; rehash every
//     live entry into the new table.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "hashmap.h"

typedef struct {
    char *key;     // strdup'd; NULL == empty slot
    void *value;
} Bucket;

struct Hashmap {
    Bucket *buckets;
    size_t cap;     // always a power of two
    size_t size;    // live entries
};

static size_t round_up_pow2(size_t n) {
    if (n < 2) return 2;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// FNV-1a 64-bit. Reduced to bucket index via `& (cap - 1)` because
// `cap` is always a power of two.
static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

Hashmap *hashmap_new(size_t initial_cap) {
    Hashmap *m = malloc(sizeof(Hashmap));
    if (!m) return NULL;
    m->cap = round_up_pow2(initial_cap > 0 ? initial_cap : 16);
    m->size = 0;
    m->buckets = calloc(m->cap, sizeof(Bucket));
    if (!m->buckets) { free(m); return NULL; }
    return m;
}

void hashmap_free(Hashmap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) free(m->buckets[i].key);
    free(m->buckets);
    free(m);
}

// Probe for `key`. Returns the bucket index either holding `key` or the
// first empty slot encountered (open addressing with linear probing).
static size_t probe(const Hashmap *m, const char *key) {
    size_t mask = m->cap - 1;
    size_t i = (size_t)fnv1a(key) & mask;
    while (m->buckets[i].key && strcmp(m->buckets[i].key, key) != 0) {
        i = (i + 1) & mask;
    }
    return i;
}

static void resize(Hashmap *m, size_t new_cap) {
    Bucket *old = m->buckets;
    size_t old_cap = m->cap;
    m->buckets = calloc(new_cap, sizeof(Bucket));
    m->cap = new_cap;
    m->size = 0;
    if (!m->buckets) {
        // Unrecoverable allocation failure during resize. Restore the
        // old table so the caller's map state isn't corrupted, then
        // bail. The compiler's process is one-shot anyway — if this
        // ever fires, the host is OOM and we want to die cleanly.
        m->buckets = old;
        m->cap = old_cap;
        abort();
    }
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].key) continue;
        // Re-insert directly to avoid re-strdup'ing the key.
        size_t mask = new_cap - 1;
        size_t j = (size_t)fnv1a(old[i].key) & mask;
        while (m->buckets[j].key) j = (j + 1) & mask;
        m->buckets[j].key = old[i].key;
        m->buckets[j].value = old[i].value;
        m->size++;
    }
    free(old);
}

void hashmap_put(Hashmap *m, const char *key, void *value) {
    // Load factor check BEFORE insert so we never exceed 75%.
    if ((m->size + 1) * 4 >= m->cap * 3) resize(m, m->cap * 2);

    size_t i = probe(m, key);
    if (m->buckets[i].key) {
        // Replace.
        m->buckets[i].value = value;
        return;
    }
    m->buckets[i].key = strdup(key);
    m->buckets[i].value = value;
    m->size++;
}

void *hashmap_get(const Hashmap *m, const char *key) {
    size_t i = probe(m, key);
    return m->buckets[i].key ? m->buckets[i].value : NULL;
}

int hashmap_contains(const Hashmap *m, const char *key) {
    size_t i = probe(m, key);
    return m->buckets[i].key != NULL;
}

size_t hashmap_size(const Hashmap *m) { return m->size; }

static HashmapIter advance_to_live(const Hashmap *m, size_t start) {
    HashmapIter it = { .map = m, .bucket = start, .valid = 0,
                       .key = NULL, .value = NULL };
    for (size_t i = start; i < m->cap; i++) {
        if (m->buckets[i].key) {
            it.bucket = i;
            it.valid = 1;
            it.key = m->buckets[i].key;
            it.value = m->buckets[i].value;
            return it;
        }
    }
    return it;
}

HashmapIter hashmap_begin(const Hashmap *m) {
    return advance_to_live(m, 0);
}

HashmapIter hashmap_next(HashmapIter it) {
    if (!it.valid) return it;
    return advance_to_live(it.map, it.bucket + 1);
}
