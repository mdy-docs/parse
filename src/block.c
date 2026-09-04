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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static void trim(const char **s, size_t *len);

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

/* ---- the `<element` syntax ------------------------------------------------ */

/*
 * `<figure`, `<img src="…" width="250"`, `<figcaption>caption text`.
 *
 * A bare `<` is a `<div>`. The closing `>` is optional, and when it IS present
 * whatever follows on that line is the element's INLINE content — no paragraph
 * wrapper and no newline separators, which is how `<li>King fort` produces
 * `li("King fort")` rather than `li("\n" p("King fort") "\n")`.
 *
 * Otherwise the element's children are the lines indented under it, parsed as
 * blocks. Indentation is structural in MDY, so "under it" means strictly more
 * indented, and the element closes as soon as the indentation comes back.
 */
static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == ':';
}

/** How far the element opened on line `i` extends: the first line at or below
 * its own indentation. */
static size_t child_lines(const mdy_line *lines, size_t count, size_t i, size_t indent) {
    size_t j = i + 1;
    size_t last = j;
    while (j < count) {
        if (lines[j].blank) { j++; continue; }   /* a blank line does not close it */
        if (lines[j].indent <= indent) break;
        j++;
        last = j;
    }
    return last;
}

/**
 * Parse the attributes of an opener into `el`, stopping at the closing `>` if
 * there is one. `*content` is set to whatever follows that `>`, or NULL.
 */
static void parse_attributes(mdy_doc *doc, mdy_node *el, const char *tag,
                             const char *p, size_t len,
                             const char **content, size_t *content_len) {
    *content = NULL;
    *content_len = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i >= len) break;
        if (p[i] == '>') {
            *content = p + i + 1;
            *content_len = len - i - 1;
            return;
        }

        size_t start = i;
        while (i < len && is_name_char(p[i])) i++;
        if (i == start) { i++; continue; }        /* not a name — skip the byte */
        const char *name = p + start;
        size_t name_len = i - start;

        const char *value = NULL;
        size_t value_len = 0;
        int has_value = 0;
        size_t save = i;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i < len && p[i] == '=') {
            i++;
            while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
            if (i < len && (p[i] == '"' || p[i] == '\'')) {
                char quote = p[i++];
                size_t vstart = i;
                while (i < len && p[i] != quote) i++;
                value = p + vstart;
                value_len = i - vstart;
                if (i < len) i++;
            } else {
                size_t vstart = i;
                while (i < len && p[i] != ' ' && p[i] != '\t' && p[i] != '>') i++;
                value = p + vstart;
                value_len = i - vstart;
            }
            has_value = 1;
        } else {
            i = save;   /* a bare name — `hidden` */
        }

        if (!mdy_attr_allowed(tag, name, name_len)) continue;

        const char *hast = mdy_hast_name(doc, name, name_len);
        if (!has_value) { mdy_set_bool(doc, el, hast, 1); continue; }
        if (!mdy_protocol_allowed(hast, value, value_len)) continue;

        if (strcmp(hast, "className") == 0) {
            /* A class attribute is a space-separated list, and hast keeps it
             * as one. */
            size_t k = 0;
            while (k < value_len) {
                while (k < value_len && (value[k] == ' ' || value[k] == '\t')) k++;
                size_t cstart = k;
                while (k < value_len && value[k] != ' ' && value[k] != '\t') k++;
                if (k > cstart) {
                    char one[128];
                    size_t n = k - cstart < sizeof one - 1 ? k - cstart : sizeof one - 1;
                    memcpy(one, value + cstart, n);
                    one[n] = '\0';
                    mdy_add_class(doc, el, one);
                }
            }
        } else {
            mdy_set_string(doc, el, hast, value, value_len);
        }
    }
}

