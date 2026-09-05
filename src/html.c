/*
 * hast -> HTML — a port of `hast-util-to-html`.
 *
 * The public contract, and what is deliberately absent, is in
 * include/mdyhtml.h. This file is the implementation and nothing else depends
 * on it: it takes a tree and returns a string, so it can be tested on trees
 * built by hand with no parser involved (test/html.c) and against the original
 * over the reference corpus (test/compare-html.mjs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyhtml.h"
#include "props_table.h"

void mdy_html_options_default(mdy_html_options *out) {
    out->allow_dangerous_html = 1;
}

/* ---- output --------------------------------------------------------------- */

typedef struct { char *s; size_t len, cap; int ok; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 8192;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown) { b->ok = 0; return; }
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

/* ---- character references -------------------------------------------------- */

/*
 * `stringifyEntities(value, {subset})`.
 *
 * With a subset given, `core` replaces exactly those characters and returns —
 * no surrogate pass, no control-character pass. And with the default format
 * options (no `useNamedReferences`, no `useShortestReferences`, no
 * `omitOptionalSemicolons`) `formatSmart` is just `toHexadecimal`:
 *
 *     '&#x' + code.toString(16).toUpperCase() + ';'
 *
 * So `<` is `&#x3C;`, never `&lt;`. Every character in every subset used here
 * is ASCII, which is why this can work a byte at a time: a UTF-8 continuation
 * byte can never equal one of them, so multi-byte characters pass through
 * untouched, which is what the JavaScript does with them too.
 */
static void escape(Buf *b, const char *s, size_t len, const char *subset) {
    size_t run = 0;   /* bytes since the last reference, written in one go */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != 0 && c < 0x80 && strchr(subset, (char)c)) {
            put(b, s + i - run, run);
            run = 0;
            char ref[16];
            snprintf(ref, sizeof ref, "&#x%X;", c);
            puts_(b, ref);
        } else {
            run++;
        }
    }
    put(b, s + len - run, run);
}

/*
 * The three subsets, from hast-util-to-html's `constants`, at [1][1] — the
 * safe, no-parse-errors corner, which is where the default options land.
 *
 * NUL is in two of the original's subsets and is left out of all three here:
 * it cannot appear in a NUL-terminated string, which is what this tree holds.
 */
static const char SUBSET_TEXT[]  = "<&";
static const char SUBSET_NAME[]  = "\t\n\f\r \"&'/<=>`";
static const char SUBSET_VALUE[] = "\"&'`";

/* ---- the property table --------------------------------------------------- */

typedef struct {
    const char *attribute;
    size_t attribute_len;
    unsigned flags;
    char buf[128];      /* for a `data-*` name, which is computed */
} AttrInfo;

/** `/^data[-\w.:]+$/i` on the property name. */
static int data_shaped(const char *s, size_t len) {
    if (len <= 4) return 0;
    for (size_t i = 4; i < len; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
        if (!ok) return 0;
    }
    return 1;
}

/**
 * `find(html, property).attribute`, for the direction this file needs.
 *
 * Three answers, in the order `find` gives them:
 *
 *   - a known property has its attribute in the table (`className` is
 *     `class`, `htmlFor` is `for`);
 *   - a `data*` property is kebab-cased back — `dataFooBar` is
 *     `data-foo-bar`, with a leading dash supplied when the camel run does
 *     not start one;
 *   - anything else is written exactly as it is named.
 */
