/*
 * Unicode: classification, case, and the UTF-8/UTF-16 boundary.
 *
 * THE TABLES ARE NOT OURS, and that is the point of this file. `\p{L}`,
 * `\p{N}` and simple lowercase are Unicode data, and a hand-written
 * approximation of them is wrong in ways that only show up in somebody's
 * language. This one was: an earlier cut treated "every non-ASCII byte" as a
 * letter, which kept en dashes in URLs, and lowercased only Latin-1 and Latin
 * Extended-A, which left `Ń` and `Ḫ` upper. Both produced links to nowhere.
 *
 * baru-re — the regex engine lamassu uses — already carries generated UCD
 * tables, so this reaches for those rather than inventing a third copy. Only
 * three are referenced (Letter, Number, simple lowercase) and the linker drops
 * the rest: 8 KB, against 619 KB if you go through lookup_unicode_property,
 * which pulls in every property there is.
 *
 * UTF-8 IS THE REPRESENTATION, everywhere: the source is UTF-8, the tree's text
 * is UTF-8, the JSON is UTF-8. Nothing converts unless something asks it to.
 *
 * That works because UTF-8 is self-synchronising — no byte of a multi-byte
 * character can be mistaken for ASCII — so every rule that looks for `|`, `-`
 * or `[[` can scan bytes and be right. What CANNOT scan bytes is anything that
 * asks a question ABOUT a character: is it a letter, what is its lowercase,
 * should it be deleted. Those decode first, and each of the three that did not
 * was a bug — the worst left two bytes of an en dash behind in a URL.
 *
 * The UTF-16 pair at the bottom is a BOUNDARY, not a representation. Every
 * JavaScript engine — QuickJS, lamassu, V8 — holds strings as UTF-16, so a
 * tree built here reaches JavaScript through that conversion, and unist
 * positions are counted in those units too. The reference corpus has 1,351
 * astral characters (Sumerian cuneiform), each ONE code point and TWO UTF-16
 * units, so anything conflating the counts is wrong on exactly those — which
 * is why they are what the tests use.
 */
#include <stdlib.h>
#include <string.h>

#include "ucd.h"
#include "internal.h"

/* ---- classification ------------------------------------------------------ */

static int in_ranges(const UCDRange *ranges, int count, uint32_t cp) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].start) hi = mid - 1;
        else if (cp > ranges[mid].end) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int mdy_is_letter_or_number_cp(uint32_t cp) {
    return in_ranges(ucd_gc_Letter_ranges,
                     (int)(sizeof ucd_gc_Letter_ranges / sizeof(UCDRange)), cp) ||
           in_ranges(ucd_gc_Number_ranges,
                     (int)(sizeof ucd_gc_Number_ranges / sizeof(UCDRange)), cp);
}

/*
 * The SIMPLE lowercase mapping, which is what `String.prototype.toLowerCase`
 * uses for all but a handful of characters. The exceptions are the ones that
 * lowercase to more than one code point (İ, and the Final_Sigma context rule);
 * they are in UCD_SPECIAL_CASING and are not handled here, because a mapping
 * that changes a string's length changes every offset after it and no document
 * in reach needs one.
 *
 * lamassu's own `toLowerCase` was ASCII-only when this was written, which
 * would have meant the same call inside and outside the sandbox disagreeing on
 * any non-ASCII string. That was a bug rather than a boundary and it is fixed
 * upstream (lamassu e38fcf9), over these same UCD tables — so the sandbox and
 * the host now answer alike.
 */
