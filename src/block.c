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
    out->positions = 1;
    out->sanitize = 1;
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
        /* Before the indent is stripped: a position's end column counts from
         * the start of the line, indentation and all. */
        line->units = (uint32_t)mdy_utf16_length(text + start, end - start);

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

void mdy_set_position(mdy_node *node, const mdy_line *lines, size_t from, size_t to) {
    if (!node) return;
    node->line = lines[from].number;
    node->column = 1;
    node->end_line = lines[to].number;
    node->end_column = lines[to].units + 1;
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

/* JavaScript's notion of whitespace, not C's — see mdy_trim. */
static void trim(const char **s, size_t *len) { mdy_trim(s, len); }

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
    for (size_t i = 0; i < len;) {
        /*
         * slugify is `[^a-z0-9]+` -> `-` AFTER a lowercase, so it is genuinely
         * ASCII-only — `Ašared` slugs to `a-ared` in the JavaScript too, and
         * matching that matters more than improving it, because a heading's id
         * is a URL somebody may already have linked to.
         *
         * Whole characters still, so a multi-byte one collapses to ONE hyphen
         * rather than one per byte.
         */
        uint32_t cp;
        size_t width = mdy_utf8_decode(s + i, len - i, &cp);
        i += width;

        uint32_t lowered = cp < 0x80 ? mdy_lower_cp(cp) : cp;
        if ((lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9')) {
            if (pending_hyphen && o) out[o++] = '-';
            pending_hyphen = 0;
            out[o++] = (char)lowered;
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

/*
 * A heading's `id`, unique across the document: a second `= Same` is `same-1`,
 * a third `same-2`. mdy-docs calls this the heading state and shares it across
 * a stream's documents on purpose — two articles on one page must not both own
 * `#introduction`.
 */
/** Every text descendant, concatenated — a node's rendered content. */
static size_t node_text(const mdy_node *n, char *out, size_t cap, size_t o) {
    if (n->type == MDY_TEXT) {
        size_t len = n->text ? strlen(n->text) : 0;
        if (o + len > cap) len = cap > o ? cap - o : 0;
        memcpy(out + o, n->text, len);
        return o + len;
    }
    for (const mdy_node *c = n->first; c; c = c->next) o = node_text(c, out, cap, o);
    return o;
}

static void set_heading_id(mdy_doc *doc, mdy_node *h, const char *text, size_t len) {
    /*
     * The SAME slug a `[[ label ]]` resolves to, not slugify — mdy-docs says
     * why in heading.js: "a heading and a link written from the same text
     * agree". The two differ wherever punctuation appears, because resolve
     * DELETES what slugify hyphenates: `King's List` is `kings-list` under one
     * and `king-s-list` under the other, and only the first is a link that
     * works.
     */
    size_t id_len = 0;
    const char *id = mdy_resolve_slug(doc, text, len, &id_len);
    if (!id || !*id) return;

    size_t taken = 0;
    for (size_t k = 0; k < doc->heading_count; k++)
        if (strcmp(doc->heading_ids[k], id) == 0) taken++;

    char unique[256];
    if (taken) snprintf(unique, sizeof unique, "%s-%zu", id, taken);
    else snprintf(unique, sizeof unique, "%s", id);
    (void)id_len;

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

/* ---- list markers -------------------------------------------------------- */

/** How many characters of `l` are a list marker, and whether it is ordered.
 * `-`, `*`, `+` for bullets; `1.` or `1)` for ordered. 0 means not a list. */
/*
 * A marker must be followed by a space, a tab, or THE END OF THE LINE — that
 * last one is not a nicety. `1931.` alone on a line is an ordered list whose
 * item content is on the following lines, and reading it as prose instead put
 * it inside the paragraph above and shifted every footnote number after it.
 * `1931.x` is not a marker, because something that is not a space follows.
 */
/** The number an ordered marker carries, or 0 for a bullet. Only meaningful
 * when list_marker returned non-zero. */
static long marker_number(const mdy_line *l) {
    long value = 0;
    size_t i = 0;
    while (i < l->len && l->text[i] >= '0' && l->text[i] <= '9') {
        value = value * 10 + (l->text[i] - '0');
        i++;
    }
    return i ? value : -1;
}

static size_t list_marker(const mdy_line *l, int *ordered) {
    if (l->len < 1) return 0;
    char c = l->text[0];
    if (c == '-' || c == '*' || c == '+') {
        if (l->len == 1) { *ordered = 0; return 1; }
        if (l->text[1] == ' ' || l->text[1] == '\t') { *ordered = 0; return 2; }
        return 0;
    }
    size_t digits = 0;
    while (digits < l->len && l->text[digits] >= '0' && l->text[digits] <= '9') digits++;
    if (digits == 0 || digits >= l->len) return 0;
    if (l->text[digits] != '.' && l->text[digits] != ')') return 0;
    if (digits + 1 == l->len) { *ordered = 1; return digits + 1; }   /* end of line */
    if (l->text[digits + 1] == ' ' || l->text[digits + 1] == '\t') { *ordered = 1; return digits + 2; }
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
/*
 * The name patterns from ../../src/parse/html.js: a tag is a letter then
 * letters, digits and hyphens; an attribute is a letter, underscore or colon
 * then letters, digits, dots, underscores, colons and hyphens.
 *
 * Both must START with a letter, and that is the part worth having: it is what
 * stops the `--` of `<!-- a comment -->` from being read as an attribute
 * called `--`.
 */
static int is_tag_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_tag_char(char c) {
    return is_tag_start(c) || (c >= '0' && c <= '9') || c == '-';
}
static int is_attr_start(char c) { return is_tag_start(c) || c == '_' || c == ':'; }
static int is_attr_char(char c) {
    return is_tag_start(c) || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == ':' || c == '-';
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
        if (is_attr_start(p[i])) {
            i++;
            while (i < len && is_attr_char(p[i])) i++;
        }
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

        if (doc->options.sanitize && !mdy_attr_allowed(tag, name, name_len)) continue;

        const char *hast = mdy_hast_name(doc, name, name_len);
        if (!has_value) { mdy_set_bool(doc, el, hast, 1); continue; }
        if (doc->options.sanitize && !mdy_protocol_allowed(hast, value, value_len)) continue;

        if (strcmp(hast, "className") == 0) {
            /* A class attribute is a space-separated list, and hast keeps it
             * as one. A repeated attribute replaces — properties is an object. */
            mdy_clear_class(doc, el);
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
/** Case-insensitive prefix test. */
static int starts_ci(const mdy_line *l, size_t at, const char *want) {
    size_t n = strlen(want);
    if (at + n > l->len) return 0;
    for (size_t k = 0; k < n; k++) {
        char c = l->text[at + k];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != want[k]) return 0;
    }
    return 1;
}

/*
 * A fence opener, as src/parse/fence.js reads one: three or more backticks or
 * tildes, then whatever is written after them.
 *
 * Two details that a "three backticks" test misses, and both were wrong here:
 *
 *   - The LANGUAGE is the first word of the info, not the whole of it. An info
 *     of `js title="a b"` names the language `js`, and taking the lot put the
 *     title inside the class attribute.
 *   - A backtick fence may not carry a backtick in its info. Without that
 *     rule a line of prose holding a stray pair of backticks opens a code
 *     block that swallows the rest of the document.
 *
 * Returns the marker width, or 0 when the line is not an opener.
 */
static size_t fence_opener(const mdy_line *l, char *marker,
                           const char **lang, size_t *lang_len) {
    if (l->len < 3 || (l->text[0] != '`' && l->text[0] != '~')) return 0;
    char c = l->text[0];
    size_t width = 0;
    while (width < l->len && l->text[width] == c) width++;
    if (width < 3) return 0;

    const char *info = l->text + width;
    size_t info_len = l->len - width;
    trim(&info, &info_len);
    if (c == '`' && memchr(info, '`', info_len)) return 0;

    /* `info.trim().split(/\s+/)[0]` — the first word and no more. */
    size_t n = 0;
    while (n < info_len && info[n] != ' ' && info[n] != '\t') n++;

    *marker = c;
    *lang = info;
    *lang_len = n;
    return width;
}

/** A line that closes a fence: the same character, at least as many, and
 * nothing else once trailing whitespace is off. */
static int closes_fence(const mdy_line *l, char marker, size_t width) {
    const char *t = l->text;
    size_t len = l->len;
    mdy_trim_end(&t, &len);
    if (len < width) return 0;
    for (size_t k = 0; k < len; k++) if (t[k] != marker) return 0;
    return 1;
}

/*
 * Take a document's comments out.
 *
 * A `#` with nothing against it — a space, or the end of the line. What
 * follows says a comment was meant, because a word against the `#` makes a
 * tag instead; which also makes `# Title`, a Markdown heading, a comment
 * here. MDY writes headings with `=`.
 *
 * A comment is a WHOLE LINE and leaves nothing behind, so the markup around
 * it closes over the gap: the lines either side are as adjacent as they were,
 * and the indentation the block parser reads is the content's alone. Each
 * surviving line keeps its own `number`, so positions still point at the
 * source the comment came out of.
 *
 * A fenced block is the one place they are content: `# ` opens a comment in
 * half the languages a code sample might be written in, and one that quietly
 * lost them would be worse than useless. So the fences are found first and
 * whatever they hold is left exactly as it is.
 */
static int is_comment_line(const mdy_line *l) {
    if (l->len == 0 || l->text[0] != '#') return 0;
    return l->len == 1 || l->text[1] == ' ' || l->text[1] == '\t';
}

static void strip_comments(mdy_line *lines, size_t *count) {
    size_t n = *count;
    size_t any = 0;
    for (size_t i = 0; i < n; i++) if (is_comment_line(&lines[i])) { any = 1; break; }
    if (!any) return;                    /* left exactly as they are */

    char marker = 0;
    size_t width = 0;
    size_t fence_indent = 0;
    int in_fence = 0;
    size_t o = 0;

    for (size_t i = 0; i < n; i++) {
        if (in_fence) {
            if (closes_fence(&lines[i], marker, width)) {
                /* The closer belongs to the block and cannot open another. */
                in_fence = 0;
                lines[o++] = lines[i];
                continue;
            }
            /* An unclosed fence runs to the end of whatever encloses it,
             * which is wherever the indentation comes back out. */
            if (lines[i].len && lines[i].indent < fence_indent) in_fence = 0;
            else { lines[o++] = lines[i]; continue; }
        }

        char c = 0;
        const char *lang = NULL;
        size_t lang_len = 0;
        size_t w = fence_opener(&lines[i], &c, &lang, &lang_len);
        if (w) {
            in_fence = 1;
            marker = c;
            width = w;
            fence_indent = lines[i].indent;
        } else if (is_comment_line(&lines[i])) {
            continue;
        }
        lines[o++] = lines[i];
    }
    *count = o;
}

/* The five elements whose content is text and nothing else — html.js's
 * `rawText` set, and the same names the HTML parser treats as RCDATA. */
static int is_raw_text(const char *tag) {
    return strcmp(tag, "pre") == 0 || strcmp(tag, "script") == 0 ||
           strcmp(tag, "style") == 0 || strcmp(tag, "textarea") == 0 ||
           strcmp(tag, "title") == 0;
}

static size_t parse_element(mdy_doc *doc, mdy_node *parent,
                            const mdy_line *lines, size_t count, size_t i) {
    const mdy_line *l = &lines[i];

    /*
     * `<!doctype html>` is its own node type and carries nothing — not the
     * name, not the line it was on. A comment is NOT special, by contrast:
     * `<!-- a comment -->` comes out as a <div> with `a` and `comment` as
     * boolean attributes, which is what the ordinary element rule below does
     * with it and what the JavaScript does too.
     */
    if (starts_ci(l, 1, "!doctype")) {
        mdy_node *dt = mdy_alloc(&doc->arena, sizeof *dt);
        if (dt) {
            memset(dt, 0, sizeof *dt);
            dt->type = MDY_DOCTYPE;
            separate(doc, parent);
            mdy_append(parent, dt);
        }
        return i + 1;
    }

    /*
     * Space after the `<` means nothing, the same as space between the
     * attributes — html.js says so in as many words, and a real layout is
     * written that way: `< html lang="en"` with the tag indented for reading.
     * Without this the tag came out empty, so every such element became a
     * <div> and its name became an attribute.
     */
    size_t n = 1;                       /* past the `<` */
    while (n < l->len && (l->text[n] == ' ' || l->text[n] == '\t')) n++;
    size_t tag_at = n;
    if (n < l->len && is_tag_start(l->text[n])) {
        n++;
        while (n < l->len && is_tag_char(l->text[n])) n++;
    }

    char tag[64];
    size_t tag_len = n - tag_at;
    if (tag_len == 0) { memcpy(tag, "div", 3); tag_len = 3; }
    else {
        if (tag_len > sizeof tag - 1) tag_len = sizeof tag - 1;
        for (size_t k = 0; k < tag_len; k++) {
            char c = l->text[tag_at + k];
            tag[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    tag[tag_len] = '\0';

    /* An element the schema does not allow produces nothing, and its children
     * go with it — which is what `sanitize` does. */
    if (doc->options.sanitize && !mdy_tag_allowed(tag))
        return child_lines(lines, count, i, l->indent);

    mdy_node *el = mdy_new_element(doc, tag, tag_len);
    const char *content = NULL;
    size_t content_len = 0;
    parse_attributes(doc, el, tag, l->text + n, l->len - n, &content, &content_len);

    size_t end = child_lines(lines, count, i, l->indent);

    /*
     * Elements whose content is TEXT and nothing else: pre, script, style,
     * textarea, title. Markup inside a <script> is not markup, and parsing it
     * as if it were is how a stylesheet ends up with an <em> in it.
     *
     * The lines come through as written, minus the indentation that put them
     * in here — two columns past the opener's own, so anything deeper keeps
     * the difference. A BLANK line contributes nothing at all rather than an
     * empty line: the JavaScript joins with `filter(Boolean)`, which drops it
     * along with an empty opener.
     */
    if (is_raw_text(tag)) {
        size_t strip = l->indent + 2;
        size_t need = 0;
        if (content) { trim(&content, &content_len); need += content_len + 1; }
        for (size_t k = i + 1; k < end; k++) {
            if (lines[k].blank) continue;
            size_t extra = lines[k].indent > strip ? lines[k].indent - strip : 0;
            need += extra + lines[k].len + 1;
        }
        char *text = need ? mdy_alloc(&doc->arena, need + 1) : NULL;
        size_t o = 0;
        if (text) {
            if (content && content_len) {
                memcpy(text + o, content, content_len);
                o += content_len;
            }
            for (size_t k = i + 1; k < end; k++) {
                if (lines[k].blank) continue;
                if (o) text[o++] = '\n';
                size_t extra = lines[k].indent > strip ? lines[k].indent - strip : 0;
                for (size_t sp = 0; sp < extra; sp++) text[o++] = ' ';
                memcpy(text + o, lines[k].text, lines[k].len);
                o += lines[k].len;
            }
            text[o] = '\0';
        }
        if (o) mdy_append(el, mdy_new_text(doc, text, o));
        mdy_set_position(el, lines, i, end > i + 1 ? end - 1 : i);
        separate(doc, parent);
        mdy_append(parent, el);
        return end;
    }

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
    mdy_set_position(el, lines, i, end > i + 1 ? end - 1 : i);
    if (end > i + 1) {
        /* The children's own column, so one of them being deeper than the
         * others is what makes a div rather than all of them. */
        size_t inner = lines[i + 1].indent;
        for (size_t k = i + 1; k < end; k++) if (!lines[k].blank && lines[k].indent < inner) inner = lines[k].indent;
        mdy_parse_block(doc, el, lines + i + 1, end - (i + 1), inner);
    }
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
                     const char *text, size_t len, int align,
                     const mdy_line *lines, size_t row_line) {
    mdy_node *cell = mdy_new_element(doc, tag, tag_len);
    if (align != ALIGN_NONE) {
        const char *style = align == ALIGN_LEFT ? "text-align: left"
                          : align == ALIGN_RIGHT ? "text-align: right"
                          : "text-align: center";
        mdy_set_string(doc, cell, "style", style, strlen(style));
    }
    mdy_parse_inline(doc, cell, text, len);
    mdy_set_position(cell, lines, row_line, row_line);
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
    for (size_t c = 0; c < columns; c++) add_cell(doc, head_row, "th", 2, starts[c], lens[c], align[c], lines, i);
    mdy_append(head_row, mdy_new_text(doc, "\n", 1));
    mdy_set_position(head_row, lines, i, i);
    mdy_append(thead, head_row);
    mdy_append(thead, mdy_new_text(doc, "\n", 1));
    mdy_set_position(thead, lines, i, i);
    mdy_append(table, thead);
    mdy_append(table, mdy_new_text(doc, "\n", 1));

    if (rows > 2) {
        mdy_node *tbody = mdy_new_element(doc, "tbody", 5);
        mdy_append(tbody, mdy_new_text(doc, "\n", 1));
        for (size_t r = i + 2; r < i + rows; r++) {
            size_t n = split_cells(&lines[r], starts, lens, 64);
            mdy_node *tr = mdy_new_element(doc, "tr", 2);
            for (size_t c = 0; c < n && c < columns; c++)
                add_cell(doc, tr, "td", 2, starts[c], lens[c], align[c], lines, r);
            mdy_append(tr, mdy_new_text(doc, "\n", 1));
            mdy_set_position(tr, lines, r, r);
            mdy_append(tbody, tr);
            mdy_append(tbody, mdy_new_text(doc, "\n", 1));
        }
        mdy_set_position(tbody, lines, i + 2, i + rows - 1);
        mdy_append(table, tbody);
        mdy_append(table, mdy_new_text(doc, "\n", 1));
    }

    mdy_set_position(table, lines, i, i + rows - 1);
    separate(doc, parent);
    mdy_append(parent, table);
    return i + rows;
}

/* ---- the block loop ------------------------------------------------------ */

/* Returns whether anything was added — an all-whitespace run produces no
 * paragraph, and must not produce a separator either. */
static int add_paragraph(mdy_doc *doc, mdy_node *parent, const char *joined, size_t len) {
    mdy_trim_end(&joined, &len);
    if (!len) return 0;
    mdy_node *p = mdy_new_element(doc, "p", 1);
    mdy_parse_inline(doc, p, joined, len);
    mdy_append(parent, p);
    return 1;
}

void mdy_parse_block(mdy_doc *doc, mdy_node *parent, const mdy_line *lines, size_t count, size_t base) {
    size_t i = 0;
    int produced = 0;

    while (i < count) {
        const mdy_line *l = &lines[i];

        if (l->blank) { i++; continue; }

        /*
         * Indentation is structural: a line further in than this run's own
         * column is a block of its own, in a <div>. It nests — four columns
         * under two is a div inside a div — and it applies at the very start
         * of a document too, so a file whose first line is indented opens with
         * one.
         *
         * An element opener takes its own indented lines as children instead
         * (parse_element), and a list item absorbs its continuation lines, so
         * by the time this fires the indentation belongs to nothing else.
         */
        if (l->indent > base) {
            size_t j = i;
            while (j < count && (lines[j].blank || lines[j].indent > base)) j++;
            while (j > i && lines[j - 1].blank) j--;

            size_t inner = lines[i].indent;
            for (size_t k = i; k < j; k++)
                if (!lines[k].blank && lines[k].indent < inner) inner = lines[k].indent;

            /*
             * EVERY TWO COLUMNS IS ONE LEVEL. The grammar says so in as many
             * words, and it means an indent of eight is FOUR nested <div>s
             * rather than one deep-indented one. Making a single div for any
             * depth looks right in a two-space document and is wrong in every
             * other: the corpus has eight-column indents that come out four
             * levels deep.
             */
            size_t levels = (inner - base) / 2;
            if (levels == 0) levels = 1;

            mdy_node *outer = NULL, *innermost = NULL;
            for (size_t d = 0; d < levels; d++) {
                mdy_node *div = mdy_new_element(doc, "div", 3);
                mdy_set_position(div, lines, i, j > i ? j - 1 : i);
                if (innermost) {
                    mdy_append(innermost, mdy_new_text(doc, "\n", 1));
                    mdy_append(innermost, div);
                    mdy_append(innermost, mdy_new_text(doc, "\n", 1));
                } else {
                    outer = div;
                }
                innermost = div;
            }
            mdy_parse_block(doc, innermost, lines + i, j - i, base + levels * 2);

            separate(doc, parent);
            mdy_append(parent, outer);
            produced = 1;
            i = j;
            continue;
        }

        /* --- thematic break: three or more of - * _ alone --- */
        if ((all_of(l, '-') || all_of(l, '*') || all_of(l, '_')) && l->len >= 3) {
            separate(doc, parent);
            mdy_node *rule = mdy_new_element(doc, "hr", 2);
            mdy_set_position(rule, lines, i, i);
            mdy_append(parent, rule);
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
            mdy_parse_inline(doc, h, body, body_len);
            /*
             * From the RENDERED text, not the source: `= Producing a //Book of
             * the Dead//` is `producing-a-book-of-the-dead`, because the
             * markers are gone by the time anyone reads the heading. Slugging
             * the source leaves the slashes in an id nothing can link to.
             */
            {
                char rendered[1024];
                size_t rlen = node_text(h, rendered, sizeof rendered, 0);
                set_heading_id(doc, h, rendered, rlen);
            }
            mdy_set_position(h, lines, i, i);
            separate(doc, parent);
            mdy_append(parent, h);
            produced = 1;
            i++;
            continue;
        }

        /* --- fenced code: three or more backticks or tildes --- */
        {
            char fence = 0;
            const char *lang = NULL;
            size_t lang_len = 0;
            size_t width = fence_opener(l, &fence, &lang, &lang_len);
            if (width) {
                size_t j = i + 1;
                size_t start = j;
                while (j < count && !closes_fence(&lines[j], fence, width)) j++;

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
                /* The fence's whole span, closing line included — and `code`
                 * carries the same one, which is what the JavaScript does. */
                size_t last_line = j < count ? j : (j > 0 ? j - 1 : 0);
                mdy_set_position(pre, lines, i, last_line);
                mdy_set_position(code, lines, i, last_line);
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
            size_t start_line = i;
            mdy_node *list = mdy_new_element(doc, ordered ? "ol" : "ul", 2);
            /*
             * An ordered list that does not begin at 1 says so — `1931.` gives
             * `<ol start="1931">`, which is what makes the rendered numbering
             * match what the author wrote. One is the default and is left off.
             */
            if (ordered) {
                long first = marker_number(l);
                if (first >= 0 && first != 1) mdy_set_number(doc, list, "start", (double)first);
            }
            mdy_append(list, mdy_new_text(doc, "\n", 1));
            int any_task = 0;

            /*
             * LOOSE OR TIGHT. A blank line between items does not end the
             * list — it makes it loose, and every item then wraps its content
             * in a <p> rather than holding it inline. That is one decision for
             * the whole list, so it has to be made before any item is built.
             */
            int loose = 0;
            {
                size_t scan = i;
                int seen_blank = 0;
                while (scan < count) {
                    if (lines[scan].blank) { seen_blank = 1; scan++; continue; }
                    int k_ordered = 0;
                    size_t k_width = list_marker(&lines[scan], &k_ordered);
                    if (k_width && k_ordered == ordered && lines[scan].indent == l->indent) {
                        if (seen_blank) { loose = 1; break; }
                        scan++;
                        continue;
                    }
                    if (lines[scan].indent > l->indent) { scan++; continue; }
                    break;
                }
            }

            while (i < count) {
                /* Skip blank lines BETWEEN items — in a loose list they
                 * separate items rather than ending the list. */
                if (lines[i].blank) {
                    size_t peek = i;
                    while (peek < count && lines[peek].blank) peek++;
                    int p_ordered = 0;
                    if (peek < count && list_marker(&lines[peek], &p_ordered) &&
                        p_ordered == ordered && lines[peek].indent == l->indent) {
                        i = peek;
                        continue;
                    }
                    break;
                }
                int this_ordered = 0;
                size_t width = list_marker(&lines[i], &this_ordered);
                if (!width || this_ordered != ordered || lines[i].indent != l->indent) break;

                /*
                 * An item owns every following line indented past the marker:
                 * a plain one continues its text, a deeper list marker becomes
                 * a nested list. `- one` then `  two` is one item reading
                 * "one two"; `- a` then `  - b` is an item holding a <ul>.
                 */
                /*
                 * A continuation line needs NO indentation — `- one` followed
                 * by an unindented `two` is one item reading "one two". What
                 * ends an item is another marker, or a line that starts a
                 * block of its own; indentation only matters for deciding
                 * whether a blank line is a gap inside the item or the end of
                 * it.
                 */
                size_t item_end = i + 1;
                while (item_end < count) {
                    const mdy_line *k = &lines[item_end];
                    if (k->blank) {
                        size_t peek = item_end;
                        while (peek < count && lines[peek].blank) peek++;
                        if (peek < count && lines[peek].indent > l->indent) { item_end = peek; continue; }
                        break;
                    }
                    if (k->indent > l->indent) { item_end++; continue; }
                    int k_ordered = 0;
                    if (list_marker(k, &k_ordered)) break;
                    if (k->text[0] == '<' || k->text[0] == '=') break;
                    if ((all_of(k, '-') || all_of(k, '*') || all_of(k, '_')) && k->len >= 3) break;
                    item_end++;
                }
                while (item_end > i + 1 && lines[item_end - 1].blank) item_end--;

                const char *body = lines[i].text + width;
                size_t body_len = lines[i].len - width;
                trim(&body, &body_len);

                mdy_node *item = mdy_new_element(doc, "li", 2);

                /* `[ ]` or `[x]` after the marker makes it a task. */
                int task = -1;
                if (body_len >= 3 && body[0] == '[' && body[2] == ']' &&
                    (body[1] == ' ' || body[1] == 'x' || body[1] == 'X')) {
                    task = body[1] == ' ' ? 0 : 1;
                    body += 3;
                    body_len -= 3;
                    while (body_len && *body == ' ') { body++; body_len--; }
                    any_task = 1;
                    mdy_add_class(doc, item, "task-list-item");

                    mdy_node *box = mdy_new_element(doc, "input", 5);
                    mdy_set_string(doc, box, "type", "checkbox", 8);
                    mdy_set_bool(doc, box, "checked", task);
                    mdy_set_bool(doc, box, "disabled", 1);
                    mdy_append(item, box);
                    mdy_append(item, mdy_new_text(doc, " ", 1));
                }

                /*
                 * Continuation lines that are themselves plain join the item's
                 * text; from the first line that opens a block, the rest is
                 * parsed as blocks. That split is what makes `- one` / `  two`
                 * one sentence and `- a` / `  - b` a nested list.
                 */
                size_t plain_end = i + 1;
                while (plain_end < item_end && !lines[plain_end].blank) {
                    int sub = 0;
                    if (list_marker(&lines[plain_end], &sub) || lines[plain_end].text[0] == '<' ||
                        lines[plain_end].text[0] == '=') break;
                    plain_end++;
                }

                size_t total = body_len;
                for (size_t k = i + 1; k < plain_end; k++) total += lines[k].len + 1;
                char *joined = mdy_alloc(doc ? &doc->arena : NULL, total + 1);
                size_t o = 0;
                memcpy(joined, body, body_len);
                o = body_len;
                for (size_t k = i + 1; k < plain_end; k++) {
                    /* The separator goes in even when the marker line left
                     * nothing behind it — `1931.` then `next` is an item
                     * reading " next", with the space the join put there. */
                    joined[o++] = ' ';
                    memcpy(joined + o, lines[k].text, lines[k].len);
                    o += lines[k].len;
                }
                joined[o] = '\0';
                if (loose) {
                    /* `li("\n" p(content) "\n")` — the shape a blank line
                     * between items produces. */
                    mdy_node *wrap = mdy_new_element(doc, "p", 1);
                    mdy_parse_inline(doc, wrap, joined, o);
                    /* The paragraph a loose item wraps its content in spans
                     * the same lines the item does — it IS the item's
                     * content, not a block of its own. */
                    mdy_set_position(wrap, lines, i, item_end > i ? item_end - 1 : i);
                    mdy_append(item, mdy_new_text(doc, "\n", 1));
                    mdy_append(item, wrap);
                    mdy_append(item, mdy_new_text(doc, "\n", 1));
                } else {
                    mdy_parse_inline(doc, item, joined, o);
                }

                if (item_end > plain_end) {
                    size_t inner = lines[plain_end].indent;
                    for (size_t k = plain_end; k < item_end; k++)
                        if (!lines[k].blank && lines[k].indent < inner) inner = lines[k].indent;
                    mdy_parse_block(doc, item, lines + plain_end, item_end - plain_end, inner);
                }

                mdy_set_position(item, lines, i, item_end > i ? item_end - 1 : i);
                mdy_append(list, item);
                mdy_append(list, mdy_new_text(doc, "\n", 1));
                i = item_end;
            }

            /* The list is marked once, after its items, because one task item
             * makes the whole list a task list. */
            if (any_task) mdy_add_class(doc, list, "contains-task-list");
            mdy_set_position(list, lines, start_line, i > start_line ? i - 1 : start_line);
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
            /* A line that starts another block ends this paragraph — and a
             * line further in than this run is another block, which is what
             * makes `top` / `  in` a paragraph and a div rather than one
             * paragraph reading "top in". */
            if (j > i && lines[j].indent > base) break;
            char fence_here = 0;
            const char *fl = NULL;
            size_t fll = 0;
            if (j > i && fence_opener(&lines[j], &fence_here, &fl, &fll)) break;
            if (j > i && (lines[j].text[0] == '=' || lines[j].text[0] == '<' ||
                          list_marker(&lines[j], &ordered_here) ||
                          ((all_of(&lines[j], '-') || all_of(&lines[j], '*') || all_of(&lines[j], '_')) && lines[j].len >= 3))) break;
            total += lines[j].len + 1;
            j++;
        }
        char *joined = mdy_alloc(&doc->arena, total + 1);
        size_t o = 0;
        for (size_t k = i; k < j; k++) {
            /*
             * TRAILING whitespace only. A line's leading spaces are its
             * indentation and were removed when the lines were measured; a
             * leading NO-BREAK space is not indentation and belongs to the
             * text, which is what `\u00a0\u00a0Kingdom of …` in the corpus
             * depends on.
             */
            const char *lt = lines[k].text;
            size_t ll = lines[k].len;
            mdy_trim_end(&lt, &ll);
            if (k > i && o) joined[o++] = ' ';
            memcpy(joined + o, lt, ll);
            o += ll;
        }
        joined[o] = '\0';
        /* A run of only whitespace produces no paragraph, and so must produce
         * no separator either — which is why the emptiness is decided here
         * rather than inside add_paragraph. */
        /*
         * Setext: a line of `=` under a paragraph makes it an <h1>, and FOUR
         * or more `-` make it an <h2>. Three hyphens do not — that is a
         * thematic break, and the paragraph above it stands on its own.
         */
        const mdy_line *under = j < count ? &lines[j] : NULL;
        int setext = 0;
        if (under && !under->blank && under->indent == base) {
            if (all_of(under, '=')) setext = 1;
            else if (all_of(under, '-') && under->len >= 4) setext = 2;
        }

        const char *probe = joined;
        size_t probe_len = o;
        mdy_trim_end(&probe, &probe_len);

        if (setext && probe_len) {
            char tag[3] = { 'h', (char)('0' + setext), '\0' };
            mdy_node *h = mdy_new_element(doc, tag, 2);
            mdy_parse_inline(doc, h, probe, probe_len);
            {
                char rendered[1024];
                size_t rlen = node_text(h, rendered, sizeof rendered, 0);
                set_heading_id(doc, h, rendered, rlen);
            }
            mdy_set_position(h, lines, i, j);
            separate(doc, parent);
            mdy_append(parent, h);
            produced = 1;
            i = j + 1;
            continue;
        }

        if (probe_len) {
            separate(doc, parent);
            mdy_node *before = parent->last;
            if (add_paragraph(doc, parent, joined, o)) {
                mdy_node *made = before ? before->next : parent->first;
                mdy_set_position(made, lines, i, j > i ? j - 1 : i);
            }
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

        /*
         * A definition's content runs on: following non-blank lines belong to
         * it, joined with a space exactly as a paragraph's do, and it ends at
         * a blank line. Taking only the marker line left every continuation
         * behind as a stray paragraph in the body of the document — which is
         * what `p` being 26 too high was, and why several documents showed
         * footnote prose where their Footnotes section should have been.
         */
        const char *head = l->text + k;
        size_t head_len = l->len - k;
        trim(&head, &head_len);

        size_t last = i;
        size_t total = head_len;
        while (last + 1 < count && !lines[last + 1].blank) {
            /* Another definition starts its own; it does not continue this. */
            const mdy_line *next = &lines[last + 1];
            if (next->len > 4 && next->text[0] == '[' && next->text[1] == '[') {
                size_t probe = 2;
                while (probe < next->len && (next->text[probe] == ' ' || next->text[probe] == '\t')) probe++;
                if (probe < next->len && next->text[probe] == '^') break;
            }
            last++;
            total += next->len + 1;
        }

        char *joined = mdy_alloc(&doc->arena, total + 1);
        size_t jo = 0;
        memcpy(joined, head, head_len);
        jo = head_len;
        for (size_t r = i + 1; r <= last; r++) {
            if (jo) joined[jo++] = ' ';
            memcpy(joined + jo, lines[r].text, lines[r].len);
            jo += lines[r].len;
        }
        joined[jo] = '\0';

        const char *content = joined;
        size_t content_len = jo;
        trim(&content, &content_len);

        /*
         * A LATER definition with the same id replaces an earlier one. The
         * reference corpus leans on this hard — one file has 402 definitions
         * with 360 distinct ids — and keeping the first instead of the last
         * cost 135 links, because the definitions being shadowed were the
         * short ones and the definitions doing the shadowing were full of
         * citations.
         */
        mdy_footnote *existing = mdy_footnote_find(doc, l->text + id_start, id_len);
        if (existing) {
            existing->content = mdy_strdup_n(&doc->arena, content, content_len);
            existing->content_len = content_len;
            for (size_t r = i; r <= last; r++) { lines[r].len = 0; lines[r].blank = 1; }
            i = last;
            continue;
        }

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

        for (size_t r = i; r <= last; r++) { lines[r].len = 0; lines[r].blank = 1; }
        i = last;
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

    /*
     * ORDER, and it is observable: front matter comes off FIRST, and comments
     * only after it.
     *
     * Stripping first looks harmless and is not. A `#` line above the fence
     * stops the fence being the top of the document, so there is no front
     * matter and the whole thing is a paragraph — strip it first and the
     * fence floats up and the document acquires front matter it does not
     * have. And a `#` line INSIDE the block is YAML's own comment, which is
     * the front matter's business rather than the grammar's.
     *
     * This is src/parse/block.js's sequence: extractMatter, then the code,
     * then stripComments, then the lines are measured. The code is the one
     * step this parser does not have — see shims/parse.js.
     */
    size_t start = 0;
    if (doc->options.frontmatter) start = front_matter_lines(lines, count);

    if (!doc->options.documents) {
        size_t body = count - start;
        strip_comments(lines + start, &body);
        collect_definitions(doc, lines + start, body);
        mdy_parse_block(doc, doc->root, lines + start, body, 0);
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
        /* Per document, and after its own front matter — see the note above.
         * Compacting inside [section_start, i) leaves the lines beyond `i`
         * where the scan above still expects them. */
        size_t body = i - section_start;
        strip_comments(lines + section_start, &body);
        collect_definitions(doc, lines + section_start, body);
        mdy_parse_block(doc, article, lines + section_start, body, 0);
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
