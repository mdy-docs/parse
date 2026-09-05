/*
 * mdyast — MDY document text to a hast tree, in C.
 *
 * WHY THIS EXISTS. mdy-docs parses MDY into hast and everything downstream —
 * rehype plugins, the query engine, a template's `$.render` — works on that
 * tree. hast is the extension point, which is why the native backend embeds a
 * JavaScript engine at all. Moving the PARSE into C does not remove that
 * extension point; it removes the largest single cost in front of it. On the
 * reference corpus the front end is 4,441 lines of JavaScript producing 285k
 * nodes from 6.5 MB of text, and it was measured at 8.8x slower under QuickJS
 * than under V8 — the one component where that ratio decides a build's wall
 * clock.
 *
 * WHAT THE TREE ACTUALLY IS, measured rather than assumed. Across the whole
 * reference corpus:
 *
 *     3 node types      root, element, text (a layout adds doctype)
 *     32 tag names      a, sup, p, li, em, td, img, figure, tr, br, h2-h4, …
 *     19 property names href, id, className, dataFootnoteRef, src, width, …
 *
 * That is a much smaller thing than hast in general, and it is what makes a C
 * implementation tractable: no comments, no doctypes, no raw nodes, and a
 * closed vocabulary small enough to intern.
 *
 * NAMING. Every exported symbol is `mdy_`-prefixed, deliberately. Two of the
 * C projects already in this family — lamassu's regex engine and nisaba's —
 * export unprefixed names like `compile_into` and `vm_execute_internal`, and
 * linking both into one binary silently bound one library's calls to the
 * other's differently versioned implementation. A prefix is cheap; finding
 * that is not.
 */
#ifndef MDYAST_H
#define MDYAST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the tree ------------------------------------------------------------ */

typedef enum {
    MDY_ROOT = 0,
    MDY_ELEMENT,
    MDY_TEXT,
    /*
     * `<!doctype html>`. A fourth type, and one this measured its way past at
     * first: a scan of the reference corpus found three, because it scanned
     * the corpus's DOCUMENTS and a doctype only appears in a site's LAYOUTS.
     * Measuring the wrong population is its own kind of assumption.
     */
    MDY_DOCTYPE,
} mdy_node_type;

/*
 * A property's value. hast allows strings, numbers, booleans and arrays of
 * strings; the corpus uses the first three plus `className`, which is always
 * an array. Keeping the union closed to what is actually produced means an
 * unexpected shape is a compile error rather than a silent stringification.
 */
typedef enum {
    MDY_PROP_STRING = 0,
    MDY_PROP_NUMBER,
    MDY_PROP_BOOL,
    MDY_PROP_LIST,   /* space-free list of strings — className, and only that */
} mdy_prop_type;

typedef struct mdy_prop {
    const char *name;        /* interned; the hast property name, e.g. "className" */
    mdy_prop_type type;
    union {
        const char *string;  /* arena-owned */
        double number;
        int boolean;
    } as;
    const char **list;       /* MDY_PROP_LIST: arena-owned, list_len entries */
    size_t list_len;
    struct mdy_prop *next;   /* insertion order, which the emitter preserves */
} mdy_prop;

typedef struct mdy_node {
    mdy_node_type type;
    const char *tag;         /* MDY_ELEMENT: interned tag name */
    const char *text;        /* MDY_TEXT: arena-owned value */
    mdy_prop *props;         /* MDY_ELEMENT: first property, or NULL */
    mdy_prop *props_tail;    /* so appending stays O(1) */
    struct mdy_node *first;  /* first child */
    struct mdy_node *last;   /* last child, so appending stays O(1) */
    struct mdy_node *next;   /* next sibling */
    /*
     * unist position, on BLOCK elements only — inline ones (strong, em, a) do
     * not carry one, nor does the root, nor the synthesised `<article>` and
     * footnotes `<section>`, which come from no source line. Zero means none.
     *
     * 1-based, and pointing at the ORIGINAL file when `line_offset` is set.
     * Columns are UTF-16 code units, counted from the start of the line
     * INCLUDING its indentation — which is why an indented block still starts
     * at column 1 rather than at its indent. mdy-docs reports warnings against
     * these, so they are part of the contract rather than a debugging aid.
     */
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} mdy_node;

/* ---- parsing ------------------------------------------------------------- */

/*
 * The subset of mdy-docs' options that changes the TREE. Everything left out
 * either changes nothing structural or belongs to a stage this does not
 * implement — see README.md's coverage table, which is generated from the
 * comparison harness rather than written by hand.
 */
