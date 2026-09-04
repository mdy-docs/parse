/*
 * The block grammar, and the entry point.
 *
 * Measured before written: on the reference corpus, block structure is 62% of
 * the front end's time and inline the other 38%, with no single rule
 * dominating either half. So this is where a port has to start, and there is
 * no shortcut in which one hot rule is moved and the rest left alone.
 *
 * INDENTATION IS STRUCTURAL in MDY — every two columns is one level — which is
 * why everything here works on a pre-measured line array rather than on raw
 * text. Each rule needs the width before it needs the content.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

void mdy_options_default(mdy_options *out) {
    out->documents = 0;
    out->frontmatter = 1;
    out->autolink = 1;
    out->emphasis = 1;
    out->max_heading = 6;
    out->line_offset = 0;
}

/* ---- lines --------------------------------------------------------------- */

/** Split into lines, measuring indentation as we go. A tab is two columns,
 * matching the JavaScript's own "every two columns is one level". */
static mdy_line *split_lines(mdy_doc *doc, const char *text, size_t len, size_t *count) {
    size_t n = 1;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') n++;

    mdy_line *lines = mdy_alloc(&doc->arena, sizeof *lines * n);
    if (!lines) { *count = 0; return NULL; }

    size_t out = 0, start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n') continue;
        size_t end = i;
        if (end > start && text[end - 1] == '\r') end--;   /* CRLF */

        mdy_line *line = &lines[out];
        line->text = text + start;
        line->len = end - start;
        line->number = (uint32_t)out + 1 + doc->options.line_offset;

        size_t indent = 0, k = 0;
        while (k < line->len && (line->text[k] == ' ' || line->text[k] == '\t')) {
            indent += line->text[k] == '\t' ? 2 : 1;
            k++;
        }
        line->indent = indent;
        line->text += k;
        line->len -= k;
        line->blank = line->len == 0;

        out++;
        start = i + 1;
    }
    *count = out;
    return lines;
}

/* ---- small helpers ------------------------------------------------------- */

static int all_of(const mdy_line *l, char c) {
    if (l->len == 0) return 0;
    for (size_t i = 0; i < l->len; i++) if (l->text[i] != c) return 0;
    return 1;
}

static size_t run_of(const mdy_line *l, char c) {
    size_t n = 0;
    while (n < l->len && l->text[n] == c) n++;
    return n;
}

static void trim(const char **s, size_t *len) {
    while (*len && (**s == ' ' || **s == '\t')) { (*s)++; (*len)--; }
    while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t')) (*len)--;
}

/*
 * A heading's `id`, matching src/format.js's slugify: lowercase, runs of
 * anything that is not an ASCII letter or digit collapse to one hyphen, and
 * hyphens are trimmed from both ends.
 *
 * NOT Unicode-aware, and that is the JavaScript's behaviour rather than a
 * shortcut here: `Ašared` slugs to `a-ared` there too. Matching it exactly
 * matters more than improving it, because a heading's id is a URL that may
 * already be linked to.
 */
static char *slugify(mdy_doc *doc, const char *s, size_t len) {
    char *out = mdy_alloc(&doc->arena, len + 1);
    if (!out) return NULL;
    size_t o = 0;
    int pending_hyphen = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c >= 'A' && c <= 'Z') { c = (unsigned char)(c - 'A' + 'a'); keep = 1; }
        if (keep) {
            if (pending_hyphen && o) out[o++] = '-';
            pending_hyphen = 0;
            out[o++] = (char)c;
        } else {
            pending_hyphen = 1;
        }
    }
    out[o] = '\0';
    return out;
}

/* ---- list markers -------------------------------------------------------- */

/** How many characters of `l` are a list marker, and whether it is ordered.
 * `-`, `*`, `+` for bullets; `1.` or `1)` for ordered. 0 means not a list. */
static size_t list_marker(const mdy_line *l, int *ordered) {
    if (l->len < 2) return 0;
    char c = l->text[0];
    if ((c == '-' || c == '*' || c == '+') && (l->text[1] == ' ' || l->text[1] == '\t')) {
        *ordered = 0;
        return 2;
    }
    size_t digits = 0;
    while (digits < l->len && l->text[digits] >= '0' && l->text[digits] <= '9') digits++;
    if (digits > 0 && digits + 1 < l->len &&
        (l->text[digits] == '.' || l->text[digits] == ')') &&
        (l->text[digits + 1] == ' ' || l->text[digits + 1] == '\t')) {
        *ordered = 1;
        return digits + 2;
    }
    return 0;
}

/* ---- the block loop ------------------------------------------------------ */

/* Returns whether anything was added — an all-whitespace run produces no
 * paragraph, and must not produce a separator either. */
