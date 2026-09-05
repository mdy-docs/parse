/*
 * hast -> HTML.
 *
 * A port of `hast-util-to-html`, which is what mdy-docs writes its pages with
 * (through `rehype-stringify`, with `allowDangerousHtml: true` and every other
 * option left at its default). Same input, same output, byte for byte: the
 * differential harness in test/compare-html.mjs holds it against the original
 * over the reference corpus.
 *
 * It is deliberately a SEPARATE translation unit from the parser and shares
 * nothing with it but the tree type. The parser reads text and produces a
 * tree; this reads a tree and produces text; neither needs the other, and a
 * tree that reaches here may have been built by anything — a transform,
 * another front end, a host embedding this library. test/html.c exercises it
 * on hand-built trees, with no parsing involved at all.
 *
 * WHAT IS NOT HERE, and why each is absent rather than forgotten:
 *
 *   - `omitOptionalTags`. Off in mdy-docs and off by default, and it is the
 *     largest part of the original (500 lines of two-sided sibling rules). A
 *     writer that omitted tags when asked not to would be wrong; one that
 *     cannot omit them is simply this one.
 *   - The SVG schema. `<svg>` switches property lookup to a second table
 *     mid-tree. Nothing in the corpus contains one, and the sanitize schema
 *     strips it. mdy_to_html writes SVG attributes by the HTML rules, which
 *     differ in case (`viewBox` stays `viewbox`).
 *   - `<template>`'s `content`. hast keeps a template's children off to one
 *     side; this tree model has no such field.
 *
 * Each is a refusal to guess, not an oversight. If one is needed it is a
 * feature to add, and the harness will say what it costs.
 */
#ifndef MDYHTML_H
#define MDYHTML_H

#include <stddef.h>
#include "mdyast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * Whether a `raw` node is written through as-is.
     *
     * mdy-docs passes `allowDangerousHtml: true`, because by the time a tree
     * reaches the writer the author IS the template and the template already
     * ran in a sandbox. With it off, a raw node is escaped like text.
     */
    int allow_dangerous_html;
} mdy_html_options;

/* allow_dangerous_html on, matching mdy-docs' own writer. */
void mdy_html_options_default(mdy_html_options *out);

/*
 * The tree as HTML. Caller frees. NULL only on allocation failure.
 *
 * `node` may be any node, not only a root — serialising an element on its own
 * is what a partial render wants.
 */
char *mdy_to_html(const mdy_node *node, const mdy_html_options *options);

#ifdef __cplusplus
}
#endif
#endif
