/*
 * linkify-it, in C.
 *
 * WHY A PORT AND NOT A HEURISTIC. Five of the eight documents that still
 * differed from mdy-docs came down to URL boundaries, and each time the
 * hand-rolled rule was "close": a trailing comma kept in one place and dropped
 * in another, a hyphen in the last label, a dot at the end of a sentence.
 * Those are not a rule anyone guesses — they are linkify-it's, and the only
 * way to have them is to have them.
 *
 * WHY NOT ITS REGEXES. The obvious route is to compile linkify's own patterns
 * with baru-re, which lamassu already links and which speaks the dialect they
 * need. It does not work: the patterns inline the Unicode classes rather than
 * using \p{...}, so `http_validator` alone is 31 KB and wants ~460 character
 * classes against baru-re's 256. Raising that limit segfaults. Measured, then
 * reverted.
 *
 * That inlining is also what makes a C port SMALL. Those 31 KB are Z, P and Cc
 * written out longhand; here they are three table lookups, and what is left is
 * the grammar itself — a few hundred lines.
 *
 * WHAT IS IMPLEMENTED is what `new LinkifyIt()` does with no options, which is
 * what mdy-docs constructs:
 *
 *   fuzzyLink  FALSE by default — a bare `example.com` is NOT a link, which
 *              removes the TLD list from everything except fuzzy email.
 *   schemas    http: https: ftp: (the web validator), // (relative), mailto:
 *   fuzzyEmail true — `user@host.tld` without a scheme.
 */
#include <stdlib.h>
#include <string.h>

#include "ucd.h"
#include "internal.h"

/* ---- character classes --------------------------------------------------- */

static int in_ranges(const UCDRange *r, int n, uint32_t cp) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < r[mid].start) hi = mid - 1;
        else if (cp > r[mid].end) lo = mid + 1;
        else return 1;
    }
    return 0;
}
#define IN(table, cp) in_ranges(table, (int)(sizeof table / sizeof(UCDRange)), (cp))

static int is_Z(uint32_t cp)  { return IN(ucd_gc_Separator_ranges, cp); }
static int is_P(uint32_t cp)  { return IN(ucd_gc_Punctuation_ranges, cp); }
static int is_Cc(uint32_t cp) { return IN(ucd_gc_Control_ranges, cp); }

/* `[><｜]` — linkify calls these text separators and excludes them from hosts
 * so that `<http://x>` and a fullwidth bar do not end up inside one. */
static int is_sep(uint32_t cp) { return cp == '>' || cp == '<' || cp == 0xFF5C; }

static int is_ZCc(uint32_t cp)  { return is_Z(cp) || is_Cc(cp); }
static int is_ZPCc(uint32_t cp) { return is_Z(cp) || is_P(cp) || is_Cc(cp); }

/* A "pseudo letter": anything that is not a separator and not Z/P/Cc. Broad on
 * purpose — an internationalised domain may hold almost any letter, and
 * linkify does not enumerate them. */
static int is_pseudo_letter(uint32_t cp) { return !is_sep(cp) && !is_ZPCc(cp); }

/* ---- a cursor ------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t len;
} Text;

/** The code point at `i`, and its width. Width 0 at the end. */
static uint32_t at(const Text *t, size_t i, size_t *width) {
    if (i >= t->len) { *width = 0; return 0; }
    uint32_t cp;
    *width = mdy_utf8_decode(t->s + i, t->len - i, &cp);
    return cp;
}

