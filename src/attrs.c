/*
 * Attributes: parsing them, naming them the way hast does, and dropping the
 * ones the schema does not allow.
 *
 * All three are one file because they are one decision per attribute, and
 * splitting them would mean walking the same list three times. The order is
 * the order the JavaScript applies: parse, rename, then check — so an
 * attribute is judged under the name the schema knows it by.
 *
 * SANITISATION IS NOT OPTIONAL HERE, and it is easy to miss why. `<td
 * scope="col"` produces no `scope` in the tree at all, because `scope` is
 * allowed on `<th>` and not on `<td>`. Skipping the check does not produce
 * "slightly more" tree — it produces a different one.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- hast property names -------------------------------------------------- */

/*
 * The HTML attribute names that hast spells differently. Only the ones this
 * parser can actually meet: the full table is a few hundred entries and every
 * one absent from here falls through to the `data-`/`aria-` rules or to itself,
 * which is right for `src`, `href`, `alt`, `id`, `lang` and the rest.
 */
static const struct { const char *html; const char *hast; } RENAMES[] = {
    { "class",       "className" },
    { "colspan",     "colSpan" },
    { "rowspan",     "rowSpan" },
    { "for",         "htmlFor" },
    { "http-equiv",  "httpEquiv" },
    { "accept-charset", "acceptCharset" },
    { "maxlength",   "maxLength" },
    { "minlength",   "minLength" },
    { "readonly",    "readOnly" },
    { "tabindex",    "tabIndex" },
    { "crossorigin", "crossOrigin" },
    { "datetime",    "dateTime" },
    { "srcset",      "srcSet" },
    { "usemap",      "useMap" },
    { "novalidate",  "noValidate" },
    { "autocomplete","autoComplete" },
    { "autofocus",   "autoFocus" },
    { "autoplay",    "autoPlay" },
    { "contenteditable", "contentEditable" },
    { "spellcheck",  "spellCheck" },
};

/**
 * `data-foo-bar` -> `dataFooBar`, `aria-label` -> `ariaLabel`. Written into
 * `out` (which must hold len+1), returning its length, or 0 when the name is
 * not one of those.
 */
