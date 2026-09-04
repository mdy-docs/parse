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
#include <stdio.h>
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

/*
 * Where a URL begins. A scheme, or the `//host` of a protocol-relative one —
 * mdy-docs uses linkify-it, and `//host` is a case it catches that is easy to
 * miss.
 *
 * It is not a curiosity in this corpus: `//TravellersinEgypt.org//` is a
 * protocol-relative link, and reading it as an emphasis marker instead
 * produced 199 spurious <em> — the single largest remaining difference at the
 * time. A `//` followed by something host-shaped is a URL; a `//` followed by
 * prose is a marker.
 */
static int host_length(const char *p, size_t left) {
    size_t n = 0, dots = 0, label = 0;
    while (n < left) {
        char c = p[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') {
            label++;
            n++;
            continue;
        }
        if ((unsigned char)c >= 0x80) {
            /* A host may hold any letter — `//rꜥ-qdy.t//` is a link in this
             * corpus — so the whole character is consumed, not its lead byte. */
            uint32_t cp;
            size_t width = mdy_utf8_decode(p + n, left - n, &cp);
            if (!mdy_is_letter_or_number_cp(cp)) break;
            label++;
            n += width;
            continue;
        }
        if (c == '.' && label > 0) { dots++; label = 0; n++; continue; }
        break;
    }
    /* A host needs a dot and a final label that looks like a TLD. */
    if (dots == 0 || label < 2) return 0;
    return (int)n;
}