static int add_paragraph(mdy_doc *doc, mdy_node *parent, const char *joined, size_t len) {
    trim(&joined, &len);
    if (!len) return 0;
    mdy_node *p = mdy_new_element(doc, "p", 1);
    mdy_parse_inline(doc, p, joined, len);
    mdy_append(parent, p);
    return 1;
}

/*
 * Block children of an ELEMENT are separated by newline text nodes — `"\n" p
 * "\n" p "\n"` — and block children of the ROOT are not. That asymmetry is
 * the JavaScript's, and it is what makes the HTML it stringifies come out one
 * block per line inside a container while a bare document has no leading
 * newline. Lists follow the same rule, which is why the list code below emits
 * its own.
 */
static void separate(mdy_doc *doc, mdy_node *parent) {
    if (parent->type == MDY_ELEMENT) mdy_append(parent, mdy_new_text(doc, "\n", 1));
}

void mdy_parse_block(mdy_doc *doc, mdy_node *parent, const mdy_line *lines, size_t count) {
    size_t i = 0;
    int produced = 0;
    while (i < count) {
        const mdy_line *l = &lines[i];

        if (l->blank) { i++; continue; }

        /* --- thematic break: three or more of - * _ alone --- */
        if ((all_of(l, '-') || all_of(l, '*') || all_of(l, '_')) && l->len >= 3) {
            separate(doc, parent);
            mdy_append(parent, mdy_new_element(doc, "hr", 2));
            produced = 1;
            i++;
            continue;
        }

        /* --- heading: one `=` per level, trailing `=` are decoration --- */
        if (l->text[0] == '=') {
            size_t depth = run_of(l, '=');
            size_t max = doc->options.max_heading ? (size_t)doc->options.max_heading : 6;
            const char *body = l->text + depth;
            size_t body_len = l->len - depth;
            /* Trailing decoration, e.g. `== Title ==`. */
            while (body_len && body[body_len - 1] == '=') body_len--;
            trim(&body, &body_len);

            if (depth > max) depth = max;
            char tag[3] = { 'h', (char)('0' + (int)depth), '\0' };
            mdy_node *h = mdy_new_element(doc, tag, 2);
            char *id = slugify(doc, body, body_len);
            if (id && *id) mdy_set_string(doc, h, "id", id, strlen(id));
            mdy_parse_inline(doc, h, body, body_len);
            separate(doc, parent);
            mdy_append(parent, h);
            produced = 1;
            i++;
            continue;
        }

        /* --- fenced code: three or more backticks or tildes --- */
        if (l->len >= 3 && (l->text[0] == '`' || l->text[0] == '~')) {
            char fence = l->text[0];
            size_t width = run_of(l, fence);
            if (width >= 3) {
                const char *lang = l->text + width;
                size_t lang_len = l->len - width;
                trim(&lang, &lang_len);

                size_t j = i + 1;
                size_t start = j;
                while (j < count && !(run_of(&lines[j], fence) >= width && lines[j].len == run_of(&lines[j], fence))) j++;

                /* The content, verbatim, with its own newlines and the
                 * original indentation relative to the fence. */
                size_t total = 0;
                for (size_t k = start; k < j; k++) total += lines[k].indent + lines[k].len + 1;
                char *body = mdy_alloc(&doc->arena, total + 1);
                size_t o = 0;
                for (size_t k = start; k < j; k++) {
                    size_t strip = lines[k].indent > l->indent ? l->indent : lines[k].indent;
                    for (size_t s = strip; s < lines[k].indent; s++) body[o++] = ' ';
                    memcpy(body + o, lines[k].text, lines[k].len);
                    o += lines[k].len;
                    body[o++] = '\n';
                }
                body[o] = '\0';

                mdy_node *pre = mdy_new_element(doc, "pre", 3);
                mdy_node *code = mdy_new_element(doc, "code", 4);
                if (lang_len) {
                    char cls[64];
                    size_t n = lang_len < sizeof cls - 10 ? lang_len : sizeof cls - 10;
                    memcpy(cls, "language-", 9);
                    memcpy(cls + 9, lang, n);
                    cls[9 + n] = '\0';
                    mdy_add_class(doc, code, cls);
                }
                if (o) mdy_append(code, mdy_new_text(doc, body, o));
                mdy_append(pre, code);
                separate(doc, parent);
                mdy_append(parent, pre);
                produced = 1;
                i = j < count ? j + 1 : j;
                continue;
            }
        }

        /* --- lists --- */
        int ordered = 0;
        size_t marker = list_marker(l, &ordered);
        if (marker) {
            mdy_node *list = mdy_new_element(doc, ordered ? "ol" : "ul", 2);
            /* The JavaScript puts a newline text node before each item and one
             * after the last, so the HTML it stringifies is line-per-item.
             * Matching that exactly is the difference between an identical
             * tree and a nearly identical one. */
            mdy_append(list, mdy_new_text(doc, "\n", 1));
            while (i < count) {
                int this_ordered = 0;
                size_t width = lines[i].blank ? 0 : list_marker(&lines[i], &this_ordered);
                if (!width || this_ordered != ordered) break;

                const char *body = lines[i].text + width;
                size_t body_len = lines[i].len - width;
                trim(&body, &body_len);

                mdy_node *item = mdy_new_element(doc, "li", 2);
                mdy_parse_inline(doc, item, body, body_len);
                mdy_append(list, item);
                mdy_append(list, mdy_new_text(doc, "\n", 1));
                i++;
            }
            separate(doc, parent);
            mdy_append(parent, list);
            produced = 1;
            continue;
        }

        /* --- paragraph: adjacent non-blank lines joined with a space --- */
        size_t j = i;
        size_t total = 0;
        while (j < count && !lines[j].blank) {
            int ordered_here = 0;
            /* A line that starts another block ends this paragraph. */
            if (j > i && (lines[j].text[0] == '=' || list_marker(&lines[j], &ordered_here) ||
                          ((all_of(&lines[j], '-') || all_of(&lines[j], '*') || all_of(&lines[j], '_')) && lines[j].len >= 3))) break;
            total += lines[j].len + 1;
            j++;
        }
        char *joined = mdy_alloc(&doc->arena, total + 1);
        size_t o = 0;
        for (size_t k = i; k < j; k++) {
            if (k > i) joined[o++] = ' ';
            memcpy(joined + o, lines[k].text, lines[k].len);
            o += lines[k].len;
        }
        joined[o] = '\0';
        /* A run of only whitespace produces no paragraph, and so must produce
         * no separator either — which is why the emptiness is decided here
         * rather than inside add_paragraph. */
        const char *probe = joined;
        size_t probe_len = o;
        trim(&probe, &probe_len);
        if (probe_len) {
            separate(doc, parent);
            add_paragraph(doc, parent, joined, o);
            produced = 1;
        }
        i = j;
    }

    if (produced) separate(doc, parent);
}

