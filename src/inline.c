/*
 * Inline parsing.
 *
 * MDY's inline model is TOGGLING, not nesting, and that is the single most
 * important thing about this file. A marker sequence opens a span; the next
 * occurrence of the same sequence closes it. There is no left-flanking /
 * right-flanking analysis, no delimiter stack, none of CommonMark's emphasis
 * machinery — which is why a C implementation of this is a few hundred lines
 * rather than a few thousand.
 *
 * The markers are all two characters, all doubled, and there are nine of them.
 * A single `*` is literal text: `a *b* c` is three words, `a **b** c` has a
 * <strong>. That is a deliberate divergence from markdown and it is what makes
 * scanning cheap — two bytes decide, with no lookbehind.
 */
#include <string.h>

#include "internal.h"

typedef struct {
    const char *seq;    /* two characters, always */
    const char *tag;
    int raw;            /* nothing inside is markup until the closer */
} Marker;

/*
 * defaultMarkers from ../../mdy-docs/src/parse/markers.js, in that order.
 * Longest-first does not arise: every sequence is exactly two characters and
 * none prefixes another.
 */
static const Marker MARKERS[] = {
    { "!!", "strong", 0 },
    { "**", "strong", 0 },
    { "//", "em",     0 },
    { "__", "u",      0 },
    { "~~", "del",    0 },
    { "??", "mark",   0 },
    { "^^", "sup",    0 },
    { ",,", "sub",    0 },
    { "``", "code",   1 },
};
static const size_t MARKER_COUNT = sizeof MARKERS / sizeof MARKERS[0];

static const Marker *marker_at(const char *p, size_t left) {
    if (left < 2) return NULL;
    for (size_t i = 0; i < MARKER_COUNT; i++) {
        if (p[0] == MARKERS[i].seq[0] && p[1] == MARKERS[i].seq[1]) return &MARKERS[i];
    }
    return NULL;
}

/* ---- autolink ------------------------------------------------------------ */

static int is_url_start(const char *p, size_t left) {
    return (left >= 8 && memcmp(p, "https://", 8) == 0) ||
           (left >= 7 && memcmp(p, "http://", 7) == 0);
}

/*
 * Where a bare URL ends. Trailing punctuation is excluded, because a URL at
 * the end of a sentence is followed by a full stop that is not part of it —
 * and a closing bracket only counts if an opening one was inside.
 */
static size_t url_length(const char *p, size_t left) {
    size_t n = 0;
    while (n < left) {
        unsigned char c = (unsigned char)p[n];
        if (c <= ' ' || c == '<' || c == '>' || c == '"') break;
        n++;
    }
    while (n > 0) {
        char c = p[n - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') { n--; continue; }
        if (c == ')') {
            int opens = 0;
            for (size_t i = 0; i < n; i++) { if (p[i] == '(') opens++; else if (p[i] == ')') opens--; }
            if (opens < 0) { n--; continue; }
        }
        break;
    }
    return n;
}

/* ---- wiki links ---------------------------------------------------------- */

/*
 * `[[ label ]]` and `[[ label | url ]]`.
 *
 * A bare label resolves to its own slug — `[[ Seti I ]]` links to `seti-i` —
 * which is mdy-docs' defaultResolve. A `|` splits label from target and the
 * spaces around it are optional.
 *
 * A label beginning with `^` is NOT this: it is a footnote reference, and
 * whether it becomes one depends on a definition appearing elsewhere in the
 * document. That is a whole-document pass rather than an inline rule, so this
 * leaves such spans alone — which is also what the JavaScript does when the
 * definition is missing.
 */
/* ---- the scanner --------------------------------------------------------- */

typedef struct {
    mdy_doc *doc;
    mdy_node *parent;    /* where finished nodes are appended */
    char *buf;           /* pending literal text */
    size_t len, cap;
} Ctx;

static void flush(Ctx *ctx) {
    if (ctx->len == 0) return;
    mdy_append(ctx->parent, mdy_new_text(ctx->doc, ctx->buf, ctx->len));
    ctx->len = 0;
}

static const char *slug_of(mdy_doc *doc, const char *s, size_t len, size_t *out_len);
static size_t wiki_link(Ctx *ctx, const char *p, size_t left);

static void push(Ctx *ctx, const char *s, size_t n) {
    if (ctx->len + n > ctx->cap) return;   /* the buffer is the whole input's size */
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
}

/*
 * Scan `text` into `parent`. Recursion is one level per open marker, and a
 * marker that never closes degrades to literal text — checked by looking ahead
 * for the closer before opening anything, which is what makes an unmatched
 * `**` come out as two asterisks rather than swallowing the rest of the line.
 */
static void scan(Ctx *ctx, const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        const char *p = text + i;
        size_t left = len - i;

        /* An escape makes the next character literal, and consumes the
         * backslash — `a \*b\* c` is `a *b* c`. */
        if (*p == '\\' && left > 1) {
            push(ctx, p + 1, 1);
            i += 2;
            continue;
        }

        const Marker *m = marker_at(p, left);
        if (m) {
            /* Only open if this marker closes again later; otherwise it is
             * text. */
            size_t close = 0;
            int found = 0;
            for (size_t j = i + 2; j + 1 < len; j++) {
                if (text[j] == '\\') { j++; continue; }
                if (text[j] == m->seq[0] && text[j + 1] == m->seq[1]) { close = j; found = 1; break; }
            }
            if (found) {
                flush(ctx);
                mdy_node *el = mdy_new_element(ctx->doc, m->tag, strlen(m->tag));
                mdy_append(ctx->parent, el);

                const char *inner = text + i + 2;
                size_t inner_len = close - (i + 2);
                if (m->raw) {
                    /* Nothing inside is markup — that is what raw means. */
                    if (inner_len) mdy_append(el, mdy_new_text(ctx->doc, inner, inner_len));
                } else {
                    Ctx nested = *ctx;
                    nested.parent = el;
                    nested.len = 0;
                    nested.buf = ctx->buf + ctx->len;      /* the tail is unused */
                    nested.cap = ctx->cap - ctx->len;
                    scan(&nested, inner, inner_len);
                    flush(&nested);
                }
                i = close + 2;
                continue;
            }
        }

        if (left >= 4 && p[0] == '[' && p[1] == '[') {
            size_t n = wiki_link(ctx, p, left);
            if (n) { i += n; continue; }
        }

        if (ctx->doc->options.autolink && is_url_start(p, left)) {
            size_t n = url_length(p, left);
            if (n > 0) {
                flush(ctx);
                mdy_node *a = mdy_new_element(ctx->doc, "a", 1);
                mdy_set_string(ctx->doc, a, "href", p, n);
                mdy_append(a, mdy_new_text(ctx->doc, p, n));
                mdy_append(ctx->parent, a);
                i += n;
                continue;
            }
        }

        push(ctx, p, 1);
        i++;
    }
    flush(ctx);
}

