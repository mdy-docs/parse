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

/* ---- Unicode, the little of it this needs -------------------------------- */

/*
 * Lowercase one UTF-8 character in place, returning how many bytes it was.
 *
 * Only Latin-1 Supplement and Latin Extended-A, which is what a corpus of
 * European place names and transliterations actually contains — `Égypte` has
 * to lowercase or `[[ Description de l'Égypte ]]` resolves to a different URL
 * than the JavaScript gives it. A full case table is a few thousand entries
 * and would be dead weight here; when a document needs one, this is the place
 * that grows.
 *
 * Both ranges are two bytes in UTF-8 and both have regular structure:
 *   U+00C0–U+00DE  (C3 80–9E)  add 0x20, except U+00D7 which is multiplication
 *   U+0100–U+017F  (C4 80–C5 BF)  even code point upper, odd lower
 */
/*
 * Is this code point a letter or a number, as `\p{L}` and `\p{N}` mean it?
 *
 * Only the question defaultResolve asks, and only accurately enough for what a
 * document contains: everything non-ASCII is a letter EXCEPT the punctuation
 * and symbol blocks. An en dash in `First Jewish–Roman War` is deleted by the
 * JavaScript and kept by a rule that says "non-ASCII is a letter", which is
 * how this was found.
 */
int mdy_is_letter_or_number(const char *p, size_t left) {
    unsigned char a = (unsigned char)p[0];
    if (a < 0x80) return (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') || (a >= '0' && a <= '9');
    if (left < 2) return 0;
    unsigned char b = (unsigned char)p[1];

    /* U+00A0–U+00BF: no-break space, punctuation, signs — none are letters. */
    if (a == 0xC2) return 0;
    /* U+00D7 multiplication and U+00F7 division sit inside Latin-1's letters. */
    if (a == 0xC3) return b != 0x97 && b != 0xB7;

    if (left < 3) return 1;
    unsigned char c = (unsigned char)p[2];
    unsigned int cp = ((unsigned)(a & 0x0F) << 12) | ((unsigned)(b & 0x3F) << 6) | (c & 0x3F);
    if (a >= 0xE0 && a <= 0xEF) {
        if (cp >= 0x2000 && cp <= 0x206F) return 0;   /* General Punctuation */
        if (cp >= 0x20A0 && cp <= 0x2BFF) return 0;   /* currency, arrows, symbols */
        if (cp >= 0x3000 && cp <= 0x303F) return 0;   /* CJK punctuation */
        if (cp >= 0xFE30 && cp <= 0xFE6F) return 0;   /* compatibility forms */
        if (cp >= 0xFF00 && cp <= 0xFF20) return 0;   /* fullwidth punctuation */
    }
    return 1;
}

size_t mdy_lower_utf8(const char *in, size_t left, char *out) {
    unsigned char a = (unsigned char)in[0];

    if (a < 0x80) {
        out[0] = (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : (char)a;
        return 1;
    }
    if (left < 2) { out[0] = (char)a; return 1; }
    unsigned char b = (unsigned char)in[1];

    if (a == 0xC3 && b >= 0x80 && b <= 0x9E && b != 0x97) {
        out[0] = (char)a;
        out[1] = (char)(b + 0x20);
        return 2;
    }
    if ((a == 0xC4 || a == 0xC5) && b >= 0x80 && b <= 0xBF) {
        unsigned int cp = 0x100 + ((unsigned)(a - 0xC4) << 6) + (b - 0x80);
        /*
         * Latin Extended-A is upper/lower PAIRS, but the parity flips twice:
         * U+0100–U+0137 and U+014A–U+0177 pair even-then-odd, while
         * U+0139–U+0148 and U+0179–U+017E pair odd-then-even. Assuming one
         * parity throughout leaves `Ń` (U+0143) uppercase, which is how this
         * was found — `Kazimierz MichaŃowski` in a resolved link.
         */
        unsigned int lower = cp;
        if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) {
            if ((cp & 1) == 0) lower = cp + 1;
        } else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
            if ((cp & 1) == 1) lower = cp + 1;
        }
        out[0] = (char)(0xC4 + ((lower - 0x100) >> 6));
        out[1] = (char)(0x80 + ((lower - 0x100) & 0x3F));
        return 2;
    }
    /* Latin Extended Additional — Ḫ and friends, which a transliterated corpus
     * is full of. Same even/odd pairing throughout the block. */
    if (a == 0xE1 && left >= 3) {
        unsigned char c2 = (unsigned char)in[2];
        if (b == 0xB8 || b == 0xB9 || b == 0xBA) {
            unsigned int cp = 0x1E00 + ((unsigned)(b - 0xB8) << 6) + (c2 - 0x80);
            if (cp <= 0x1E95 && (cp & 1) == 0) cp++;
            out[0] = (char)a;
            out[1] = (char)(0xB8 + ((cp - 0x1E00) >> 6));
            out[2] = (char)(0x80 + ((cp - 0x1E00) & 0x3F));
            return 3;
        }
    }

    /* Anything else is copied through: its own bytes, unchanged. */
    size_t n = (a & 0xE0) == 0xC0 ? 2 : (a & 0xF0) == 0xE0 ? 3 : (a & 0xF8) == 0xF0 ? 4 : 1;
    if (n > left) n = left;
    for (size_t i = 0; i < n; i++) out[i] = in[i];
    return n;
}
