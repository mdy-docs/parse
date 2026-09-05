/*
 * Markdown to hast — the contract is in include/mdymarkdown.h.
 *
 * md4c reports a document as a stream of enter/leave callbacks, which suits
 * building a tree directly: a stack of open nodes, and every callback appends
 * to whatever is on top. No intermediate representation, and no second pass.
 *
 * The shapes here were taken from mdy-docs' own pipeline rather than derived
 * from the spec — the newline padding between block children, `language-x` on
 * a code element, the task-list classes, `start` only when it is not 1. Those
 * are remark-rehype's choices, and a port that reasons them out gets them
 * subtly wrong.
 */
#include <stdlib.h>
#include <string.h>

#include "md4c.h"
#include "entity.h"
#include "mdymarkdown.h"
#include "internal.h"

enum { STACK_MAX = 128 };

/*
 * remark-rehype's `wrap(nodes, loose)`, which is where every `\n` in the tree
 * comes from:
 *
 *   loose   a newline BEFORE the first child, between each pair, and after
 *           the last — what a <ul>, <blockquote>, <table> or a loose <li> gets
 *   tight   a newline between each pair and nowhere else — what the root gets,
 *           and what a tight <li> gets
 *
 * Getting it wrong is a difference on every document rather than an unusual
 * one: the first version padded after every block and was wrong 1389 times
 * out of 1390, all of them a single trailing newline.
 */
typedef struct {
    mdy_node *node;
    int loose;
    int children;     /* how many block children have been appended */
} Frame;

typedef struct {
    mdy_doc *doc;
    Frame stack[STACK_MAX];
    int depth;
    /* A list's tightness, carried to its items. */
    int list_tight[STACK_MAX];
    int list_depth;
    /* Inside a code block or raw HTML, text arrives in pieces and has to be
     * gathered before it becomes one node. */
    char *pending;
    size_t pending_len, pending_cap;
    int gathering;
    /*
     * INLINE TEXT IS COALESCED. md4c reports text in pieces — a run, an
     * entity, a soft break, another run — and the reference produces one text
     * node per run of adjacent text. So text accumulates here and becomes a
     * node only when something else has to be appended.
     */
    char *inline_text;
    size_t inline_len, inline_cap;
    int failed;
} Build;

static void flush_text(Build *b);

static mdy_node *top(Build *b) { return b->depth > 0 ? b->stack[b->depth - 1].node : NULL; }
static Frame *frame(Build *b) { return b->depth > 0 ? &b->stack[b->depth - 1] : NULL; }

static void push(Build *b, mdy_node *n, int loose) {
    flush_text(b);
    if (b->depth >= STACK_MAX) { b->failed = 1; return; }
    b->stack[b->depth].node = n;
    b->stack[b->depth].loose = loose;
    b->stack[b->depth].children = 0;
    b->depth++;
}

static void pop(Build *b) { flush_text(b); if (b->depth > 0) b->depth--; }

static void append(Build *b, mdy_node *n) {
    flush_text(b);
    mdy_node *parent = top(b);
    if (parent && n) mdy_append(parent, n);
}

/* Text, held until it is finished. */
static void text_out(Build *b, const char *s, size_t len) {
    if (!len) return;
    if (b->inline_len + len + 1 > b->inline_cap) {
        size_t cap = b->inline_cap ? b->inline_cap : 256;
        while (cap < b->inline_len + len + 1) cap *= 2;
        char *grown = realloc(b->inline_text, cap);
        if (!grown) { b->failed = 1; return; }
        b->inline_text = grown;
        b->inline_cap = cap;
    }
    memcpy(b->inline_text + b->inline_len, s, len);
    b->inline_len += len;
    b->inline_text[b->inline_len] = '\0';
}

/* …and written out as ONE node when anything else happens. Every append,
 * push and pop goes through this first. */
static void flush_text(Build *b) {
    if (!b->inline_len) return;
    mdy_node *n = mdy_new_text(b->doc, b->inline_text, b->inline_len);
    b->inline_len = 0;
    mdy_node *parent = top(b);
    if (parent && n) mdy_append(parent, n);
}