/** Consume an element opener at line `i`; returns the next line to read. */
static size_t parse_element(mdy_doc *doc, mdy_node *parent,
                            const mdy_line *lines, size_t count, size_t i) {
    const mdy_line *l = &lines[i];
    size_t n = 1;                       /* past the `<` */
    while (n < l->len && is_name_char(l->text[n])) n++;

    char tag[64];
    size_t tag_len = n - 1;
    if (tag_len == 0) { memcpy(tag, "div", 3); tag_len = 3; }
    else {
        if (tag_len > sizeof tag - 1) tag_len = sizeof tag - 1;
        for (size_t k = 0; k < tag_len; k++) {
            char c = l->text[1 + k];
            tag[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    tag[tag_len] = '\0';

    /* An element the schema does not allow produces nothing, and its children
     * go with it — which is what `sanitize` does. */
    if (!mdy_tag_allowed(tag)) return child_lines(lines, count, i, l->indent);

    mdy_node *el = mdy_new_element(doc, tag, tag_len);
    const char *content = NULL;
    size_t content_len = 0;
    parse_attributes(doc, el, tag, l->text + n, l->len - n, &content, &content_len);

    size_t end = child_lines(lines, count, i, l->indent);

    if (content) {
        /*
         * `<tag>text` — the rest of the line is INLINE content: no paragraph
         * wrapper and no separator before it. It may still have indented
         * children after it, and then both appear:
         *
         *     <th>Country      ->  th("Country" "\n" p("(present day)") "\n")
         *       (present day)
         *
         * Returning early here, as this did first, silently dropped every
         * such child — a whole column of a table, in the document that found
         * it.
         */
        trim(&content, &content_len);
        mdy_parse_inline(doc, el, content, content_len);
    }
    if (end > i + 1) mdy_parse_block(doc, el, lines + i + 1, end - (i + 1));
    separate(doc, parent);
    mdy_append(parent, el);
    return end;
}

/* ---- pipe tables ---------------------------------------------------------- */

/*
 * GitHub-flavoured tables: a header row of `|`-separated cells, a delimiter
 * row of the same width, then body rows.
 *
 * Alignment is expressed as a `style` attribute (`text-align: center`) rather
 * than the legacy `align` one — that is mdy-docs' `tableAlign: 'style'`
 * default, and the two are not interchangeable downstream.
 */
enum { ALIGN_NONE = 0, ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/** Split a row on `|`, ignoring escaped pipes and the optional outer ones.
 * Returns how many cells, writing their bounds into `starts`/`lens`. */
static size_t split_cells(const mdy_line *l, const char **starts, size_t *lens, size_t max) {
    const char *text = l->text;
    size_t len = l->len;
    /* Leading and trailing pipes are decoration. */
    size_t from = 0, to = len;
    while (from < to && (text[from] == ' ' || text[from] == '\t')) from++;
    if (from < to && text[from] == '|') from++;
    while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t')) to--;
    if (to > from && text[to - 1] == '|') to--;

    size_t count = 0, start = from;
    for (size_t i = from; i <= to && count < max; i++) {
        if (i < to && (text[i] != '|' || (i > from && text[i - 1] == '\\'))) continue;
        const char *cell = text + start;
        size_t cell_len = i - start;
        trim(&cell, &cell_len);
        starts[count] = cell;
        lens[count] = cell_len;
        count++;
        start = i + 1;
        if (i == to) break;
    }
    return count;
}

/** Is this a delimiter row, and what alignment does each cell ask for? */
static int delimiter_row(const mdy_line *l, int *align, size_t want) {
    const char *starts[64];
    size_t lens[64];
    size_t n = split_cells(l, starts, lens, 64);
    if (n != want || n == 0) return 0;

    for (size_t c = 0; c < n; c++) {
        const char *s = starts[c];
        size_t len = lens[c];
        if (len == 0) return 0;
        int left = s[0] == ':';
        int right = s[len - 1] == ':';
        size_t from = left ? 1 : 0, to = right ? len - 1 : len;
        if (to <= from) return 0;
        for (size_t k = from; k < to; k++) if (s[k] != '-') return 0;
        align[c] = left && right ? ALIGN_CENTER : left ? ALIGN_LEFT : right ? ALIGN_RIGHT : ALIGN_NONE;
    }
    return 1;
}

/** How many lines the table at `i` occupies, or 0 if this is not one. */
static size_t table_rows(const mdy_line *lines, size_t count, size_t i) {
    const char *starts[64];
    size_t lens[64];
    size_t want = split_cells(&lines[i], starts, lens, 64);
    if (want < 1) return 0;
    int align[64];
    if (!delimiter_row(&lines[i + 1], align, want)) return 0;

    size_t j = i + 2;
    while (j < count && !lines[j].blank && memchr(lines[j].text, '|', lines[j].len)) j++;
    return j - i;
}

static void add_cell(mdy_doc *doc, mdy_node *row, const char *tag, size_t tag_len,
                     const char *text, size_t len, int align) {
    mdy_node *cell = mdy_new_element(doc, tag, tag_len);
    if (align != ALIGN_NONE) {
        const char *style = align == ALIGN_LEFT ? "text-align: left"
                          : align == ALIGN_RIGHT ? "text-align: right"
                          : "text-align: center";
        mdy_set_string(doc, cell, "style", style, strlen(style));
    }
    mdy_parse_inline(doc, cell, text, len);
    mdy_append(row, mdy_new_text(doc, "\n", 1));
    mdy_append(row, cell);
}

static size_t parse_table(mdy_doc *doc, mdy_node *parent, const mdy_line *lines,
                          size_t i, size_t rows) {
    const char *starts[64];
    size_t lens[64];
    int align[64] = { 0 };
    size_t columns = split_cells(&lines[i], starts, lens, 64);
    delimiter_row(&lines[i + 1], align, columns);

    mdy_node *table = mdy_new_element(doc, "table", 5);
    mdy_append(table, mdy_new_text(doc, "\n", 1));

    mdy_node *thead = mdy_new_element(doc, "thead", 5);
    mdy_append(thead, mdy_new_text(doc, "\n", 1));
    mdy_node *head_row = mdy_new_element(doc, "tr", 2);
    for (size_t c = 0; c < columns; c++) add_cell(doc, head_row, "th", 2, starts[c], lens[c], align[c]);
    mdy_append(head_row, mdy_new_text(doc, "\n", 1));
    mdy_append(thead, head_row);
    mdy_append(thead, mdy_new_text(doc, "\n", 1));
    mdy_append(table, thead);
    mdy_append(table, mdy_new_text(doc, "\n", 1));

    if (rows > 2) {
        mdy_node *tbody = mdy_new_element(doc, "tbody", 5);
        mdy_append(tbody, mdy_new_text(doc, "\n", 1));
        for (size_t r = i + 2; r < i + rows; r++) {
            size_t n = split_cells(&lines[r], starts, lens, 64);
            mdy_node *tr = mdy_new_element(doc, "tr", 2);
            for (size_t c = 0; c < n && c < columns; c++)
                add_cell(doc, tr, "td", 2, starts[c], lens[c], align[c]);
            mdy_append(tr, mdy_new_text(doc, "\n", 1));
            mdy_append(tbody, tr);
            mdy_append(tbody, mdy_new_text(doc, "\n", 1));
        }
        mdy_append(table, tbody);
        mdy_append(table, mdy_new_text(doc, "\n", 1));
    }

    separate(doc, parent);
    mdy_append(parent, table);
    return i + rows;
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

void mdy_parse_block(mdy_doc *doc, mdy_node *parent, const mdy_line *lines, size_t count) {
    size_t i = 0;
    int produced = 0;

    /* The level this run of lines sits at. Anything more indented than the
     * first line belongs to a block of its own — see the div rule below. */
    size_t base = 0;
    for (size_t k = 0; k < count; k++) if (!lines[k].blank) { base = lines[k].indent; break; }

    while (i < count) {
        const mdy_line *l = &lines[i];

        if (l->blank) { i++; continue; }

        /*
         * NOT IMPLEMENTED: "lines indented under anything else get a <div> of
         * their own". The rule is real — `top` then an indented line does
         * produce `p("top") div(…)` — but a first attempt at it created 755
         * divs where the JavaScript makes 40, so the condition is narrower
         * than "more indented than this level". An element opener already
         * takes its own indented lines as children (parse_element), which is
         * where almost all indentation in a real document goes; what is left
         * is 38 divs across the whole corpus, and guessing at the rule is
         * worse than not having it. (void)base until it is worked out.
         */
        (void)base;

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
            if (id && *id) {
                /*
                 * Ids are unique across the document: a second `= Same` is
                 * `same-1`, a third `same-2`. mdy-docs calls this the heading
                 * state and shares it across a stream's documents, so a page
                 * built from several never has two headings a link cannot
                 * tell apart.
                 */
                char unique[256];
                size_t taken = 0;
                for (size_t k = 0; k < doc->heading_count; k++)
                    if (strcmp(doc->heading_ids[k], id) == 0) taken++;
                if (taken) {
                    snprintf(unique, sizeof unique, "%s-%zu", id, taken);
                } else {
                    snprintf(unique, sizeof unique, "%s", id);
                }
                if (doc->heading_count == doc->heading_cap) {
                    size_t grown = doc->heading_cap ? doc->heading_cap * 2 : 32;
                    const char **next = mdy_alloc(&doc->arena, sizeof(char *) * grown);
                    if (next) {
                        for (size_t k = 0; k < doc->heading_count; k++) next[k] = doc->heading_ids[k];
                        doc->heading_ids = next;
                        doc->heading_cap = grown;
                    }
                }
                if (doc->heading_count < doc->heading_cap) doc->heading_ids[doc->heading_count++] = id;
                mdy_set_string(doc, h, "id", unique, strlen(unique));
            }
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

        /* --- an element opener --- */
        if (l->text[0] == '<') {
            i = parse_element(doc, parent, lines, count, i);
            produced = 1;
            continue;
        }

        /* --- a pipe table --- */
        if (memchr(l->text, '|', l->len) && i + 1 < count) {
            size_t n = table_rows(lines, count, i);
            if (n) { i = parse_table(doc, parent, lines, i, n); produced = 1; continue; }
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

/* ---- footnote definitions ------------------------------------------------ */

/*
 * `[[ ^id ]]: text`, collected out of the line stream before anything is
 * parsed — a reference only becomes a footnote if its definition exists, so
 * this has to happen first.
 *
 * A collected line is blanked rather than removed. Blank is exactly what a
 * removed line means to every rule below: it separates blocks and produces
 * nothing.
 */
static void collect_definitions(mdy_doc *doc, mdy_line *lines, size_t count) {
    for (size_t i = 0; i < count; i++) {
        mdy_line *l = &lines[i];
        if (l->len < 8 || l->text[0] != '[' || l->text[1] != '[') continue;

        size_t k = 2;
        while (k < l->len && (l->text[k] == ' ' || l->text[k] == '\t')) k++;
        if (k >= l->len || l->text[k] != '^') continue;
        k++;

        size_t id_start = k;
        while (k < l->len && l->text[k] != ' ' && l->text[k] != '\t' && l->text[k] != ']') k++;
        size_t id_len = k - id_start;
        if (id_len == 0) continue;

        while (k < l->len && (l->text[k] == ' ' || l->text[k] == '\t')) k++;
        if (k + 2 >= l->len || l->text[k] != ']' || l->text[k + 1] != ']' || l->text[k + 2] != ':') continue;
        k += 3;

        const char *content = l->text + k;
        size_t content_len = l->len - k;
        trim(&content, &content_len);

        if (doc->note_count == doc->note_cap) {
            size_t grown = doc->note_cap ? doc->note_cap * 2 : 16;
            mdy_footnote *next = mdy_alloc(&doc->arena, sizeof *next * grown);
            if (!next) return;
            for (size_t n = 0; n < doc->note_count; n++) next[n] = doc->notes[n];
            doc->notes = next;
            doc->note_cap = grown;
        }
        mdy_footnote *note = &doc->notes[doc->note_count++];
        note->id = mdy_intern(&doc->arena, &doc->names, l->text + id_start, id_len);
        note->content = mdy_strdup_n(&doc->arena, content, content_len);
        note->content_len = content_len;
        note->number = 0;
        note->refs = 0;

        l->len = 0;
        l->blank = 1;
    }
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
        collect_definitions(doc, lines + start, count - start);
        mdy_parse_block(doc, doc->root, lines + start, count - start);
        mdy_footnote_section(doc, doc->root);
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

        /* Footnotes belong to their own document: each article collects,
         * numbers and lists its own, so two documents in one file cannot share
         * an id or a numbering run. */
        doc->note_count = 0;
        doc->next_number = 0;
        collect_definitions(doc, lines + section_start, i - section_start);
        mdy_parse_block(doc, article, lines + section_start, i - section_start);
        mdy_footnote_section(doc, article);
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
