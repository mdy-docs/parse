#ifndef MDY_TEXT_H
#define MDY_TEXT_H

/*
 * Text primitives the parser needs and an embedder usually does too: Unicode
 * case, UTF-8, and the UTF-16 boundary a JavaScript engine's strings live on.
 *
 * These are public because getting them wrong is invisible until a document is
 * not in English. A host that lowercases only ASCII agrees with JavaScript on
 * every English word and disagrees on `İ` — and a search index built that way
 * silently loses the tail of a name.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A letter or a number, as `\p{L}\p{N}` means it — from the UCD, not a guess
 * at which ranges look alphabetic. */
int mdy_is_letter_or_number_cp(uint32_t cp);

/* Unicode's SIMPLE lowercase mapping: one code point to one code point.
 *
 * JavaScript's `toLowerCase` uses FULL casing, where a few characters become
 * several — `İ` (U+0130) is `i` followed by a combining dot above. The simple
 * mapping gives the `i` and not the dot, which agrees wherever a caller only
 * asks whether a character folds to a particular letter. A caller reproducing
 * `String.prototype.toLowerCase` exactly needs more than this. */
uint32_t mdy_lower_cp(uint32_t cp);

/* Whitespace as JavaScript means it — `String.trim` removes no-break spaces
 * and the Unicode separators, and an ASCII-only trim leaves them in. */
int mdy_is_js_space(uint32_t cp);
void mdy_trim(const char **s, size_t *len);
void mdy_trim_end(const char **s, size_t *len);

/* Decode one UTF-8 character, returning its width in bytes; an ill-formed
 * sequence is one byte and U+FFFD. Encode one, into 4 bytes of `out`. */
size_t mdy_utf8_decode(const char *p, size_t left, uint32_t *out);
size_t mdy_utf8_encode(uint32_t cp, char *out);

/* The UTF-16 boundary — what a JavaScript engine's strings are, and what
 * unist positions count in. Astral characters are two units, not one. */
size_t mdy_utf16_length(const char *s, size_t len);
size_t mdy_to_utf16(const char *s, size_t len, uint16_t *out, size_t cap);
size_t mdy_from_utf16(const uint16_t *units, size_t count, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MDY_TEXT_H */
