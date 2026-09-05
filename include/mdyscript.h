/*
 * The MDY script layer, compiled.
 *
 * A document's `%` and `%%` lines are JavaScript, and its content lines are
 * template literals. `mdy_script_compile` turns a whole document into the one
 * run of JavaScript statements that produces its lines:
 *
 *     % for (const name of names) {        const __out = []
 *     - {{ name }}                 ──►     for (const name of names) {
 *     % }                                    __out.push([1, `- ${name}`])
 *                                          }
 *
 * A port of mdy-docs' src/parse/script.js, and checked against it the way
 * everything else here is: same document in, byte-identical source out, over
 * the whole corpus.
 *
 * WHY THIS IS A SEPARATE STAGE, and separate from the parser too: what runs
 * the statements is somebody else's business. This produces source; a host
 * hands it to whatever sandbox it has, and hands the `__out` it gets back to
 * `mdy_script_output`. The native backend compiles it once with lamassu, keeps
 * the bytecode, and calls it per render with the request as a VALUE — which is
 * only possible because the statements never mention the data.
 *
 * NOT here: `scriptBrackets`, which pairs up the brackets of a document's code
 * for an editor to fold and highlight. It is tooling rather than compilation,
 * and nothing in a build asks for it.
 */
#ifndef MDYSCRIPT_H
#define MDYSCRIPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdy_script mdy_script;

/*
 * Compile a document. `len` may be 0 for a NUL-terminated string.
 *
 * Never fails on malformed input: this is a text transformation, and code that
 * does not parse is code the engine will report on when it runs. NULL only on
 * allocation failure.
 */
mdy_script *mdy_script_compile(const char *text, size_t len);

/* The statements. NUL-terminated; `len` may be NULL. */
const char *mdy_script_source(const mdy_script *script, size_t *len);

/* How many lines the document had, and whether each was code rather than
 * content — which is what a caller needs to map a runtime error back to the
 * line somebody wrote. */
size_t mdy_script_line_count(const mdy_script *script);
int mdy_script_is_code(const mdy_script *script, size_t line);

void mdy_script_free(mdy_script *script);

/*
 * Whether a line is code rather than content — `/^[ \t]*%(.*)$/`.
 *
 * Leading space carries no meaning: a `%` line is taken out of the document
 * before any column is counted, so how far in the author writes their code is
 * their own business and none of the markup's.
 */
int mdy_script_is_line(const char *line, size_t len);

#ifdef __cplusplus
}
#endif
#endif