static void attr_info(const char *property, size_t len, AttrInfo *out) {
    out->flags = 0;

    size_t lo = 0, hi = MDY_PROP_INFO_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *cand = MDY_PROP_INFO[mid].property;
        size_t clen = strlen(cand);
        size_t n = clen < len ? clen : len;
        int cmp = memcmp(cand, property, n);
        if (cmp == 0) cmp = clen < len ? -1 : (clen > len ? 1 : 0);
        if (cmp == 0) {
            out->attribute = MDY_PROP_INFO[mid].attribute;
            out->attribute_len = strlen(out->attribute);
            out->flags = MDY_PROP_INFO[mid].flags;
            return;
        }
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }

    /* `normal.slice(0, 4) === 'data'` is on the LOWERCASED name; the kebab
     * conversion then runs on the original. */
    int is_data = len > 4 &&
        (property[0] == 'd' || property[0] == 'D') &&
        (property[1] == 'a' || property[1] == 'A') &&
        (property[2] == 't' || property[2] == 'T') &&
        (property[3] == 'a' || property[3] == 'A');

    if (is_data && property[4] != '-' && data_shaped(property, len) &&
        len * 2 + 8 < sizeof out->buf) {
        /*
         * `rest.replace(/[A-Z]/g, c => '-' + c.toLowerCase())`, then a leading
         * dash if the result does not already start with one.
         *
         * Guarded by `if (!dash.test(rest))`: a `rest` that already contains
         * `-[a-z]` is left as it was written. The original's `dash` regex
         * carries the `g` flag and is shared between calls, so its `lastIndex`
         * makes that test answer differently depending on what was asked
         * before it. That is a bug in the original rather than a rule, and it
         * is not reproduced here — this reads the test as written.
         */
        const char *rest = property + 4;
        size_t rest_len = len - 4;
        int has_dash_lower = 0;
        for (size_t i = 0; i + 1 < rest_len; i++) {
            if (rest[i] == '-' && rest[i + 1] >= 'a' && rest[i + 1] <= 'z') { has_dash_lower = 1; break; }
        }
        size_t o = 0;
        out->buf[o++] = 'd'; out->buf[o++] = 'a'; out->buf[o++] = 't'; out->buf[o++] = 'a';
        if (has_dash_lower) {
            memcpy(out->buf + o, rest, rest_len);
            o += rest_len;
        } else {
            size_t start = o;
            for (size_t i = 0; i < rest_len; i++) {
                char c = rest[i];
                if (c >= 'A' && c <= 'Z') {
                    out->buf[o++] = '-';
                    out->buf[o++] = (char)(c - 'A' + 'a');
                } else {
                    out->buf[o++] = c;
                }
            }
            if (o == start || out->buf[start] != '-') {
                memmove(out->buf + start + 1, out->buf + start, o - start);
                out->buf[start] = '-';
                o++;
            }
        }
        out->buf[o] = '\0';
        out->attribute = out->buf;
        out->attribute_len = o;
        return;
    }

    out->attribute = property;
    out->attribute_len = len;
}

/* ---- void elements --------------------------------------------------------- */

/** html-void-elements, the same list the parser uses. */
static int is_void(const char *tag) {
    static const char *const VOID[] = {
        "area", "base", "basefont", "bgsound", "br", "col", "command", "embed",
        "frame", "hr", "image", "img", "input", "keygen", "link", "meta",
        "param", "source", "track", "wbr",
    };
    for (size_t i = 0; i < sizeof VOID / sizeof VOID[0]; i++)
        if (strcmp(VOID[i], tag) == 0) return 1;
    return 0;
}

/* ---- values ---------------------------------------------------------------- */

/** `String(value)` for a number, which for every value this tree can hold is
 * an integer. A non-integer would need JavaScript's own shortest-round-trip
 * formatting; nothing produces one, and `%g` would be a guess. */
static void put_number(Buf *b, double v) {
    char tmp[40];
    if (v == (double)(long long)v) snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    else snprintf(tmp, sizeof tmp, "%g", v);
    puts_(b, tmp);
}

/*
 * One attribute, or nothing.
 *
 * The order of the tests is the original's and each one drops something:
 * an overloaded boolean whose value repeats its own name becomes `true`; a
 * boolean with any non-string value becomes `Boolean(value)`; and `false`,
 * `null` and `undefined` produce no attribute at all rather than an empty one.
 */
static void write_attribute(Buf *b, const mdy_prop *p) {
    AttrInfo info;
    attr_info(p->name, strlen(p->name), &info);

    int truthy = 0;
    int boolean_like = 0;

    if ((info.flags & MDY_ATTR_OVERLOADED) &&
        p->type == MDY_PROP_STRING &&
        (strcmp(p->as.string, info.attribute) == 0 || p->as.string[0] == '\0')) {
        boolean_like = 1;
        truthy = 1;
    } else if (info.flags & (MDY_ATTR_BOOLEAN | MDY_ATTR_OVERLOADED)) {
        if (p->type != MDY_PROP_STRING ||
            strcmp(p->as.string, info.attribute) == 0 || p->as.string[0] == '\0') {
            boolean_like = 1;
            switch (p->type) {
                case MDY_PROP_BOOL:   truthy = p->as.boolean != 0; break;
                case MDY_PROP_NUMBER: truthy = p->as.number != 0; break;
                case MDY_PROP_STRING: truthy = p->as.string[0] != '\0'; break;
                case MDY_PROP_LIST:   truthy = 1; break;  /* an array is truthy */
            }
        }
    }

    /* `false` here is the value being dropped, not an empty attribute. */
    if (boolean_like && !truthy) return;
    if (!boolean_like && p->type == MDY_PROP_BOOL && !p->as.boolean) return;

    escape(b, info.attribute, info.attribute_len, SUBSET_NAME);
    if (boolean_like || (p->type == MDY_PROP_BOOL && p->as.boolean)) return;

    put(b, "=\"", 2);
    switch (p->type) {
        case MDY_PROP_STRING:
            escape(b, p->as.string, strlen(p->as.string), SUBSET_VALUE);
            break;
        case MDY_PROP_NUMBER:
            /* Digits and a sign; nothing in the value subset. */
            put_number(b, p->as.number);
            break;
        case MDY_PROP_LIST: {
            /*
             * `spaces(value, {padLeft: true})` or `commas(value, {padLeft:
             * true})` — the separator is `' '` for a space-separated list and
             * `', '` for a comma-separated one, the pad being what `padLeft`
             * asks for.
             */
            const char *sep = (info.flags & MDY_ATTR_COMMAS) ? ", " : " ";
            Buf tmp = { NULL, 0, 0, 1 };
            for (size_t i = 0; i < p->list_len; i++) {
                if (i) puts_(&tmp, sep);
                puts_(&tmp, p->list[i]);
            }
            if (tmp.s) escape(b, tmp.s, tmp.len, SUBSET_VALUE);
            free(tmp.s);
            break;
        }
        case MDY_PROP_BOOL:
            /* A non-boolean property holding `true` — `String(true)`. */
            puts_(b, "true");
            break;
    }
    put(b, "\"", 1);
}

