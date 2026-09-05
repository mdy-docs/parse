/*
 * YAML, for reading a document's front matter, its ```data fences, and the
 * .yaml files a site is built from.
 *
 * YAML 1.2, core schema. Correct rather than compatible: where an
 * implementation and the specification disagree this follows the
 * specification, and where a construct is not supported it says so with an
 * error instead of guessing. A parser that silently mis-reads data is worse
 * than one that refuses it — the data is what a site is built from.
 *
 * WHAT IT READS
 *
 *   - block mappings and sequences, nested by indentation, including a
 *     sequence at its key's own indent and the compact `- key: value` form
 *   - plain, single-quoted and double-quoted scalars, each of which may run
 *     across lines and fold
 *   - block scalars: literal `|` and folded `>`, with the chomping indicators
 *     `-` and `+` and an explicit indentation indicator
 *   - flow mappings and sequences, nested, and spanning lines
 *   - comments
 *   - the core schema's scalar resolution: null, booleans, integers (decimal,
 *     `0o` octal, `0x` hexadecimal), floats including `.inf` and `.nan`, and
 *     everything else a string. `Yes` is a STRING — that is 1.1's boolean, not
 *     1.2's, and the difference decides real front matter here.
 *
 * WHAT IT REFUSES, with an error naming the line
 *
 *   - anchors, aliases and merge keys (`&`, `*`, `<<`)
 *   - explicit tags (`!`, `!!`)
 *   - explicit keys (`?`) and complex keys
 *   - multiple documents in one stream, and directives (`%YAML`, `%TAG`)
 *
 * None of the four appears anywhere in the corpus this was built for — 179
 * YAML blocks across 4.9 MB were surveyed before a line was written — and each
 * is a feature to add rather than a corner to guess at.
 */
#ifndef MDYYAML_H
#define MDYYAML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MDY_YAML_NULL = 0,
    MDY_YAML_BOOL,
    MDY_YAML_NUMBER,
    MDY_YAML_STRING,
    MDY_YAML_SEQUENCE,
    MDY_YAML_MAPPING,
} mdy_yaml_type;

typedef struct mdy_yaml mdy_yaml;
typedef struct mdy_yaml_node mdy_yaml_node;

/*
 * Parse a whole stream. `len` may be 0 for a NUL-terminated string.
 *
 * On failure returns NULL and writes a message into `error` (which may be
 * NULL), of the shape `line 12: what went wrong`. An empty stream is not a
 * failure: it parses to a null node, which is what `parse("")` should give.
 */
mdy_yaml *mdy_yaml_parse(const char *text, size_t len, char *error, size_t error_len);
const mdy_yaml_node *mdy_yaml_root(const mdy_yaml *doc);
void mdy_yaml_free(mdy_yaml *doc);

mdy_yaml_type mdy_yaml_type_of(const mdy_yaml_node *node);

/* A string's bytes (UTF-8, NUL-terminated; `len` may be NULL), or NULL when
 * the node is not a string. */
const char *mdy_yaml_string(const mdy_yaml_node *node, size_t *len);
double mdy_yaml_number(const mdy_yaml_node *node);
int mdy_yaml_bool(const mdy_yaml_node *node);

/* Sequences and mappings, in the order they were written. */
size_t mdy_yaml_count(const mdy_yaml_node *node);
const mdy_yaml_node *mdy_yaml_at(const mdy_yaml_node *node, size_t index);
const char *mdy_yaml_key(const mdy_yaml_node *node, size_t index, size_t *len);
const mdy_yaml_node *mdy_yaml_value(const mdy_yaml_node *node, size_t index);

/* The value for `key`, or NULL. Linear, which is what a front matter block
 * wants; a caller with a large mapping should walk it once instead. */
const mdy_yaml_node *mdy_yaml_get(const mdy_yaml_node *node, const char *key);

/*
 * The tree as JSON, written the way `JSON.stringify` writes it, so two
 * implementations can be compared byte for byte. That is what it is for.
 * Caller frees. NULL on allocation failure.
 */
char *mdy_yaml_to_json(const mdy_yaml_node *node);

#ifdef __cplusplus
}
#endif
#endif