static int is_alpha(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

/* ---- host ---------------------------------------------------------------- */

/*
 * `xn--[a-z0-9-]{1,59}` — punycode, matched case-insensitively as the `i` flag
 * on the validators requires.
 */
static size_t match_xn(const Text *t, size_t i) {
    if (i + 4 > t->len) return 0;
    const char *p = t->s + i;
    if (!((p[0] == 'x' || p[0] == 'X') && (p[1] == 'n' || p[1] == 'N') &&
          p[2] == '-' && p[3] == '-')) return 0;
    size_t n = 4, taken = 0;
    while (i + n < t->len && taken < 59) {
        char c = t->s[i + n];
        if (is_alpha((unsigned char)c) || is_digit((unsigned char)c) || c == '-') { n++; taken++; }
        else break;
    }
    return taken >= 1 ? n : 0;
}

/*
 * One label: `xn--…` or a pseudo letter, or a pseudo letter followed by up to
 * 61 of (hyphen | pseudo letter) and then a pseudo letter. In other words a
 * label may hold hyphens but may not begin or end with one.
 */
static size_t match_domain(const Text *t, size_t i) {
    size_t xn = match_xn(t, i);
    if (xn) return xn;

    size_t w;
    uint32_t cp = at(t, i, &w);
    if (!w || !is_pseudo_letter(cp)) return 0;

    size_t n = w;              /* a single pseudo letter is a whole label */
    size_t last_good = n;
    size_t inner = 0;
    while (i + n < t->len && inner <= 61) {
        uint32_t c = at(t, i + n, &w);
        if (!w) break;
        if (c == '-') { n += w; inner++; continue; }
        if (!is_pseudo_letter(c)) break;
        n += w;
        inner++;
        last_good = n;         /* a label may not end on a hyphen */
    }
    return last_good;
}

/** `xn--…` or up to 63 pseudo letters — the last label of a fuzzy host. */
static size_t match_domain_root(const Text *t, size_t i) {
    size_t xn = match_xn(t, i);
    if (xn) return xn;
    size_t n = 0, taken = 0, w;
    while (i + n < t->len && taken < 63) {
        uint32_t cp = at(t, i + n, &w);
        if (!w || !is_pseudo_letter(cp)) break;
        n += w;
        taken++;
    }
    return n;
}

/** `(?::(?:6(?:[0-4]\d{3}|5(?:[0-4]\d{2}|5(?:[0-2]\d|3[0-5])))|[1-5]?\d{1,4}))?`
 * — a port number, which is to say 0-65535 and nothing longer. */
static size_t match_port(const Text *t, size_t i) {
    if (i >= t->len || t->s[i] != ':') return 0;
    size_t n = 1, digits = 0;
    while (i + n < t->len && digits < 5 && is_digit((unsigned char)t->s[i + n])) { n++; digits++; }
    if (digits == 0) return 0;

    /* The regex bounds the value; a five-digit port over 65535 is not one. */
    if (digits == 5) {
        unsigned long v = strtoul(t->s + i + 1, NULL, 10);
        if (v > 65535) return 0;
    }
    return n;
}

/*
 * `(?=$|sep|ZPCc)(?!-|_|:\d|\.-|\.(?!$|ZPCc))`
 *
 * The host must END here — the next character is the end, a separator, or
 * Z/P/Cc — and must not end in a way that means it was still going: a trailing
 * hyphen or underscore, a colon before a digit (an unmatched port), or a dot
 * followed by anything that is not itself a terminator.
 */
static int host_terminates(const Text *t, size_t i) {
    size_t w;
    uint32_t cp = at(t, i, &w);
    if (w && !is_sep(cp) && !is_ZPCc(cp)) return 0;

    if (!w) return 1;                       /* end of input */
    if (cp == '-' || cp == '_') return 0;
    if (cp == ':') {
        size_t w2;
        uint32_t next = at(t, i + w, &w2);
        if (w2 && is_digit(next)) return 0;
    }
    if (cp == '.') {
        size_t w2;
        uint32_t next = at(t, i + w, &w2);
        if (next == '-') return 0;
        if (w2 && !is_sep(next) && !is_ZPCc(next)) return 0;
    }
    return 1;
}

/* ---- path ---------------------------------------------------------------- */

/** A bracket pair nested up to four deep, as linkify's nestedPairRE builds. */
static size_t match_pair(const Text *t, size_t i, char open, char close, int depth) {
    if (i >= t->len || t->s[i] != open || depth > 4) return 0;
    size_t n = 1, taken = 0;
    while (i + n < t->len && taken < 1000) {
        char c = t->s[i + n];
        if (c == close) return n + 1;
        if (c == open) {
            size_t inner = match_pair(t, i + n, open, close, depth + 1);
            if (!inner) return 0;
            n += inner;
            taken++;
            continue;
        }
        size_t w;
        uint32_t cp = at(t, i + n, &w);
        if (!w || is_Z(cp) || is_Cc(cp)) return 0;
        n += w;
        taken++;
    }
    return 0;
}

/** A quoted run of up to 100 characters, neither Z nor Cc inside. */
static size_t match_quoted(const Text *t, size_t i, char quote) {
    if (i >= t->len || t->s[i] != quote) return 0;
    size_t n = 1, taken = 0;
    while (i + n < t->len && taken < 100) {
        if (t->s[i + n] == quote) return n + 1;
        size_t w;
        uint32_t cp = at(t, i + n, &w);
        if (!w || is_Z(cp) || is_Cc(cp)) return 0;
        n += w;
        taken++;
    }
    return 0;
}

/*
 * The path, and the reason a URL knows where a sentence ends.
 *
 * Every alternative below is one of linkify's, in its order, and the
 * conditional ones are the whole point: `,` and `;` continue the path only
 * when something follows them, `.` only when what follows is neither another
 * dot nor the end, `!` and `?` only when not doubled. That is what leaves the
 * full stop out of "see http://example.com." while keeping the comma inside
 * "http://x.com/a,b".
 */
static size_t match_path(const Text *t, size_t i) {
    size_t w;
    uint32_t first = at(t, i, &w);
    if (!w) return 0;

    if (first == '/' && !(i + 1 < t->len)) return 1;      /* a bare trailing slash */
    if (first != '/' && first != '?' && first != '#') return 0;

    size_t n = w;
    size_t taken = 0;
    while (i + n < t->len && taken < 10000) {
        size_t step = 0;
        char c = t->s[i + n];

        if ((step = match_pair(t, i + n, '[', ']', 0))) { n += step; taken++; continue; }
        if ((step = match_pair(t, i + n, '(', ')', 0))) { n += step; taken++; continue; }
        if ((step = match_pair(t, i + n, '{', '}', 0))) { n += step; taken++; continue; }
        if ((step = match_quoted(t, i + n, '"')))       { n += step; taken++; continue; }
        if ((step = match_quoted(t, i + n, '\'')))      { n += step; taken++; continue; }

        uint32_t cp = at(t, i + n, &w);
        size_t w2;
        uint32_t next = at(t, i + n + w, &w2);

        if (c == '\'' && (next == '-' || (w2 && is_pseudo_letter(next)))) { n += w; taken++; continue; }

        if (c == '.') {
            /* `\.{2,20}[:]?[a-zA-Z0-9%/&]` — a run of dots that resumes. */
            size_t dots = 0;
            while (i + n + dots < t->len && t->s[i + n + dots] == '.' && dots < 20) dots++;
            if (dots >= 2) {
                size_t k = n + dots;
                if (i + k < t->len && t->s[i + k] == ':') k++;
                if (i + k < t->len) {
                    char after = t->s[i + k];
                    if (is_alpha((unsigned char)after) || is_digit((unsigned char)after) ||
                        after == '%' || after == '/' || after == '&') {
                        n = k + 1;
                        taken++;
                        continue;
                    }
                }
            }
            /* `\.(?!ZCc|[.]|$)` — a single dot, if something real follows. */
            if (w2 && next != '.' && !is_ZCc(next)) { n += w; taken++; continue; }
            break;
        }

        if (c == '-') {                                   /* `\-{1,20}` */
            size_t run = 0;
            while (i + n + run < t->len && t->s[i + n + run] == '-' && run < 20) run++;
            n += run;
            taken++;
            continue;
        }

        if (c == ',' || c == ';') {                       /* `,(?!ZCc|$)` */
            if (w2 && !is_ZCc(next)) { n += w; taken++; continue; }
            break;
        }

        if (c == '!') {                                   /* `\!{1,20}(?!ZCc|[!]|$)` */
            size_t run = 0;
            while (i + n + run < t->len && t->s[i + n + run] == '!' && run < 20) run++;
            size_t w3;
            uint32_t after = at(t, i + n + run, &w3);
            if (w3 && after != '!' && !is_ZCc(after)) { n += run; taken++; continue; }
            break;
        }

        if (c == '?') {                                   /* `\?(?!ZCc|[?]|$)` */
            if (w2 && next != '?' && !is_ZCc(next)) { n += w; taken++; continue; }
            break;
        }

        if (c == '\\' || c == '/' || c == ':' || c == '%' || c == '@' ||
            c == '#' || c == '&' || c == '=' || c == '_' || c == '~' || c == '*') {
            n += w;
            taken++;
            continue;
        }

        /* `(?!path_terminator).` — anything that is not ZPCc or a separator. */
        if (!is_ZPCc(cp) && !is_sep(cp)) { n += w; taken++; continue; }
        break;
    }
    return n;
}

/* ---- the validators ------------------------------------------------------ */

/** `(?:(?:domain\.){0,10}domain)port host_terminator` — the host of an
 * explicit-scheme URL, which needs no TLD because the scheme said so. */
static size_t match_url_host_port(const Text *t, size_t i) {
    size_t n = 0;
    for (int label = 0; label <= 10; label++) {
        size_t d = match_domain(t, i + n);
        if (!d) return 0;
        n += d;
        if (i + n < t->len && t->s[i + n] == '.') {
            /* Another label may follow — but only if one actually does. */
            size_t save = n;
            n++;
            if (!match_domain(t, i + n)) { n = save; break; }
            continue;
        }
        break;
    }
    n += match_port(t, i + n);
    if (!host_terminates(t, i + n)) return 0;
    return n;
}

/** `http:` / `https:` / `ftp:` — `//` then a host then a path. `pos` is just
 * past the scheme. */
static size_t validate_web(const Text *t, size_t pos) {
    if (pos + 2 > t->len || t->s[pos] != '/' || t->s[pos + 1] != '/') return 0;
    size_t n = 2;
    size_t host = match_url_host_port(t, pos + n);
    if (!host) return 0;
    n += host;
    n += match_path(t, pos + n);
    return n;
}

/*
 * `//` on its own — a protocol-relative URL. The host must be `localhost` or a
 * dotted name ending in a root label, which is stricter than the explicit-
 * scheme case: without a scheme in front, `//foo` alone is not a URL.
 */
static size_t validate_relative(const Text *t, size_t pos) {
    /* `if (pos >= 3 && text[pos-3] === ':' or '/') return 0` — the `//` of an
     * `http://` belongs to that URL, not to a second, nested one. */
    if (pos >= 3 && (t->s[pos - 3] == ':' || t->s[pos - 3] == '/')) return 0;

    size_t n = 0;
    if (pos + 9 <= t->len && memcmp(t->s + pos, "localhost", 9) == 0) {
        n = 9;
    } else {
        /*
         * `(?:domain\.){1,10}domain_root` — greedy, WITH BACKTRACKING, and the
         * backtracking is not decoration. `//E.J. Brill` wants one label and a
         * root, not two labels: taking `E.` and `J.` leaves a space where the
         * root has to be, and a loop that only ever goes forwards concludes
         * there is no link at all. linkify finds `//E.J` there.
         */
        size_t ends[11];
        int count = 0;
        size_t at_n = 0;
        while (count < 10) {
            size_t d = match_domain(t, pos + at_n);
            if (!d) break;
            if (pos + at_n + d >= t->len || t->s[pos + at_n + d] != '.') break;
            at_n += d + 1;
            ends[count++] = at_n;
        }
        if (count == 0) return 0;

        size_t root = 0;
        while (count > 0) {
            root = match_domain_root(t, pos + ends[count - 1]);
            if (root) break;
            count--;               /* one fewer label, try again */
        }
        if (!root) return 0;
        n = ends[count - 1] + root;
    }
    n += match_port(t, pos + n);
    if (!host_terminates(t, pos + n)) return 0;
    n += match_path(t, pos + n);
    return n;
}

/* ---- mail ---------------------------------------------------------------- */

/* `[-!#$%&'*+/=?^_`{|}~a-zA-Z0-9]` — the atom characters of a mail name. */
static int is_mail_atom(uint32_t c) {
    if (is_alpha(c) || is_digit(c)) return 1;
    switch (c) {
        case '-': case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '/': case '=': case '?': case '^': case '_':
        case '`': case '{': case '|': case '}': case '~':
            return 1;
        default:
            return 0;
    }
}

/** A mail name ending at `end`, scanning BACKWARDS — which is how linkify does
 * it, because the `@` is what is found first. Returns where it starts. */
static size_t mail_name_start(const Text *t, size_t end) {
    size_t i = end;
    while (i > 0) {
        unsigned char c = (unsigned char)t->s[i - 1];
        if (is_mail_atom(c)) { i--; continue; }
        /* A dot counts only between atoms, never at either edge. */
        if (c == '.' && i - 1 > 0 && i < end &&
            is_mail_atom((unsigned char)t->s[i - 2]) &&
            is_mail_atom((unsigned char)t->s[i])) { i--; continue; }
        break;
    }
    /* It may not start with a dot, and must not be empty. */
    while (i < end && t->s[i] == '.') i++;
    return i;
}

/** `(?:(?:domain\.){0,4}domain)host_terminator` — a mail host after an
 * explicit `mailto:`, which needs no dot at all. */
static size_t match_mail_host(const Text *t, size_t i) {
    size_t n = 0;
    for (int label = 0; label <= 4; label++) {
        size_t d = match_domain(t, i + n);
        if (!d) return 0;
        n += d;
        if (i + n < t->len && t->s[i + n] == '.') {
            size_t save = n;
            n++;
            if (!match_domain(t, i + n)) { n = save; break; }
            continue;
        }
        break;
    }
    return host_terminates(t, i + n) ? n : 0;
}

/** `(?:domain[.]){1,4}domain_root` — a FUZZY mail host, which does need a dot,
 * since nothing else says this is an address. */
static size_t match_fuzzy_mail_host(const Text *t, size_t i) {
    size_t ends[5];
    int count = 0;
    size_t n = 0;
    while (count < 4) {
        size_t d = match_domain(t, i + n);
        if (!d) break;
        if (i + n + d >= t->len || t->s[i + n + d] != '.') break;
        n += d + 1;
        ends[count++] = n;
    }
    if (count == 0) return 0;
    size_t root = 0;
    while (count > 0) {
        root = match_domain_root(t, i + ends[count - 1]);
        if (root) break;
        count--;
    }
    if (!root) return 0;
    n = ends[count - 1] + root;
    return host_terminates(t, i + n) ? n : 0;
}

/* ---- finding schemas ----------------------------------------------------- */

/*
 * `(^|(?!_)(?:[><｜]|ZPCc))(schema)` — a scheme counts only at the start or
 * after a separator, and specifically NOT after an underscore. `_http://x` is
 * not a link.
 */
static int schema_position_ok(const Text *t, size_t i) {
    if (i == 0) return 1;
    size_t back = i;
    while (back > 0 && ((unsigned char)t->s[back - 1] & 0xC0) == 0x80) back--;
    if (back > 0) back--;
    size_t w;
    uint32_t before = at(t, back, &w);
    if (before == '_') return 0;
    return is_sep(before) || is_ZPCc(before);
}

typedef struct { const char *name; size_t len; int kind; } Schema;

#define SCHEMA_WEB      0
#define SCHEMA_RELATIVE 1
#define SCHEMA_MAILTO   2

static const Schema SCHEMAS[] = {
    { "https:", 6, SCHEMA_WEB },
    { "http:",  5, SCHEMA_WEB },
    { "ftp:",   4, SCHEMA_WEB },
    { "mailto:", 7, SCHEMA_MAILTO },
    { "//",     2, SCHEMA_RELATIVE },
};

/** Case-insensitive prefix compare, which the `i` flag on schema_search wants. */
static int prefix_ci(const Text *t, size_t i, const char *want, size_t n) {
    if (i + n > t->len) return 0;
    for (size_t k = 0; k < n; k++) {
        char a = t->s[i + k], b = want[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

size_t mdy_find_links(const char *text, size_t len, mdy_link *out, size_t max) {
    Text t = { text, len };
    size_t found = 0;

    for (size_t i = 0; i < len && found < max;) {
        size_t w;
        at(&t, i, &w);
        if (!w) break;

        for (size_t s = 0; s < sizeof SCHEMAS / sizeof SCHEMAS[0]; s++) {
            const Schema *sc = &SCHEMAS[s];
            if (!prefix_ci(&t, i, sc->name, sc->len)) continue;
            if (!schema_position_ok(&t, i)) continue;

            size_t pos = i + sc->len;
            size_t n;
            if (sc->kind == SCHEMA_RELATIVE) n = validate_relative(&t, pos);
            else if (sc->kind == SCHEMA_MAILTO) {
                /* `mail_name@mail_host`, both required. */
                size_t name = pos;
                while (name < t.len && is_mail_atom((unsigned char)t.s[name])) name++;
                while (name < t.len && t.s[name] == '.' && name + 1 < t.len &&
                       is_mail_atom((unsigned char)t.s[name + 1])) {
                    name++;
                    while (name < t.len && is_mail_atom((unsigned char)t.s[name])) name++;
                }
                if (name == pos || name >= t.len || t.s[name] != '@') { n = 0; }
                else {
                    size_t host = match_mail_host(&t, name + 1);
                    n = host ? (name + 1 + host) - pos : 0;
                }
            }
            else n = validate_web(&t, pos);
            if (!n) continue;

            out[found].start = i;
            out[found].end = pos + n;
            out[found].mailto = 0;      /* an explicit `mailto:` is already there */
            found++;
            i = pos + n;
            goto next;
        }

        /*
         * Fuzzy email — `user@host.tld` with no scheme in front, which is on
         * by default where fuzzy LINKS are not. Found from the `@`, with the
         * name read backwards from it.
         */
        if (t.s[i] == '@') {
            size_t host = match_fuzzy_mail_host(&t, i + 1);
            if (host) {
                size_t start = mail_name_start(&t, i);
                if (start < i && (start == 0 || !is_mail_atom((unsigned char)t.s[start - 1]))) {
                    out[found].start = start;
                    out[found].end = i + 1 + host;
                    out[found].mailto = 1;   /* bare email: normalize() adds the scheme */
                    found++;
                    i = i + 1 + host;
                    continue;
                }
            }
        }
        i += w;
    next:;
    }
    return found;
}