/* ---- nodes ----------------------------------------------------------------- */

static void write_node(Buf *b, const mdy_node *n, const mdy_node *parent,
                       const mdy_html_options *o);

static void write_children(Buf *b, const mdy_node *n, const mdy_html_options *o) {
    for (const mdy_node *c = n->first; c; c = c->next) write_node(b, c, n, o);
}

static void write_node(Buf *b, const mdy_node *n, const mdy_node *parent,
                       const mdy_html_options *o) {
    switch (n->type) {
        case MDY_ROOT:
            write_children(b, n, o);
            return;

        case MDY_TEXT:
            /*
             * Text inside a <script> or a <style> is NOT escaped: those hold
             * a program, and `&` in one means `&`. Everywhere else `<` and `&`
             * become references.
             */
            if (parent && parent->type == MDY_ELEMENT &&
                (strcmp(parent->tag, "script") == 0 || strcmp(parent->tag, "style") == 0)) {
                puts_(b, n->text ? n->text : "");
            } else if (n->text) {
                escape(b, n->text, strlen(n->text), SUBSET_TEXT);
            }
            return;

        case MDY_RAW:
            /* Written through, or escaped like text when that is refused. */
            if (o->allow_dangerous_html) puts_(b, n->text ? n->text : "");
            else if (n->text) escape(b, n->text, strlen(n->text), SUBSET_TEXT);
            return;

        case MDY_COMMENT:
            /* `bogusComments` is off, so the plain form. The original also
             * escapes nothing inside; a comment holding `-->` is the author's
             * problem, as it is in the JavaScript. */
            puts_(b, "<!--");
            puts_(b, n->text ? n->text : "");
            puts_(b, "-->");
            return;

        case MDY_DOCTYPE:
            /* Lower case with a space, which is what `upperDoctype: false` and
             * `tightDoctype: false` mean. */
            puts_(b, "<!doctype html>");
            return;

        case MDY_ELEMENT:
            break;
    }

    /*
     * An element categorised as void that turns out to have children is not
     * void after all — which is how `<menuitem>` and anything else the lists
     * disagree about still serialises with a closing tag.
     */
    int self_closing = is_void(n->tag) && n->first == NULL;

    put(b, "<", 1);
    puts_(b, n->tag);

    /*
     * Attributes are separated by a single space, and there is a space before
     * the first only when there IS one. `tightAttributes` is off, so the
     * separator never collapses.
     */
    for (const mdy_prop *p = n->props; p; p = p->next) {
        size_t before = b->len;
        put(b, " ", 1);
        size_t mark = b->len;
        write_attribute(b, p);
        /* A property that serialises to nothing takes its separator with it —
         * the original collects the non-empty ones and joins those. */
        if (b->len == mark && b->s) { b->len = before; b->s[b->len] = '\0'; }
    }

    put(b, ">", 1);
    write_children(b, n, o);

    /* `closeSelfClosing` is off, so a void element gets no ` /`, and no
     * closing tag either. */
    if (!self_closing) {
        put(b, "</", 2);
        puts_(b, n->tag);
        put(b, ">", 1);
    }
}

char *mdy_to_html(const mdy_node *node, const mdy_html_options *options) {
    mdy_html_options defaults;
    if (!options) { mdy_html_options_default(&defaults); options = &defaults; }
    if (!node) return NULL;

    Buf b = { NULL, 0, 0, 1 };
    put(&b, "", 0);           /* so an empty tree returns "" rather than NULL */
    write_node(&b, node, NULL, options);
    if (!b.ok) { free(b.s); return NULL; }
    return b.s;
}