static void text_node(Build *b, const char *s, size_t len) {
    if (!len) return;
    text_out(b, s, len);
}

static void newline(Build *b) {
    flush_text(b);
    mdy_node *parent = top(b);
    if (parent) mdy_append(parent, mdy_new_text(b->doc, "\n", 1));
}

/* Before a BLOCK child goes in: a newline between siblings, and one before
 * the first when the parent is loose. */
static void before_block(Build *b) {
    Frame *f = frame(b);
    if (!f) return;
    if (f->children > 0 || f->loose) newline(b);
    f->children++;
}

/* After the last: only a loose parent gets a trailing one, and only when it
 * had something in it. */
static void close_block(Build *b) {
    Frame *f = frame(b);
    if (f && f->loose && f->children > 0) newline(b);
    pop(b);
}

/* ---- gathering verbatim text (code blocks, raw HTML) ---------------------- */

static void gather(Build *b, const char *s, size_t len) {
    if (b->pending_len + len + 1 > b->pending_cap) {
        size_t cap = b->pending_cap ? b->pending_cap : 256;
        while (cap < b->pending_len + len + 1) cap *= 2;
        char *grown = realloc(b->pending, cap);
        if (!grown) { b->failed = 1; return; }
        b->pending = grown;
        b->pending_cap = cap;
    }
    memcpy(b->pending + b->pending_len, s, len);
    b->pending_len += len;
    b->pending[b->pending_len] = '\0';
}

static void flush_gathered(Build *b) {
    if (b->pending_len) text_out(b, b->pending, b->pending_len);
    b->pending_len = 0;
}

/* ---- entities ------------------------------------------------------------- */