uint32_t mdy_lower_cp(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;

    int lo = 0, hi = (int)(sizeof UCD_SIMPLE_LOWERCASE / sizeof(SimpleCaseMapping)) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (UCD_SIMPLE_LOWERCASE[mid].cp == cp) return UCD_SIMPLE_LOWERCASE[mid].mapping;
        if (UCD_SIMPLE_LOWERCASE[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp;
}

/* ---- UTF-8 --------------------------------------------------------------- */

/*
 * Decode one character. Returns how many bytes it took and writes the code
 * point; an ill-formed sequence is one byte and U+FFFD, which is what every
 * conforming decoder does and what keeps a corrupt file from shifting every
 * offset after it.
 *
 * Overlong forms, surrogates encoded as three bytes (CESU-8), and anything
 * above U+10FFFF are all ill-formed and rejected — a surrogate that reached
 * the tree would be an unpaired one, and unpaired surrogates are how a string
 * stops round-tripping.
 */
size_t mdy_utf8_decode(const char *p, size_t left, uint32_t *out) {
    unsigned char a = (unsigned char)p[0];
    if (a < 0x80) { *out = a; return 1; }

    size_t need;
    uint32_t cp;
    if ((a & 0xE0) == 0xC0)      { need = 2; cp = a & 0x1F; }
    else if ((a & 0xF0) == 0xE0) { need = 3; cp = a & 0x0F; }
    else if ((a & 0xF8) == 0xF0) { need = 4; cp = a & 0x07; }
    else                         { *out = 0xFFFD; return 1; }

    if (need > left) { *out = 0xFFFD; return 1; }
    for (size_t i = 1; i < need; i++) {
        unsigned char b = (unsigned char)p[i];
        if ((b & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (b & 0x3F);
    }

    static const uint32_t MIN[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < MIN[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *out = 0xFFFD;
        return 1;
    }
    *out = cp;
    return need;
}

/** Encode one code point. `out` must hold 4 bytes. */
size_t mdy_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- UTF-16 -------------------------------------------------------------- */

/*
 * How many UTF-16 code units this UTF-8 string is — which is NOT how many
 * characters it is, and not how many bytes. It is what a JavaScript engine
 * calls `.length`, and what unist positions count in.
 */
size_t mdy_utf16_length(const char *s, size_t len) {
    size_t units = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += mdy_utf8_decode(s + i, len - i, &cp);
        units += cp > 0xFFFF ? 2 : 1;
    }
    return units;
}

size_t mdy_to_utf16(const char *s, size_t len, uint16_t *out, size_t cap) {
    size_t units = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += mdy_utf8_decode(s + i, len - i, &cp);
        if (cp > 0xFFFF) {
            /* A surrogate PAIR: one character, two units. The corpus is full
             * of them — every cuneiform sign is one — and a conversion that
             * truncates to a single unit turns 𒀭 into a lone high surrogate,
             * which no engine will hand back as the same string. */
            if (units + 2 > cap) return units;
            cp -= 0x10000;
            out[units++] = (uint16_t)(0xD800 + (cp >> 10));
            out[units++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (units + 1 > cap) return units;
            out[units++] = (uint16_t)cp;
        }
    }
    return units;
}

size_t mdy_from_utf16(const uint16_t *units, size_t count, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            /* An unpaired surrogate cannot be encoded. U+FFFD rather than a
             * refusal: a host handing one over has already lost that
             * character, and failing the whole document over it helps nobody. */
            cp = 0xFFFD;
        }
        char buf[4];
        size_t n = mdy_utf8_encode(cp, buf);
        if (o + n > cap) return o;
        memcpy(out + o, buf, n);
        o += n;
    }
    return o;
}

/*
 * Whitespace as JavaScript means it — `String.prototype.trim` removes all of
 * this, and a trim that only knows about space and tab does not.
 *
 * Not academic: a label in the reference corpus ends `"Obelisk"\u00a0`, and an
 * ASCII-only trim leaves the no-break space in the link's text. WhiteSpace and
 * LineTerminator from the spec: tab, vertical tab, form feed, space, no-break
 * space, zero-width no-break space, every Space_Separator, and the four line
 * terminators.
 */
int mdy_is_js_space(uint32_t cp) {
    switch (cp) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        case 0x20: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return 1;
        default:
            return cp >= 0x2000 && cp <= 0x200A;
    }
}

/** Trim JavaScript whitespace from both ends of a UTF-8 range. */
void mdy_trim(const char **s, size_t *len) {
    while (*len) {
        uint32_t cp;
        size_t w = mdy_utf8_decode(*s, *len, &cp);
        if (!mdy_is_js_space(cp)) break;
        *s += w;
        *len -= w;
    }
    while (*len) {
        /* Step back over one whole character: one byte back into a multi-byte
         * one is not a character. */
        size_t back = *len;
        while (back > 0 && ((unsigned char)(*s)[back - 1] & 0xC0) == 0x80) back--;
        if (back > 0) back--;
        uint32_t cp;
        mdy_utf8_decode(*s + back, *len - back, &cp);
        if (!mdy_is_js_space(cp)) break;
        *len = back;
    }
}
