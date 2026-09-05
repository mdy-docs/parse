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
    /*
     * The prefix a footnote's ids are built on. `user-content-` for the first
     * document, `user-content-<n>-` for the nth in a stream — two documents on
     * one page must not both own `#user-content-fn-1`.
     */
    const char *note_prefix;

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
void mdy_clear_class(mdy_doc *doc, mdy_node *el);

/* ---- attributes and the schema ------------------------------------------- */

const char *mdy_hast_name(mdy_doc *doc, const char *name, size_t len);
int mdy_tag_allowed(const char *tag);
int mdy_tag_stripped(const char *tag);
int mdy_attr_allowed(const char *tag, const char *name, size_t len);
int mdy_protocol_allowed(const char *attr, const char *value, size_t len);

/* ---- Unicode (src/unicode.c) --------------------------------------------- */

/* One character in, one out. `\p{L}` or `\p{N}` — the question
 * defaultResolve asks of every character — and the simple lowercase mapping,
 * which is what String.prototype.toLowerCase uses. Both read baru-re's
 * generated UCD tables rather than approximating them. */
int mdy_is_letter_or_number_cp(uint32_t cp);

/* Whitespace as JavaScript means it, and a trim that uses it — String.trim
 * removes no-break spaces and the Unicode separators, and an ASCII-only trim
 * leaves them in a link's text. */
int mdy_is_js_space(uint32_t cp);
void mdy_trim(const char **s, size_t *len);
void mdy_trim_end(const char **s, size_t *len);
uint32_t mdy_lower_cp(uint32_t cp);

/* Decode one UTF-8 character, returning its width in bytes; an ill-formed
 * sequence is one byte and U+FFFD. Encode one, into 4 bytes of `out`. */
size_t mdy_utf8_decode(const char *p, size_t left, uint32_t *out);
size_t mdy_utf8_encode(uint32_t cp, char *out);

/* The UTF-16 boundary — what a JavaScript engine's strings are, and what
 * unist positions count in. Astral characters are two units, not one. */
size_t mdy_utf16_length(const char *s, size_t len);
size_t mdy_to_utf16(const char *s, size_t len, uint16_t *out, size_t cap);
size_t mdy_from_utf16(const uint16_t *units, size_t count, char *out, size_t cap);

/* Where a bare `[[ label ]]` points — mdy-docs' defaultResolve. Headings use
 * it too, so a heading and a link written from the same text agree; it is NOT
 * `slugify`, which hyphenates what this deletes. */
const char *mdy_resolve_slug(mdy_doc *doc, const char *s, size_t len, size_t *out_len);

/* ---- links (src/linkify.c) ----------------------------------------------- */

/* One autolinked span, as byte offsets into the text. */
typedef struct { size_t start, end; } mdy_link;

/*
 * Every URL in `text`, in order — a port of linkify-it, which is what mdy-docs
 * uses. Returns how many were written. See src/linkify.c for what "port"
 * covers and why it is not a set of heuristics.
 */
size_t mdy_find_links(const char *text, size_t len, mdy_link *out, size_t max);

/* ---- emoji (src/emoji.c) -------------------------------------------------- */

/*
 * An emoji at `p`, or NULL. `at_boundary` says whether this position starts a
 * word — the run began here, whitespace preceded it, or a marker was just
 * consumed — which emoticons require and shortcodes do not.
 */
const char *mdy_match_emoji(const char *p, size_t left, int at_boundary, size_t *consumed);

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
    /* The whole line's length in UTF-16 units, indentation included — what a
     * position's end column is measured in. Kept from before the indent was
     * stripped, because that is the only point it is still known. */
    uint32_t units;
} mdy_line;

/* Give a block element its unist position: from the first of its lines to the
 * end of the last. */
void mdy_set_position(mdy_node *node, const mdy_line *lines, size_t from, size_t to);

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