static void put_codepoint(Build *b, unsigned cp) {
    char out[4];
    size_t n = 0;
    if (cp < 0x80) out[n++] = (char)cp;
    else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { out[n++] = (char)(0xF0 | (cp >> 18)); out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
    if (b->gathering) gather(b, out, n);
    else text_node(b, out, n);
}

/*
 * `&#1234;`, `&#x12AB;` and the named ones. md4c ships the HTML5 entity table
 * as entity.c and deliberately does not use it itself — it is encoding
 * agnostic and hands the caller the entity text verbatim — so resolving is
 * this file's job, and the table is right there.
 *
 * An entity that is not in the table is written through as it was typed,
 * which is what CommonMark says to do with `&nope;`.
 */
static void entity(Build *b, const char *s, size_t len) {
    if (len >= 4 && s[1] == '#') {
        unsigned cp = 0;
        size_t i = 2;
        int hex = (s[2] == 'x' || s[2] == 'X');
        if (hex) i = 3;
        for (; i + 1 < len; i++) {
            char c = s[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else { cp = 0; break; }
            cp = cp * (hex ? 16u : 10u) + (unsigned)d;
        }
        if (cp == 0) cp = 0xFFFD;          /* CommonMark: NUL becomes U+FFFD */
        put_codepoint(b, cp);
        return;
    }
    const ENTITY *e = entity_lookup(s, len);
    if (e) {
        put_codepoint(b, e->codepoints[0]);
        if (e->codepoints[1]) put_codepoint(b, e->codepoints[1]);
        return;
    }
    if (b->gathering) gather(b, s, len);
    else text_node(b, s, len);
}

/* ---- attributes ------------------------------------------------------------ */

/* md4c hands an attribute as a run of substrings so entities can be resolved
 * in it; for the common case there is exactly one and it is plain text. */
static void set_attribute(Build *b, mdy_node *el, const char *name, const MD_ATTRIBUTE *a) {
    if (!a || !a->text || !a->size) return;
    mdy_set_string(b->doc, el, name, a->text, a->size);
}

/* ---- blocks ---------------------------------------------------------------- */

static int enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    Build *b = ud;
    if (b->failed) return -1;

    switch (type) {
        case MD_BLOCK_DOC:
            return 0;

        case MD_BLOCK_P: {
            before_block(b);
            mdy_node *p = mdy_new_element(b->doc, "p", 1);
            append(b, p);
            push(b, p, 0);
            return 0;
        }

        case MD_BLOCK_H: {
            const MD_BLOCK_H_DETAIL *d = detail;
            before_block(b);
            char tag[3] = { 'h', (char)('0' + d->level), '\0' };
            mdy_node *h = mdy_new_element(b->doc, tag, 2);
            append(b, h);
            push(b, h, 0);
            return 0;
        }

        case MD_BLOCK_QUOTE: {
            before_block(b);
            mdy_node *q = mdy_new_element(b->doc, "blockquote", 10);
            append(b, q);
            push(b, q, 1);
            return 0;
        }

        case MD_BLOCK_UL: {
            const MD_BLOCK_UL_DETAIL *d = detail;
            before_block(b);
            mdy_node *ul = mdy_new_element(b->doc, "ul", 2);
            append(b, ul);
            push(b, ul, 1);
            if (b->list_depth < STACK_MAX) b->list_tight[b->list_depth++] = d->is_tight;
            return 0;
        }

        case MD_BLOCK_OL: {
            const MD_BLOCK_OL_DETAIL *d = detail;
            before_block(b);
            mdy_node *ol = mdy_new_element(b->doc, "ol", 2);
            /* `<ol>` counts from 1 on its own; only say otherwise when asked. */
            if (d->start != 1) mdy_set_number(b->doc, ol, "start", (double)d->start);
            append(b, ol);
            push(b, ol, 1);
            if (b->list_depth < STACK_MAX) b->list_tight[b->list_depth++] = d->is_tight;
            return 0;
        }

        case MD_BLOCK_LI: {
            const MD_BLOCK_LI_DETAIL *d = detail;
            before_block(b);
            mdy_node *li = mdy_new_element(b->doc, "li", 2);
            if (d->is_task) {
                mdy_add_class(b->doc, li, "task-list-item");
                /* The list itself is marked once its first task item is seen. */
                mdy_node *list = top(b);
                if (list) {
                    int marked = 0;
                    for (mdy_prop *p = list->props; p; p = p->next)
                        if (strcmp(p->name, "className") == 0) marked = 1;
                    if (!marked) mdy_add_class(b->doc, list, "contains-task-list");
                }
            }
            append(b, li);
            /* A tight item's content is inline and unpadded; a loose one's is
             * a paragraph, wrapped like any other block parent. */
            push(b, li, b->list_depth > 0 && !b->list_tight[b->list_depth - 1]);
            if (d->is_task) {
                mdy_node *box = mdy_new_element(b->doc, "input", 5);
                mdy_set_string(b->doc, box, "type", "checkbox", 8);
                if (d->task_mark != ' ') mdy_set_bool(b->doc, box, "checked", 1);
                mdy_set_bool(b->doc, box, "disabled", 1);
                append(b, box);
                /* The space between the box and the text is content: md4c
                 * consumes it with the marker, and the reference keeps it.
                 * It joins the run that follows rather than standing alone. */
                text_out(b, " ", 1);
            }
            return 0;
        }

        case MD_BLOCK_HR: {
            before_block(b);
            append(b, mdy_new_element(b->doc, "hr", 2));
            return 0;
        }

        case MD_BLOCK_CODE: {
            const MD_BLOCK_CODE_DETAIL *d = detail;
            before_block(b);
            mdy_node *pre = mdy_new_element(b->doc, "pre", 3);
            mdy_node *code = mdy_new_element(b->doc, "code", 4);
            if (d->lang.text && d->lang.size) {
                char cls[128];
                size_t n = d->lang.size < sizeof cls - 10 ? d->lang.size : sizeof cls - 10;
                memcpy(cls, "language-", 9);
                memcpy(cls + 9, d->lang.text, n);
                cls[9 + n] = '\0';
                mdy_add_class(b->doc, code, cls);
            }
            append(b, pre);
            push(b, pre, 0);
            append(b, code);
            push(b, code, 0);
            b->gathering = 1;
            return 0;
        }

        case MD_BLOCK_HTML:
            /* Raw HTML arrives as text and stays raw until something parses
             * it — which is what rehype-raw does on the JavaScript side. */
            before_block(b);
            b->gathering = 1;
            return 0;

        case MD_BLOCK_TABLE: {
            before_block(b);
            mdy_node *t = mdy_new_element(b->doc, "table", 5);
            append(b, t);
            push(b, t, 1);
            return 0;
        }
        case MD_BLOCK_THEAD: {
            before_block(b);
            mdy_node *n = mdy_new_element(b->doc, "thead", 5);
            append(b, n); push(b, n, 1);
            return 0;
        }
        case MD_BLOCK_TBODY: {
            before_block(b);
            mdy_node *n = mdy_new_element(b->doc, "tbody", 5);
            append(b, n); push(b, n, 1);
            return 0;
        }
        case MD_BLOCK_TR: {
            before_block(b);
            mdy_node *n = mdy_new_element(b->doc, "tr", 2);
            append(b, n); push(b, n, 1);
            return 0;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            const MD_BLOCK_TD_DETAIL *d = detail;
            mdy_node *n = mdy_new_element(b->doc, type == MD_BLOCK_TH ? "th" : "td", 2);
            if (d && d->align != MD_ALIGN_DEFAULT) {
                const char *a = d->align == MD_ALIGN_LEFT ? "left"
                              : d->align == MD_ALIGN_CENTER ? "center" : "right";
                mdy_set_string(b->doc, n, "align", a, strlen(a));
            }
            before_block(b);
            append(b, n); push(b, n, 0);
            return 0;
        }

        default:
            /* An extension this front end does not map yet — its content is
             * kept, its wrapper is not. Silently dropping the content would
             * lose text; guessing a tag would invent structure. */
            return 0;
    }
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    Build *b = ud;
    (void)detail;
    if (b->failed) return -1;

    switch (type) {
        case MD_BLOCK_DOC:
            return 0;

        case MD_BLOCK_CODE:
            flush_gathered(b);
            b->gathering = 0;
            close_block(b);         /* code */
            close_block(b);         /* pre */
            return 0;

        case MD_BLOCK_HTML: {
            /* One raw node holding what was written. */
            if (b->pending_len) {
                mdy_node *raw = mdy_new_text(b->doc, b->pending, b->pending_len);
                if (raw) raw->type = MDY_RAW;
                append(b, raw);
            }
            b->pending_len = 0;
            b->gathering = 0;
            return 0;
        }

        case MD_BLOCK_P:
        case MD_BLOCK_H:
        case MD_BLOCK_QUOTE:
        case MD_BLOCK_TABLE:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
        case MD_BLOCK_LI:
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            close_block(b);
            return 0;

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            close_block(b);
            if (b->list_depth > 0) b->list_depth--;
            return 0;

        case MD_BLOCK_HR:
            return 0;

        default:
            return 0;
    }
}

/* ---- spans ------------------------------------------------------------------ */

static int enter_span(MD_SPANTYPE type, void *detail, void *ud) {
    Build *b = ud;
    if (b->failed) return -1;

    switch (type) {
        case MD_SPAN_EM:     { mdy_node *n = mdy_new_element(b->doc, "em", 2); append(b, n); push(b, n, 0); return 0; }
        case MD_SPAN_STRONG: { mdy_node *n = mdy_new_element(b->doc, "strong", 6); append(b, n); push(b, n, 0); return 0; }
        case MD_SPAN_DEL:    { mdy_node *n = mdy_new_element(b->doc, "del", 3); append(b, n); push(b, n, 0); return 0; }
        case MD_SPAN_U:      { mdy_node *n = mdy_new_element(b->doc, "u", 1); append(b, n); push(b, n, 0); return 0; }
        case MD_SPAN_CODE: {
            mdy_node *n = mdy_new_element(b->doc, "code", 4);
            append(b, n); push(b, n, 0);
            b->gathering = 1;
            return 0;
        }
        case MD_SPAN_A: {
            const MD_SPAN_A_DETAIL *d = detail;
            mdy_node *a = mdy_new_element(b->doc, "a", 1);
            set_attribute(b, a, "href", &d->href);
            set_attribute(b, a, "title", &d->title);
            append(b, a); push(b, a, 0);
            return 0;
        }
        case MD_SPAN_IMG: {
            const MD_SPAN_IMG_DETAIL *d = detail;
            mdy_node *img = mdy_new_element(b->doc, "img", 3);
            set_attribute(b, img, "src", &d->src);
            set_attribute(b, img, "title", &d->title);
            append(b, img);
            /* An image's children are its ALT text, which is an attribute
             * rather than content — gathered, then set on the way out. */
            push(b, img, 0);
            b->gathering = 1;
            return 0;
        }
        default:
            return 0;
    }
}

static int leave_span(MD_SPANTYPE type, void *detail, void *ud) {
    Build *b = ud;
    (void)detail;
    if (b->failed) return -1;

    switch (type) {
        case MD_SPAN_CODE:
            flush_gathered(b);
            b->gathering = 0;
            pop(b);
            return 0;
        case MD_SPAN_IMG: {
            mdy_node *img = top(b);
            if (img) mdy_set_string(b->doc, img, "alt", b->pending ? b->pending : "", b->pending_len);
            b->pending_len = 0;
            b->gathering = 0;
            pop(b);
            return 0;
        }
        case MD_SPAN_EM:
        case MD_SPAN_STRONG:
        case MD_SPAN_DEL:
        case MD_SPAN_U:
        case MD_SPAN_A:
            pop(b);
            return 0;
        default:
            return 0;
    }
}

/* ---- text ------------------------------------------------------------------- */

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *s, MD_SIZE size, void *ud) {
    Build *b = ud;
    if (b->failed) return -1;

    switch (type) {
        case MD_TEXT_NULLCHAR:
            put_codepoint(b, 0xFFFD);
            return 0;
        case MD_TEXT_BR:
            append(b, mdy_new_element(b->doc, "br", 2));
            return 0;
        case MD_TEXT_SOFTBR:
            /* A soft break is a newline in the text, not a node. */
            if (b->gathering) gather(b, "\n", 1); else text_node(b, "\n", 1);
            return 0;
        case MD_TEXT_ENTITY:
            entity(b, s, size);
            return 0;
        default:
            if (b->gathering) gather(b, s, size);
            else text_node(b, s, size);
            return 0;
    }
}

