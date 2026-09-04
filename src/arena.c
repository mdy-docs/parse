/*
 * The arena and the intern table. See internal.h for why both exist.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define MDY_BLOCK_MIN (64 * 1024)

void *mdy_alloc(mdy_arena *arena, size_t size) {
    size = (size + 15) & ~(size_t)15;   /* 16-byte aligned, enough for anything here */

    if (!arena->head || arena->head->size - arena->head->used < size) {
        size_t block = MDY_BLOCK_MIN;
        while (block < size + sizeof(mdy_block)) block *= 2;
        mdy_block *next = malloc(block);
        if (!next) return NULL;
        next->next = arena->head;
        next->used = 0;
        next->size = block - sizeof(mdy_block);
        arena->head = next;
        arena->total += block;
    }

    void *p = arena->head->data + arena->head->used;
    arena->head->used += size;
    return p;
}

char *mdy_strdup_n(mdy_arena *arena, const char *s, size_t len) {
    char *out = mdy_alloc(arena, len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

void mdy_arena_free(mdy_arena *arena) {
    for (mdy_block *b = arena->head; b;) {
        mdy_block *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
    arena->total = 0;
}

/* FNV-1a, folded to the table size. The vocabulary is tiny and fixed, so
 * anything reasonable distributes it; this is chosen for being short. */
static size_t hash_of(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h & 127;
}

const char *mdy_intern(mdy_arena *arena, mdy_intern_table *table, const char *s, size_t len) {
    size_t bucket = hash_of(s, len);
    for (mdy_interned *e = table->buckets[bucket]; e; e = e->next) {
        if (e->len == len && memcmp(e->text, s, len) == 0) return e->text;
    }
    mdy_interned *entry = mdy_alloc(arena, sizeof *entry + len + 1);
    if (!entry) return NULL;
    entry->len = len;
    if (len) memcpy(entry->text, s, len);
    entry->text[len] = '\0';
    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    return entry->text;
}