void mdy_parse_inline(mdy_doc *doc, mdy_node *parent, const char *text, size_t len) {
    if (len == 0) return;
    Ctx ctx = { .doc = doc, .parent = parent, .len = 0, .cap = len + 1 };
    ctx.buf = mdy_alloc(&doc->arena, len + 1);
    if (!ctx.buf) return;
    scan(&ctx, text, len);
}

/*
 * The slug mdy-docs' defaultResolve uses for a bare label: the same rule as a
 * heading id. Kept here rather than shared with block.c because the two could
 * diverge — the JavaScript reaches them through different options — and a
 * shared helper would hide that if they ever did.
 */
static const char *slug_of(mdy_doc *doc, const char *s, size_t len, size_t *out_len) {
    char *out = mdy_alloc(&doc->arena, len + 1);
    if (!out) { *out_len = 0; return NULL; }
    size_t o = 0;
    int pending = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c >= 'A' && c <= 'Z') { c = (unsigned char)(c - 'A' + 'a'); keep = 1; }
        if (keep) {
            if (pending && o) out[o++] = '-';
            pending = 0;
            out[o++] = (char)c;
        } else {
            pending = 1;
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

static void cut(const char **s, size_t *len) {
    while (*len && (**s == ' ' || **s == '\t')) { (*s)++; (*len)--; }
    while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t')) (*len)--;
}

/** Consume `[[ … ]]` at `p`, emitting a link. Returns bytes consumed, or 0 to
 * leave it as text. */
static size_t wiki_link(Ctx *ctx, const char *p, size_t left) {
    size_t close = 0;
    int found = 0;
    for (size_t j = 2; j + 1 < left; j++) {
        if (p[j] == ']' && p[j + 1] == ']') { close = j; found = 1; break; }
        if (p[j] == '\n') return 0;    /* a wiki link does not cross a line */
    }
    if (!found) return 0;

    const char *body = p + 2;
    size_t body_len = close - 2;
    cut(&body, &body_len);
    if (body_len == 0) return 0;
    if (body[0] == '^') return 0;      /* a footnote reference — see above */

    const char *label = body;
    size_t label_len = body_len;
    const char *target = NULL;
    size_t target_len = 0;

    for (size_t j = 0; j < body_len; j++) {
        if (body[j] != '|') continue;
        label = body;
        label_len = j;
        target = body + j + 1;
        target_len = body_len - j - 1;
        cut(&label, &label_len);
        cut(&target, &target_len);
        break;
    }
    if (label_len == 0) return 0;

    if (!target) {
        size_t n = 0;
        target = slug_of(ctx->doc, label, label_len, &n);
        target_len = n;
        if (!target || n == 0) return 0;
    }

    flush(ctx);
    mdy_node *a = mdy_new_element(ctx->doc, "a", 1);
    mdy_set_string(ctx->doc, a, "href", target, target_len);
    mdy_parse_inline(ctx->doc, a, label, label_len);
    mdy_append(ctx->parent, a);
    return close + 2;
}