/* ---- heading ids -------------------------------------------------------------
 *
 * mdy's own slugger, so a `#anchor` written in one format lands on a heading
 * written in the other. Run over the finished tree rather than during the
 * build, because a heading's id comes from all of its text and md4c reports
 * that in pieces.
 */
static void collect_text(const mdy_node *n, char *out, size_t cap, size_t *len) {
    if (n->type == MDY_TEXT && n->text) {
        size_t add = strlen(n->text);
        if (*len + add < cap) { memcpy(out + *len, n->text, add); *len += add; }
    }
    for (const mdy_node *c = n->first; c; c = c->next) collect_text(c, out, cap, len);
}

static void identify_headings(mdy_doc *doc, mdy_node *n) {
    for (mdy_node *c = n->first; c; c = c->next) {
        if (c->type == MDY_ELEMENT && c->tag && c->tag[0] == 'h' &&
            c->tag[1] >= '1' && c->tag[1] <= '6' && c->tag[2] == '\0') {
            char text[1024];
            size_t len = 0;
            collect_text(c, text, sizeof text, &len);
            size_t id_len = 0;
            const char *id = mdy_resolve_slug(doc, text, len, &id_len);
            if (id && id_len) mdy_set_string(doc, c, "id", id, id_len);
        }
        identify_headings(doc, c);
    }
}

mdy_doc *mdy_markdown_parse(const char *text, size_t len) {
    if (!text) return NULL;
    if (len == 0) len = strlen(text);

    mdy_doc *doc = mdy_doc_new();
    if (!doc) return NULL;

    Build b = {0};
    b.doc = doc;
    push(&b, doc->root, 0);

    MD_PARSER parser = {
        .abi_version = 0,
        .flags = MD_DIALECT_GITHUB,
        .enter_block = enter_block,
        .leave_block = leave_block,
        .enter_span = enter_span,
        .leave_span = leave_span,
        .text = text_cb,
    };

    int rc = md_parse(text, (MD_SIZE)len, &parser, &b);
    flush_text(&b);
    free(b.pending);
    free(b.inline_text);
    if (rc != 0 || b.failed) { mdy_free(doc); return NULL; }

    identify_headings(doc, doc->root);
    return doc;
}
