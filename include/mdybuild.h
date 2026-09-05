/*
 * Building a tree by hand.
 *
 * The parser and the markdown front end both make hast trees, and so does
 * anything that takes one apart and puts it back — an engine applying a
 * document's own `transform`, a host constructing a node to splice in. That
 * is the same handful of operations either way, so they are public rather
 * than the parser's private business.
 *
 * Everything a document owns lives in its arena: `mdy_free` is the only
 * cleanup, and no node owns anything on its own.
 */
#ifndef MDYBUILD_H
#define MDYBUILD_H

#include <stddef.h>
#include "mdyast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An empty document — an arena and a root, with nothing read into it. */
mdy_doc *mdy_doc_new(void);

mdy_node *mdy_new_element(mdy_doc *doc, const char *tag, size_t tag_len);
mdy_node *mdy_new_text(mdy_doc *doc, const char *text, size_t len);
void mdy_append(mdy_node *parent, mdy_node *child);

/*
 * Properties are an OBJECT: setting a name that is already there replaces its
 * value and keeps its position, which is what hast does and what decides the
 * order attributes are written in.
 */
void mdy_set_string(mdy_doc *doc, mdy_node *el, const char *name, const char *value, size_t value_len);
void mdy_set_number(mdy_doc *doc, mdy_node *el, const char *name, double value);
void mdy_set_bool(mdy_doc *doc, mdy_node *el, const char *name, int value);

/* `className` is the one list-valued property, and it is APPENDED to — an
 * element can pick up classes from more than one rule. */
void mdy_add_class(mdy_doc *doc, mdy_node *el, const char *class_name);

#ifdef __cplusplus
}
#endif
#endif