/* ---- front matter and documents ------------------------------------------ */

/** How many lines the leading `+++` fence occupies, or 0 if there is none.
 * The YAML inside is not parsed here — mdy-docs hands it to a YAML reader —
 * but it must be recognised so it does not become content. */
static size_t front_matter_lines(const mdy_line *lines, size_t count) {
    if (count == 0 || lines[0].indent != 0) return 0;
    if (!(lines[0].len == 3 && memcmp(lines[0].text, "+++", 3) == 0)) return 0;
    for (size_t i = 1; i < count; i++) {
        if (lines[i].len == 3 && memcmp(lines[i].text, "+++", 3) == 0) return i + 1;
    }
    return 0;
}

mdy_doc *mdy_parse(const char *text, size_t len, const mdy_options *options) {
    mdy_doc *doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;
    if (options) doc->options = *options;
    else mdy_options_default(&doc->options);

    if (len == 0 && text) len = strlen(text);

    size_t count = 0;
    mdy_line *lines = split_lines(doc, text, len, &count);

    doc->root = mdy_alloc(&doc->arena, sizeof *doc->root);
    if (!doc->root) { mdy_free(doc); return NULL; }
    memset(doc->root, 0, sizeof *doc->root);
    doc->root->type = MDY_ROOT;

    size_t start = 0;
    if (doc->options.frontmatter) start = front_matter_lines(lines, count);

    if (!doc->options.documents) {
        mdy_parse_block(doc, doc->root, lines + start, count - start);
        return doc;
    }

    /* Each `---` at column 0 starts a new document, and each becomes an
     * <article> of its own — including its own front matter, which is why the
     * scan below re-runs the check per section rather than only at the top. */
    size_t from = start;
    for (size_t i = start; i <= count; i++) {
        int boundary = i == count ||
            (lines[i].indent == 0 && lines[i].len == 3 && memcmp(lines[i].text, "---", 3) == 0);
        if (!boundary) continue;

        mdy_node *article = mdy_new_element(doc, "article", 7);
        size_t section_start = from;
        if (doc->options.frontmatter) section_start += front_matter_lines(lines + from, i - from);
        mdy_parse_block(doc, article, lines + section_start, i - section_start);
        mdy_append(doc->root, article);
        from = i + 1;
    }
    return doc;
}

const mdy_node *mdy_root(const mdy_doc *doc) { return doc ? doc->root : NULL; }
size_t mdy_bytes(const mdy_doc *doc) { return doc ? doc->arena.total : 0; }

void mdy_free(mdy_doc *doc) {
    if (!doc) return;
    mdy_arena_free(&doc->arena);
    free(doc);
}