static size_t camel_prefixed(const char *name, size_t len, char *out) {
    size_t skip;
    if (len > 5 && memcmp(name, "data-", 5) == 0) skip = 5;
    else if (len > 5 && memcmp(name, "aria-", 5) == 0) skip = 5;
    else return 0;

    memcpy(out, name, 4);     /* "data" or "aria" */
    size_t o = 4;
    int upper = 1;
    for (size_t i = skip; i < len; i++) {
        if (name[i] == '-') { upper = 1; continue; }
        char c = name[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[o++] = c;
        upper = 0;
    }
    out[o] = '\0';
    return o;
}

const char *mdy_hast_name(mdy_doc *doc, const char *name, size_t len) {
    for (size_t i = 0; i < sizeof RENAMES / sizeof RENAMES[0]; i++) {
        if (strlen(RENAMES[i].html) == len && memcmp(RENAMES[i].html, name, len) == 0) {
            return mdy_intern(&doc->arena, &doc->names, RENAMES[i].hast, strlen(RENAMES[i].hast));
        }
    }
    char buf[128];
    if (len < sizeof buf - 1) {
        size_t n = camel_prefixed(name, len, buf);
        if (n) return mdy_intern(&doc->arena, &doc->names, buf, n);
    }
    return mdy_intern(&doc->arena, &doc->names, name, len);
}

/* ---- the schema ----------------------------------------------------------- */

/*
 * defaultSchema from ../../mdy-docs/src/parse/sanitize.js. An element not
 * listed is dropped entirely; an attribute not allowed for its element is
 * dropped from it.
 */
static const char *const TAGS[] = {
    "address","article","aside","blockquote","br","details","div","figcaption",
    "figure","footer","header","hgroup","hr","main","nav","p","pre","section",
    "summary","wbr","h1","h2","h3","h4","h5","h6","dd","dl","dt","li","ol","ul",
    "caption","col","colgroup","table","tbody","td","tfoot","th","thead","tr",
    "a","abbr","b","bdi","bdo","cite","code","data","dfn","del","em","i","ins",
    "kbd","mark","q","rp","rt","ruby","s","samp","small","span","strong","sub",
    "sup","time","u","var","img","input",
};

/* Allowed on every element. */
static const char *const GLOBAL_ATTRS[] = {
    "class","dir","hidden","id","lang","role","style","title","translate",
};

static const struct { const char *tag; const char *const attrs[8]; } PER_TAG[] = {
    { "a",     { "href","name","rel","target", NULL } },
    { "img",   { "alt","decoding","height","loading","src","width", NULL } },
    { "td",    { "align","colspan","headers","rowspan","valign", NULL } },
    { "th",    { "abbr","align","colspan","headers","rowspan","scope", NULL } },
    { "table", { "align","summary", NULL } },
    { "li",    { "value", NULL } },
};

int mdy_tag_allowed(const char *tag) {
    for (size_t i = 0; i < sizeof TAGS / sizeof TAGS[0]; i++) {
        if (strcmp(TAGS[i], tag) == 0) return 1;
    }
    return 0;
}

/** `name` is the RAW html attribute name, before renaming — that is what the
 * schema is written in terms of. */
int mdy_attr_allowed(const char *tag, const char *name, size_t len) {
    for (size_t i = 0; i < sizeof GLOBAL_ATTRS / sizeof GLOBAL_ATTRS[0]; i++) {
        if (strlen(GLOBAL_ATTRS[i]) == len && memcmp(GLOBAL_ATTRS[i], name, len) == 0) return 1;
    }
    /* `data-*` and `aria-*` are allowed everywhere — the two empty objects in
     * the JavaScript's global list are its way of saying so. */
    if (len > 5 && (memcmp(name, "data-", 5) == 0 || memcmp(name, "aria-", 5) == 0)) return 1;

    for (size_t i = 0; i < sizeof PER_TAG / sizeof PER_TAG[0]; i++) {
        if (strcmp(PER_TAG[i].tag, tag) != 0) continue;
        for (size_t j = 0; PER_TAG[i].attrs[j]; j++) {
            if (strlen(PER_TAG[i].attrs[j]) == len && memcmp(PER_TAG[i].attrs[j], name, len) == 0) return 1;
        }
        return 0;
    }
    return 0;
}

/*
 * A URL whose protocol the schema permits. Anything with no scheme at all is
 * relative and fine; a scheme that is not on the list makes the attribute
 * disappear, which is the whole point — `javascript:` in an author's `href`
 * must not survive into the tree.
 */
int mdy_protocol_allowed(const char *attr, const char *value, size_t len) {
    static const char *const HREF_OK[] = { "ftp","http","https","irc","ircs","mailto","sms","tel","xmpp", NULL };
    static const char *const SRC_OK[]  = { "http","https", NULL };
    const char *const *allowed;
    if (strcmp(attr, "href") == 0) allowed = HREF_OK;
    else if (strcmp(attr, "src") == 0 || strcmp(attr, "cite") == 0) allowed = SRC_OK;
    else return 1;

    size_t colon = 0;
    while (colon < len && value[colon] != ':') {
        char c = value[colon];
        /* A `/`, `?` or `#` before any colon means there is no scheme. */
        if (c == '/' || c == '?' || c == '#') return 1;
        colon++;
    }
    if (colon == len) return 1;   /* no colon: relative */

    for (size_t i = 0; allowed[i]; i++) {
        size_t n = strlen(allowed[i]);
        if (n != colon) continue;
        size_t k = 0;
        while (k < n) {
            char a = value[k], b = allowed[i][k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

/*
 * The Unicode this file used to carry by hand — a partial lowercase table and
 * a guess at `\p{L}` — now lives in unicode.c, over baru-re's generated UCD
 * data. Both were wrong in ways only a non-English document would show.
 */
