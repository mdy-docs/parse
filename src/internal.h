/*
 * Shared internals. Nothing here is public — see ../include/mdyast.h.
 */
#ifndef MDYAST_INTERNAL_H
#define MDYAST_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mdyast.h"

/* ---- the arena ----------------------------------------------------------- */

/*
 * A bump allocator, and the reason the tree has no ownership rules at all: a
 * node, its text, its property names and its property values all live here,
 * and mdy_free drops the whole thing in one call.
 *
 * This is not only tidiness. The JavaScript builds 285k nodes for the
 * reference corpus, each a separate heap object with its own properties
 * object; that allocation traffic is a real share of what the port is trying
 * to remove, and an arena is how a C implementation actually wins rather than
 * reproducing the same cost with different syntax.
 */
typedef struct mdy_block {
    struct mdy_block *next;
    size_t used;
    size_t size;
    char data[];
} mdy_block;

typedef struct {
    mdy_block *head;
    size_t total;
} mdy_arena;

void *mdy_alloc(mdy_arena *arena, size_t size);
char *mdy_strdup_n(mdy_arena *arena, const char *s, size_t len);
void mdy_arena_free(mdy_arena *arena);

/* ---- interning ----------------------------------------------------------- */

/*
 * Tag and property names are a closed vocabulary — 32 and 19 respectively
 * across the whole reference corpus — so they are interned to a single
 * pointer each. Comparing tags becomes a pointer compare, and the emitter
 * never copies a name.
 */
typedef struct mdy_interned {
    struct mdy_interned *next;
    size_t len;
    char text[];
} mdy_interned;

typedef struct {
    mdy_interned *buckets[128];
} mdy_intern_table;

const char *mdy_intern(mdy_arena *arena, mdy_intern_table *table, const char *s, size_t len);

/* ---- the document -------------------------------------------------------- */

struct mdy_doc {
    mdy_arena arena;
    mdy_intern_table names;
    mdy_node *root;
    mdy_options options;
};

/* ---- building ------------------------------------------------------------ */

mdy_node *mdy_new_element(mdy_doc *doc, const char *tag, size_t tag_len);
mdy_node *mdy_new_text(mdy_doc *doc, const char *text, size_t len);
void mdy_append(mdy_node *parent, mdy_node *child);
void mdy_set_string(mdy_doc *doc, mdy_node *el, const char *name, const char *value, size_t value_len);
void mdy_set_number(mdy_doc *doc, mdy_node *el, const char *name, double value);
void mdy_set_bool(mdy_doc *doc, mdy_node *el, const char *name, int value);
void mdy_add_class(mdy_doc *doc, mdy_node *el, const char *class_name);

/* ---- the stages ---------------------------------------------------------- */

/* One line of the source, already measured. The block parser works on these
 * rather than on raw text, because indentation is structural in MDY and every
 * rule needs the width before it needs the content. */
typedef struct {
    const char *text;   /* not NUL terminated — use len */
    size_t len;
    size_t indent;      /* columns of leading space, tabs counted as 2 */
    int blank;
    uint32_t number;    /* 1-based line number in the original file */
} mdy_line;

void mdy_parse_block(mdy_doc *doc, mdy_node *parent, const mdy_line *lines, size_t count);

/* Parse inline content into `parent`. The one entry point the block parser
 * uses, so everything textual goes through the same rules. */
void mdy_parse_inline(mdy_doc *doc, mdy_node *parent, const char *text, size_t len);

#endif