typedef struct {
    int documents;      /* a line of exactly `---` starts a new document */
    /*
     * What each document is wrapped in. NULL means the default, `article`; an
     * EMPTY string means no wrapper at all, and the documents run together —
     * `documents: false` in the JavaScript's stream settings, which is a
     * different thing from `documents: false` the option.
     */
    const char *document_wrapper;
    int frontmatter;    /* a fence at the top is YAML, not content */
    /* The line that opens and closes it. NULL means the default, `+++`. */
    const char *frontmatter_fence;
    int autolink;       /* bare URLs in text become links */
    int emphasis;       /* the default inline marker table */
    int max_heading;    /* deeper headings clamp to this; 0 means 6 */
    uint32_t line_offset; /* added to every position */
    /* Whether the emitter writes unist positions. On by default, matching
     * mdy-docs; off makes the JSON about structure alone, which is what a
     * test comparing trees usually wants. */
    int positions;
    /*
     * Whether the element allowlist and attribute checks apply. ON by default,
     * as mdy-docs' own default is — but the DOCUMENT ENGINE turns it off
     * (`sanitize: false` in src/mdy.js), because by then the author is the
     * template and the template already ran in a sandbox. A parser embedded in
     * that engine has to be able to say so.
     */
    int sanitize;
} mdy_options;

void mdy_options_default(mdy_options *out);

/* An opaque parse result. Holds the arena the whole tree lives in, so freeing
 * this frees every node at once — no per-node ownership anywhere. */
typedef struct mdy_doc mdy_doc;

/*
 * Parse UTF-8 MDY text. Never returns NULL for valid input; a document that
 * makes no sense produces a tree with whatever could be recovered, which is
 * what the JavaScript does. `len` may be 0 for a NUL-terminated string.
 */
mdy_doc *mdy_parse(const char *text, size_t len, const mdy_options *options);

/*
 * A warning about something the parser changed or dropped.
 *
 * mdy-docs reports these on the vfile — `file.message(reason, {place, ruleId,
 * source: 'mdy'})` — and a build surfaces them to whoever wrote the document.
 * They are part of what the front end produces, not a debugging aid: a
 * `<script>` silently vanishing is a worse answer than one that says so.
 */
typedef struct {
    const char *reason;   /* the sentence, already assembled */
    const char *rule;     /* ruleId: "sanitize", "void-element", "heading-depth" */
    uint32_t line, column;
    uint32_t end_line, end_column;
} mdy_message;

size_t mdy_message_count(const mdy_doc *doc);
const mdy_message *mdy_message_at(const mdy_doc *doc, size_t i);

/*
 * A document's front matter, as SOURCE.
 *
 * The block is found here — where the fence rule lives, next to everything
 * else that reads lines — and parsed by whoever embeds this, because it is
 * YAML and a YAML reader is not a thing to write twice. One entry per
 * document, in order, with `source` NULL when that document had none.
 */
typedef struct {
    const char *source;   /* the text between the fences, NULL when absent */
    size_t source_len;
    uint32_t open_line;   /* 1-based, the opening fence */
    uint32_t close_line;  /* the closing fence */
} mdy_frontmatter;

size_t mdy_frontmatter_count(const mdy_doc *doc);
const mdy_frontmatter *mdy_frontmatter_at(const mdy_doc *doc, size_t i);

/*
 * What a document referred to: its `#tags`, its `@mentions` and the pages it
 * linked to.
 *
 * A document is asked this often enough that it should not have to be read
 * again to answer, so they are written down while the tree is built. Names go
 * in as they are WRITTEN, in the order the document reaches them, and only
 * once each. `document` says which document in a stream reached it.
 */
typedef enum { MDY_REF_TAG, MDY_REF_MENTION, MDY_REF_LINK } mdy_ref_kind;

typedef struct {
    mdy_ref_kind kind;
    const char *name;
    size_t name_len;
    uint32_t document;    /* 0-based index in a stream */
} mdy_reference;

size_t mdy_reference_count(const mdy_doc *doc);
const mdy_reference *mdy_reference_at(const mdy_doc *doc, size_t i);

/* The root node of a parsed document. Valid until mdy_free. */
const mdy_node *mdy_root(const mdy_doc *doc);

/* Bytes the arena is holding — the whole cost of the tree, since nothing is
 * allocated outside it. */
size_t mdy_bytes(const mdy_doc *doc);

void mdy_free(mdy_doc *doc);

/* ---- emitting ------------------------------------------------------------ */

/*
 * The tree as JSON, with object keys in a fixed order, so two implementations'
 * output can be compared byte for byte. That is what this is for: the only way
 * to port a 4,441-line parser safely is to diff it against the original over a
 * real corpus, document by document. See test/compare.mjs.
 *
 * Caller frees. NULL on allocation failure.
 */
char *mdy_to_json(const mdy_node *node);

/* The same without unist positions — the tree's structure alone, which is what
 * a test comparing shapes wants. */
char *mdy_to_json_bare(const mdy_node *node);

#ifdef __cplusplus
}
#endif
#endif
