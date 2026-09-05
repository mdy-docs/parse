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
 * An HTML attribute name to its hast property name.
 *
 * This is `property-information`'s `find(html, name)`, which is what mdy-docs
 * calls, reproduced exactly rather than approximated — and the difference is
 * not academic. A hand-written table of the twenty names that "obviously"
 * matter got three separate things wrong, each of which changed real output:
 *
 *   - The lookup is case-INSENSITIVE. `SRC` is `src`, and `For` is `htmlFor`.
 *   - An UNKNOWN name is kept verbatim, not lowercased. `FOO` stays `FOO`.
 *   - `data-` camel-cases only where a dash is followed by a LOWERCASE letter,
 *     so `DATA-x-Y` is `dataX-Y` and not `dataXY`.
 *
 * The last two are only reachable through malformed markup, which is exactly
 * where they were found: an unescaped quote inside an `alt=""` turns the rest
 * of a caption into bare attributes, and one name resolved differently was
 * enough to reorder the whole element's properties downstream.
 *
 * MDY_PROPS is generated from the library itself — scripts/generate_props.mjs.
 */
#include "props_table.h"

/** ASCII lowercase. Attribute names are ASCII by construction: parse_element
 * only admits [A-Za-z_:][A-Za-z0-9._:-]* as a name. */
