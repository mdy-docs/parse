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

/* ---- footnotes ----------------------------------------------------------- */

/*
 * `[[ ^id ]]` references and `[[ ^id ]]: text` definitions.
 *
 * Three rules make this a document-level pass rather than an inline rule, and
 * each was established by asking the JavaScript rather than by reading it:
 *
 *   - A reference with no definition stays literal text. So definitions have
 *     to be collected before any inline parsing runs.
 *   - Numbering follows the order of FIRST REFERENCE, not the order of
 *     definition — `[[ ^b ]]` referenced first is footnote 1 — and the list at
 *     the end is in that order too.
 *   - A definition nothing references is dropped entirely, and a document with
 *     no referenced footnotes gets no section at all.
 */
typedef struct {
    const char *id;         /* interned */
    const char *content;    /* arena-owned, the text after the colon */
    size_t content_len;
    int number;             /* 1-based, assigned at first reference; 0 = unreferenced */
    int refs;               /* how many references have been seen */
} mdy_footnote;

/* The definition for `id`, or NULL. */
mdy_footnote *mdy_footnote_find(mdy_doc *doc, const char *id, size_t len);

/* Record one reference, assigning the footnote's number if this is the first.
 * Returns which reference this is, 1-based — the suffix on its id. */
int mdy_footnote_reference(mdy_doc *doc, mdy_footnote *note);

/* Append the `<section>` of collected footnotes, if any were referenced. */
void mdy_footnote_section(mdy_doc *doc, mdy_node *parent);

/* ---- the document -------------------------------------------------------- */

struct mdy_doc {
    mdy_arena arena;
    mdy_intern_table names;
    mdy_node *root;
    mdy_options options;

    mdy_footnote *notes;
    size_t note_count, note_cap;
    int next_number;

    /* Every heading id already used, so a repeat gets `-1`, `-2`, … — see the
     * heading rule in block.c. Shared across a stream's documents on purpose:
     * two articles on one page must not both own `#introduction`. */
    const char **heading_ids;
    size_t heading_count, heading_cap;
};

/* ---- building ------------------------------------------------------------ */

mdy_node *mdy_new_element(mdy_doc *doc, const char *tag, size_t tag_len);
mdy_node *mdy_new_text(mdy_doc *doc, const char *text, size_t len);
void mdy_append(mdy_node *parent, mdy_node *child);
void mdy_set_string(mdy_doc *doc, mdy_node *el, const char *name, const char *value, size_t value_len);
void mdy_set_number(mdy_doc *doc, mdy_node *el, const char *name, double value);
void mdy_set_bool(mdy_doc *doc, mdy_node *el, const char *name, int value);
void mdy_add_class(mdy_doc *doc, mdy_node *el, const char *class_name);

/* ---- attributes and the schema ------------------------------------------- */

const char *mdy_hast_name(mdy_doc *doc, const char *name, size_t len);
int mdy_tag_allowed(const char *tag);
int mdy_attr_allowed(const char *tag, const char *name, size_t len);
int mdy_protocol_allowed(const char *attr, const char *value, size_t len);

/* Lowercase one UTF-8 character into `out` (which must hold 4 bytes),
 * returning how many bytes it consumed from `in`. */
size_t mdy_lower_utf8(const char *in, size_t left, char *out);

/* Whether one UTF-8 character is a letter or a number — `\p{L}` or `\p{N}`,
 * which is the question defaultResolve asks of every character. */
int mdy_is_letter_or_number(const char *p, size_t left);

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

/*
 * Parse a run of lines as blocks into `parent`.
 *
 * `base` is the column this run sits at, and it has to be passed rather than
 * inferred: a line further in than `base` is a block of its own and gets a
 * <div>. At the root that is 0 — so a document whose FIRST line is indented
 * still gets a div — while inside an element it is that element's children's
 * own indentation, or every child would get one. Inferring it from the first
 * line gets the root case wrong; inferring it from the parent gets the element
 * case wrong.
 */
void mdy_parse_block(mdy_doc *doc, mdy_node *parent, const mdy_line *lines, size_t count, size_t base);

/* Parse inline content into `parent`. The one entry point the block parser
 * uses, so everything textual goes through the same rules. */
void mdy_parse_inline(mdy_doc *doc, mdy_node *parent, const char *text, size_t len);

#endif