static int is_url_start(const char *p, size_t left) {
    if (left >= 8 && memcmp(p, "https://", 8) == 0) return 1;
    if (left >= 7 && memcmp(p, "http://", 7) == 0) return 1;
    if (left >= 5 && p[0] == '/' && p[1] == '/') return host_length(p + 2, left - 2) > 0;
    return 0;
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

/*
 * URL spans, found once before scanning starts.
 *
 * This is the mechanism the JavaScript uses (`findLinks`, then a check at
 * every marker) and it is not an optimisation — it is what stops the `//` in
 * `http://example.com` from opening an emphasis span. Without it a document
 * full of URLs grows emphasis it never asked for, which is exactly what
 * happened here: 200 spurious <em> across the reference corpus.
 */
#define MDY_MAX_URLS 512

typedef struct {
    size_t start, end;
} Span;

typedef struct {
    mdy_doc *doc;
    mdy_node *parent;    /* where finished nodes are appended */
    char *buf;           /* pending literal text */
    size_t len, cap;
    const Span *urls;    /* sorted, non-overlapping */
    size_t url_count;
    /*
     * Whether the next position starts a word. Tracked as STATE, not computed
     * from the previous byte, because what sets it is what the scanner just
     * did rather than what the text says: a marker or a wiki link leaves a
     * boundary behind, an em dash does not, and an ordinary character leaves
     * one only when it is whitespace. Emoticons are the only rule that reads
     * it, and getting it wrong makes faces out of the middle of URLs.
     */
    int at_boundary;
} Ctx;

/** Is `i` inside a URL that autolink will consume? */
static int inside_url(const Ctx *ctx, size_t i) {
    for (size_t k = 0; k < ctx->url_count; k++) {
        if (i >= ctx->urls[k].start && i < ctx->urls[k].end) return 1;
        if (ctx->urls[k].start > i) break;
    }
    return 0;
}

static void flush(Ctx *ctx) {
    if (ctx->len == 0) return;
    mdy_append(ctx->parent, mdy_new_text(ctx->doc, ctx->buf, ctx->len));
    ctx->len = 0;
}

static const char *slug_of(mdy_doc *doc, const char *s, size_t len, size_t *out_len);
static size_t wiki_link(Ctx *ctx, const char *p, size_t left);

/*
 * Replace every <a> in the subtree with its own children — RECURSIVELY, which
 * is what the JavaScript does: a link nested two deep inside a wiki link's
 * label is still a link inside a link, and still not a thing.
 */
static void unwrap_links(mdy_node *parent) {
    for (mdy_node *child = parent->first; child; child = child->next)
        if (child->type == MDY_ELEMENT) unwrap_links(child);

    mdy_node *first = NULL, *last = NULL;
    for (mdy_node *child = parent->first; child;) {
        mdy_node *next = child->next;
        if (child->type == MDY_ELEMENT && strcmp(child->tag, "a") == 0) {
            for (mdy_node *inner = child->first; inner;) {
                mdy_node *after = inner->next;
                inner->next = NULL;
                if (last) last->next = inner; else first = inner;
                last = inner;
                inner = after;
            }
        } else {
            child->next = NULL;
            if (last) last->next = child; else first = child;
            last = child;
        }
        child = next;
    }
    parent->first = first;
    parent->last = last;
}

/*
 * Append literal text to the pending run.
 *
 * The buffer is sized for GROWTH, not for the input's length, because several
 * rules here replace a short sequence with a longer one: `:)` is two bytes and
 * 😃 is four, `--` is two and an em dash is three. Sizing it to the input made
 * this drop whatever ran past the end — silently, which is how `ok :) yes`
 * came out as `ok 😃 ye`. Four times the input bounds every substitution in
 * this file with room to spare.
 */
static void push(Ctx *ctx, const char *s, size_t n) {
    if (ctx->len + n > ctx->cap) return;
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
            ctx->at_boundary = 0;
            i += 2;
            continue;
        }

        /* `[[ … ]]` outranks a marker, matching the JavaScript's order — so a
         * `//` inside a wiki link's target cannot pair with one outside it. */
        if (left >= 4 && p[0] == '[' && p[1] == '[') {
            size_t n = wiki_link(ctx, p, left);
            if (n) { i += n; continue; }   /* wiki_link sets at_boundary itself */
        }

        const Marker *m = inside_url(ctx, i) ? NULL : marker_at(p, left);
        if (m) {
            /*
             * A marker ALWAYS opens, and an unclosed one runs to the end of
             * the input — `a //b` is `a <em>b</em>`, not the literal text.
             *
             * Worth stating because the opposite is the intuitive guess and it
             * was the guess made here first: an assumption written into a test
             * without being checked against the JavaScript, which then passed
             * for the wrong reason and cost 196 spurious <em> across the
             * corpus before anyone looked.
             */
            size_t close = len;
            int found = 0;
            for (size_t j = i + 2; j + 1 < len; j++) {
                if (text[j] == '\\') { j++; continue; }
                if (inside_url(ctx, j)) continue;
                if (text[j] == m->seq[0] && text[j + 1] == m->seq[1]) { close = j; found = 1; break; }
            }
            {
                flush(ctx);
                mdy_node *el = mdy_new_element(ctx->doc, m->tag, strlen(m->tag));
                mdy_append(ctx->parent, el);

                const char *inner = text + i + 2;
                size_t inner_len = close - (i + 2);
                if (m->raw) {
                    /* Nothing inside is markup — that is what raw means. */
                    if (inner_len) mdy_append(el, mdy_new_text(ctx->doc, inner, inner_len));
                } else {
                    /*
                     * The nested scan works on a slice, so the outer URL spans
                     * — whose offsets are into the whole string — do not apply.
                     * It finds its own; that is what mdy_parse_inline does.
                     */
                    mdy_parse_inline(ctx->doc, el, inner, inner_len);
                }
                ctx->at_boundary = 1;
                i = found ? close + 2 : len;
                continue;
            }
        }

        /*
         * Emoji, then the typographic replacements, in that order — the
         * JavaScript checks emoji first and it matters: `:-->` would otherwise
         * lose its head to the arrow rule.
         *
         * Neither reaches inside a raw span, which is handled by the marker
         * branch copying that text verbatim rather than scanning it.
         */
        {
            size_t used = 0;
            const char *found = mdy_match_emoji(p, left, ctx->at_boundary, &used);
            if (found) {
                push(ctx, found, strlen(found));
                ctx->at_boundary = 1;
                i += used;
                continue;
            }
        }

        /*
         * `--` is an em dash and `---` is not: three hyphens stay literal,
         * because a run of them is a thematic break's business. That means
         * looking BEHIND as well as ahead, or the second and third hyphens of
         * a run pair up into one.
         *
         * Each of these leaves at_boundary false — what follows punctuation is
         * no more the start of a word than what follows a full stop.
         */
        {
            const char *drawn = NULL;
            size_t used = 0;
            if (left >= 4 && memcmp(p, "<-->", 4) == 0)      { drawn = "\xe2\x86\x94"; used = 4; }
            else if (left >= 4 && memcmp(p, "<==>", 4) == 0) { drawn = "\xe2\x87\x94"; used = 4; }
            else if (left >= 3 && memcmp(p, "-->", 3) == 0)  { drawn = "\xe2\x86\x92"; used = 3; }
            else if (left >= 3 && memcmp(p, "<--", 3) == 0)  { drawn = "\xe2\x86\x90"; used = 3; }
            else if (left >= 3 && memcmp(p, "==>", 3) == 0)  { drawn = "\xe2\x87\x92"; used = 3; }
            else if (left >= 3 && memcmp(p, "<==", 3) == 0)  { drawn = "\xe2\x87\x90"; used = 3; }
            else if (left >= 3 && p[0] == '.' && p[1] == '.' && p[2] == '.') { drawn = "\xe2\x80\xa6"; used = 3; }
            else if (left >= 2 && p[0] == '-' && p[1] == '-' &&
                     !(left >= 3 && p[2] == '-') && !(i > 0 && text[i - 1] == '-')) {
                drawn = "\xe2\x80\x94";
                used = 2;
            }
            if (drawn) {
                push(ctx, drawn, 3);
                ctx->at_boundary = 0;
                i += used;
                continue;
            }
        }

        if ((p[0] == '#' || p[0] == '@') && left > 1) {
            /* `#tag` and `@user`. The label keeps its case — `#Tag-One` links
             * to `/tags/Tag-One` — which is the one thing about these that is
             * easy to get wrong, since almost everything else here lowercases. */
            size_t n = 1;
            while (n < left) {
                char c = p[n];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_') n++;
                else break;
            }
            if (n > 1) {
                flush(ctx);
                mdy_node *a = mdy_new_element(ctx->doc, "a", 1);
                char href[256];
                snprintf(href, sizeof href, "%s%.*s", p[0] == '#' ? "/tags/" : "/users/",
                         (int)(n - 1), p + 1);
                mdy_set_string(ctx->doc, a, "href", href, strlen(href));
                mdy_append(a, mdy_new_text(ctx->doc, p, n));
                mdy_append(ctx->parent, a);
                ctx->at_boundary = 0;
                i += n;
                continue;
            }
        }

        if (ctx->doc->options.autolink && is_url_start(p, left)) {
            size_t n = url_length(p, left);
            if (n > 0) {
                flush(ctx);
                mdy_node *a = mdy_new_element(ctx->doc, "a", 1);
                mdy_set_string(ctx->doc, a, "href", p, n);
                mdy_append(a, mdy_new_text(ctx->doc, p, n));
                mdy_append(ctx->parent, a);
                ctx->at_boundary = 1;
                i += n;
                continue;
            }
        }

        /* Only whitespace leaves a boundary behind — a bracket does not, and
         * treating one as if it did made emoticons out of ordinary prose. */
        ctx->at_boundary = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
        push(ctx, p, 1);
        i++;
    }
    flush(ctx);
}