static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** The schema's property name for a normalized attribute name, or NULL. */
static const char *schema_lookup(const char *normal, size_t len) {
    size_t lo = 0, hi = MDY_PROPS_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *cand = MDY_PROPS[mid].normal;
        size_t clen = strlen(cand);
        size_t n = clen < len ? clen : len;
        int cmp = memcmp(cand, normal, n);
        if (cmp == 0) cmp = clen < len ? -1 : (clen > len ? 1 : 0);
        if (cmp == 0) return MDY_PROPS[mid].property;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/** `/^data[-\w.:]+$/` on the ORIGINAL spelling, as the library tests it. */
static int data_shaped(const char *name, size_t len) {
    for (size_t i = 4; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
        if (!ok) return 0;
    }
    return len > 4;
}

/**
 * The `data-` branch: drop `data-`, upper-case a letter following each dash
 * that is followed by a LOWERCASE one (`/-[a-z]/g`), then capitalise the first
 * character and prefix a literal lowercase `data`. Written into `out`.
 */
static size_t data_property(const char *name, size_t len, char *out) {
    memcpy(out, "data", 4);
    size_t o = 4;
    size_t i = 5;                       /* past "data-" */
    int first = 1;
    while (i < len) {
        char c = name[i];
        if (c == '-' && i + 1 < len && name[i + 1] >= 'a' && name[i + 1] <= 'z') {
            out[o++] = (char)(name[i + 1] - 'a' + 'A');
            i += 2;
        } else {
            out[o++] = c;
            i += 1;
        }
        if (first) {                    /* `rest.charAt(0).toUpperCase()` */
            out[4] = (char)((out[4] >= 'a' && out[4] <= 'z') ? out[4] - 'a' + 'A' : out[4]);
            first = 0;
        }
    }
    out[o] = '\0';
    return o;
}

const char *mdy_hast_name(mdy_doc *doc, const char *name, size_t len) {
    char normal[256];
    if (len < sizeof normal) {
        for (size_t i = 0; i < len; i++) normal[i] = lower_ascii(name[i]);
        normal[len] = '\0';

        const char *found = schema_lookup(normal, len);
        if (found) return mdy_intern(&doc->arena, &doc->names, found, strlen(found));

        if (len > 4 && memcmp(normal, "data", 4) == 0 && data_shaped(name, len) &&
            name[4] == '-') {
            char buf[256];
            size_t n = data_property(name, len, buf);
            return mdy_intern(&doc->arena, &doc->names, buf, n);
        }
    }
    /* Unknown: hast keeps the author's spelling, case and all. */
    return mdy_intern(&doc->arena, &doc->names, name, len);
}

/* ---- the schema ----------------------------------------------------------- */

/*
 * defaultSchema from mdy-docs' src/parse/sanitize.js, generated by
 * scripts/generate_schema.mjs.
 *
 * Two rules that a "drop what is not allowed" reading collapses into one, and
 * they are not the same thing:
 *
 *   - A tag in `strip` disappears, and everything indented under it with it.
 *     There are twenty of them, `script` and `style` among them, and the whole
 *     list was missing here.
 *   - A tag merely absent from `tagNames` becomes a <div> and KEEPS its
 *     content. Dropping it silently loses an author's text for a typo in a
 *     tag name.
 *
 * Attributes are judged by the name the AUTHOR wrote, lowercased — the schema
 * is written in raw HTML attribute names, before hast respells them.
 */
#include "schema_table.h"

static int in_list(const char *const *list, size_t count, const char *s, size_t len) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t clen = strlen(list[mid]);
        size_t n = clen < len ? clen : len;
        int cmp = memcmp(list[mid], s, n);
        if (cmp == 0) cmp = clen < len ? -1 : (clen > len ? 1 : 0);
        if (cmp == 0) return 1;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int mdy_tag_allowed(const char *tag) {
    return in_list(MDY_TAG_NAMES, MDY_TAG_NAMES_COUNT, tag, strlen(tag));
}

int mdy_tag_stripped(const char *tag) {
    return in_list(MDY_STRIP, MDY_STRIP_COUNT, tag, strlen(tag));
}

/** `/^aria-[a-z-]+$/` and `/^data-[\w-]+$/`, the schema's two regex rules.
 * They are anchored and they are not the same character class, which matters:
 * `aria-Label` does not match, `data-Label` does. */
static int matches_prefixed(const char *name, size_t len) {
    if (len > 5 && memcmp(name, "aria-", 5) == 0) {
        for (size_t i = 5; i < len; i++) {
            char c = name[i];
            if (!((c >= 'a' && c <= 'z') || c == '-')) return 0;
        }
        return 1;
    }
    if (len > 5 && memcmp(name, "data-", 5) == 0) {
        for (size_t i = 5; i < len; i++) {
            char c = name[i];
            int word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            if (!(word || c == '-')) return 0;
        }
        return 1;
    }
    return 0;
}

/** `name` is the RAW html attribute name, lowercased — that is what the schema
 * is written in terms of, and what sanitizeOpener matches on. */
int mdy_attr_allowed(const char *tag, const char *name, size_t len) {
    if (matches_prefixed(name, len)) return 1;
    for (size_t i = 0; i < MDY_TAG_ATTRS_COUNT; i++) {
        const char *t = MDY_TAG_ATTRS[i].tag;
        if (!(t[0] == '*' && t[1] == '\0') && strcmp(t, tag) != 0) continue;
        const char *a = MDY_TAG_ATTRS[i].attr;
        if (strlen(a) == len && memcmp(a, name, len) == 0) return 1;
    }
    return 0;
}

/*
 * A URL whose protocol the schema permits. Anything with no scheme at all is
 * relative and fine; a scheme that is not on the list makes the attribute
 * disappear, which is the whole point — `javascript:` in an author's `href`
 * must not survive into the tree.
 */
int mdy_protocol_allowed_n(const char *attr, size_t attr_len,
                           const char *value, size_t len) {
    char name[64];
    if (attr_len >= sizeof name) return 1;
    memcpy(name, attr, attr_len);
    name[attr_len] = '\0';
    return mdy_protocol_allowed(name, value, len);
}

int mdy_protocol_allowed(const char *attr, const char *value, size_t len) {
    int listed = 0;
    for (size_t i = 0; i < MDY_PROTOCOLS_COUNT; i++)
        if (strcmp(MDY_PROTOCOLS[i].attr, attr) == 0) { listed = 1; break; }
    if (!listed) return 1;

    size_t colon = 0;
    while (colon < len && value[colon] != ':') {
        char c = value[colon];
        /* A `/`, `?` or `#` before any colon means there is no scheme. */
        if (c == '/' || c == '?' || c == '#') return 1;
        colon++;
    }
    if (colon == len) return 1;   /* no colon: relative */

    for (size_t i = 0; i < MDY_PROTOCOLS_COUNT; i++) {
        if (strcmp(MDY_PROTOCOLS[i].attr, attr) != 0) continue;
        const char *proto = MDY_PROTOCOLS[i].protocol;
        size_t n = strlen(proto);
        if (n != colon) continue;
        size_t k = 0;
        while (k < n) {
            char a = value[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != proto[k]) break;
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
