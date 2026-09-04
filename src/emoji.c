/*
 * Emoji: `:rocket:` and `:)`.
 *
 * The tables are generated (scripts/generate_emoji.mjs) from the same two
 * packages mdy-docs imports, so the two agree by construction rather than by
 * someone copying 2,235 entries carefully.
 *
 * The MATCHING is the interesting half, and both rules exist to stop false
 * positives that a naive scan produces constantly:
 *
 *   A shortcode must be a name gemoji knows. That is what keeps `12:30:45`
 *   from being read as one, and it is why this corpus's `:text:` and `:cts:`
 *   stay as they are.
 *
 *   An emoticon must stand on its own: something must have ended before it,
 *   and a letter or number must not follow it. Without the first rule `:/`
 *   turns the middle of `http://example.com` into a face.
 */
#include <string.h>

#include "internal.h"
#include "emoji_table.h"

static const char *lookup(const MdyEmoji *table, size_t count, const char *key, size_t len) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t klen = strlen(table[mid].key);
        int cmp = memcmp(table[mid].key, key, klen < len ? klen : len);
        if (cmp == 0) cmp = klen < len ? -1 : klen > len ? 1 : 0;
        if (cmp == 0) return table[mid].emoji;
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/** The character after a match must not be a letter or a number. */
static int ends_word(const char *p, size_t left) {
    if (left == 0) return 1;
    uint32_t cp;
    mdy_utf8_decode(p, left, &cp);
    return !mdy_is_letter_or_number_cp(cp);
}

const char *mdy_match_emoji(const char *p, size_t left, int at_boundary, size_t *consumed) {
    if (left < 2) return NULL;

    if (p[0] == ':') {
        /* `:name:` where name is [a-z0-9_+-], matched case-insensitively. */
        size_t n = 1;
        char lowered[64];
        size_t o = 0;
        while (n < left && p[n] != ':') {
            char c = p[n];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
            if (!ok || o == sizeof lowered) { o = 0; break; }
            lowered[o++] = c;
            n++;
        }
        if (o > 0 && n < left && p[n] == ':') {
            const char *found = lookup(MDY_SHORTCODES,
                                      sizeof MDY_SHORTCODES / sizeof MDY_SHORTCODES[0],
                                      lowered, o);
            if (found) { *consumed = n + 1; return found; }
        }
    }

    /* A character no emoticon starts with cannot start one, and checking that
     * first is the difference between a table search per position and a table
     * search per candidate — worth 2x on a real corpus. */
    if (!at_boundary) return NULL;
    unsigned char first = (unsigned char)p[0];
    if (first >= 128 || !MDY_EMOTICON_OPENER[first]) return NULL;

    /* Longest first, so `:-))` does not match `:-)` and leave a bracket. */
    size_t max = left < MDY_EMOTICON_LONGEST ? left : MDY_EMOTICON_LONGEST;
    for (size_t size = max; size > 1; size--) {
        const char *found = lookup(MDY_EMOTICONS,
                                   sizeof MDY_EMOTICONS / sizeof MDY_EMOTICONS[0], p, size);
        if (found && ends_word(p + size, left - size)) { *consumed = size; return found; }
    }
    return NULL;
}