void mdy_parse_inline(mdy_doc *doc, mdy_node *parent, const char *text, size_t len) {
    if (len == 0) return;

    Span urls[MDY_MAX_URLS];
    size_t url_count = 0;
    if (doc->options.autolink) {
        for (size_t i = 0; i < len && url_count < MDY_MAX_URLS; i++) {
            if (!is_url_start(text + i, len - i)) continue;
            size_t n = url_length(text + i, len - i);
            if (n == 0) continue;
            urls[url_count].start = i;
            urls[url_count].end = i + n;
            url_count++;
            i += n - 1;
        }
    }

    size_t cap = len * 4 + 8;   /* see push() */
    Ctx ctx = { .doc = doc, .parent = parent, .len = 0, .cap = cap,
                .urls = urls, .url_count = url_count, .at_boundary = 1 };
    ctx.buf = mdy_alloc(&doc->arena, cap);
    if (!ctx.buf) return;
    scan(&ctx, text, len);
}

/*
 * Where a bare `[[ label ]]` points — mdy-docs' defaultResolve, which is NOT
 * slugify and the difference matters:
 *
 *     defaultResolve   lowercase, whitespace to `-`, then DELETE anything
 *                      outside [letters, numbers, - / . _ #]
 *     slugify          lowercase, then anything outside [a-z0-9] becomes `-`
 *
 * So `Umm el-Qa'ab` resolves to `umm-el-qaab` — the apostrophe vanishes rather
 * than becoming a hyphen — and `Edward R. Ayrton` keeps its full stop. Getting
 * these confused produced `umm-el-qa-ab` and `edward-r-ayrton`, which are
 * links to nowhere.
 *
 * Letters and numbers are Unicode there. A UTF-8 continuation byte is kept
 * here on the same basis: a multi-byte character is a letter far more often
 * than not, and treating one as punctuation would mangle every non-English
 * label in the corpus.
 */
