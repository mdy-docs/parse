/*
 * Markdown to hast — the second front end.
 *
 * mdy-docs speaks two markup languages and composes ONE tree type. `.md` text
 * arrives as hast exactly as `.mdy` does, so everything after this point —
 * composition, `transform`, the TOC, the HTML writer — sees one kind of thing
 * from two kinds of file. That is the whole of this file's job, and it stops
 * at the tree.
 *
 * CommonMark plus the GFM set, through md4c (third_party/md4c, MIT):
 * `MD_DIALECT_GITHUB` gives tables, strikethrough, task lists, permissive
 * autolinks and footnotes. What md4c reports as callbacks, this turns into
 * the same nodes mdy-docs' `remarkParse → remarkGfm → remarkRehype` chain
 * produces — including the newline padding between block children, which is
 * remark-rehype's and not decoration.
 *
 * Headings take their ids from mdy's own slugger, so a `#anchor` written in
 * one format lands on a heading written in the other.
 *
 * The result is an ordinary mdy_doc: mdy_root, mdy_to_json and mdy_to_html
 * all work on it, and mdy_free is the only cleanup.
 */
#ifndef MDYMARKDOWN_H
#define MDYMARKDOWN_H

#include <stddef.h>
#include "mdyast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `len` may be 0 for a NUL-terminated string. NULL only if md4c refuses the
 * document or an allocation fails. */
mdy_doc *mdy_markdown_parse(const char *text, size_t len);

#ifdef __cplusplus
}
#endif
#endif