static const char *slug_of(mdy_doc *doc, const char *s, size_t len, size_t *out_len) {
    /* Lowercasing can grow a character (ẞ is one byte wider lowered), so the
     * buffer allows for it rather than assuming the output is no longer than
     * the input. */
    char *out = mdy_alloc(&doc->arena, len * 2 + 2);
    if (!out) { *out_len = 0; return NULL; }

    size_t o = 0;
    int was_space = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t width = mdy_utf8_decode(s + i, len - i, &cp);
        i += width;

        int space = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0 ||
                    (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
                    cp == 0x202F || cp == 0x205F || cp == 0x3000;
        if (space) {
            /*
             * `\s+` becomes ONE hyphen — a run of whitespace, not "whitespace
             * with a hyphen already behind it". The two differ whenever a
             * deleted character sits between two spaces: `plain | label`
             * resolves to `plain--label`, because the replacement happens
             * before the deletion and leaves two runs, not one.
             */
            if (!was_space) out[o++] = '-';
            was_space = 1;
            continue;
        }
        was_space = 0;

        if (cp == '-' || cp == '/' || cp == '.' || cp == '_' || cp == '#' ||
            mdy_is_letter_or_number_cp(cp)) {
            o += mdy_utf8_encode(mdy_lower_cp(cp), out + o);
            continue;
        }
        /* Everything else is deleted — the WHOLE character, which decoding
         * rather than walking bytes is what guarantees. */
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
    if (body[0] == '^') {
        /*
         * A footnote reference — but only if the definition exists. Without
         * one it stays literal text, which is what the JavaScript does and
         * why definitions are collected before any of this runs.
         */
        mdy_footnote *note = mdy_footnote_find(ctx->doc, body + 1, body_len - 1);
        if (!note) return 0;
        int n = mdy_footnote_reference(ctx->doc, note);

        char buf[256];
        flush(ctx);
        mdy_node *sup = mdy_new_element(ctx->doc, "sup", 3);
        mdy_node *a = mdy_new_element(ctx->doc, "a", 1);

        snprintf(buf, sizeof buf, "#user-content-fn-%s", note->id);
        mdy_set_string(ctx->doc, a, "href", buf, strlen(buf));
        if (n > 1) snprintf(buf, sizeof buf, "user-content-fnref-%s-%d", note->id, n);
        else snprintf(buf, sizeof buf, "user-content-fnref-%s", note->id);
        mdy_set_string(ctx->doc, a, "id", buf, strlen(buf));
        mdy_set_bool(ctx->doc, a, "dataFootnoteRef", 1);
        mdy_set_string(ctx->doc, a, "ariaDescribedBy", "footnote-label", 14);

        snprintf(buf, sizeof buf, "%d", note->number);
        mdy_append(a, mdy_new_text(ctx->doc, buf, strlen(buf)));
        mdy_append(sup, a);
        mdy_append(ctx->parent, sup);
        ctx->at_boundary = 0;
        return close + 2;
    }

    const char *label = body;
    size_t label_len = body_len;
    const char *target = NULL;
    size_t target_len = 0;

    for (size_t j = 0; j < body_len; j++) {
        /* `\|` is a literal pipe in the label, not the separator. A corpus of
         * citations is full of them — "…desert \| Aeon Essays" — and taking
         * the first pipe regardless made the href the whole rest of the
         * construct, which then swallowed every link after it on that line. */
        if (body[j] == '\\') { j++; continue; }
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
        /* Resolved from the label's TEXT, which is the label with its escapes
         * taken off — `plain \| label` resolves as `plain | label` does. */
        char *plain = mdy_alloc(&ctx->doc->arena, label_len + 1);
        size_t plain_len = 0;
        for (size_t j = 0; j < label_len; j++) {
            if (label[j] == '\\' && j + 1 < label_len) j++;
            plain[plain_len++] = label[j];
        }
        plain[plain_len] = '\0';

        size_t n = 0;
        target = slug_of(ctx->doc, plain, plain_len, &n);
        target_len = n;
        if (!target || n == 0) return 0;
    }

    flush(ctx);
    mdy_node *a = mdy_new_element(ctx->doc, "a", 1);
    mdy_set_string(ctx->doc, a, "href", target, target_len);

    /*
     * The label is content in its own right, parsed with autolink ON so a URL
     * inside it survives the `//` marker — and then UNWRAPPED, because an <a>
     * inside an <a> is not a thing. Skipping the unwrap left 150 nested links
     * across the corpus, every one of them with a plausible href, which is why
     * counting nodes found it and checking hrefs did not.
     */
    mdy_parse_inline(ctx->doc, a, label, label_len);
    unwrap_links(a);

    mdy_append(ctx->parent, a);
    ctx->at_boundary = 1;
    return close + 2;
}
