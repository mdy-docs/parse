/*
 * Building the tree, and writing it out as JSON.
 *
 * The JSON is not a convenience: it is how this implementation is checked
 * against the JavaScript one. A 4,441-line parser cannot be ported by reading
 * it — it is ported by producing the same tree for a real corpus, document by
 * document, and diffing. Key order is fixed here and matched on the JS side by
 * test/compare.mjs so the diff is byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- building ------------------------------------------------------------ */

static mdy_node *new_node(mdy_doc *doc, mdy_node_type type) {
    mdy_node *n = mdy_alloc(&doc->arena, sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    n->type = type;
    return n;
}

mdy_node *mdy_new_element(mdy_doc *doc, const char *tag, size_t tag_len) {
    mdy_node *n = new_node(doc, MDY_ELEMENT);
    if (n) n->tag = mdy_intern(&doc->arena, &doc->names, tag, tag_len);
    return n;
}

mdy_node *mdy_new_text(mdy_doc *doc, const char *text, size_t len) {
    mdy_node *n = new_node(doc, MDY_TEXT);
    if (n) n->text = mdy_strdup_n(&doc->arena, text, len);
    return n;
}

void mdy_append(mdy_node *parent, mdy_node *child) {
    if (!parent || !child) return;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static mdy_prop *new_prop(mdy_doc *doc, mdy_node *el, const char *name) {
    /* `properties` is an OBJECT, so a repeated name replaces rather than
     * appends — emitting it twice produced JSON with a duplicate key, which is
     * not the same thing at all. */
    const char *interned = mdy_intern(&doc->arena, &doc->names, name, strlen(name));
    for (mdy_prop *q = el->props; q; q = q->next) {
        if (q->name == interned) { q->list = NULL; q->list_len = 0; return q; }
    }

    mdy_prop *p = mdy_alloc(&doc->arena, sizeof *p);
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->name = interned;
    if (el->props_tail) el->props_tail->next = p;
    else el->props = p;
    el->props_tail = p;
    return p;
}

void mdy_set_string(mdy_doc *doc, mdy_node *el, const char *name, const char *value, size_t value_len) {
    mdy_prop *p = new_prop(doc, el, name);
    if (!p) return;
    p->type = MDY_PROP_STRING;
    p->as.string = mdy_strdup_n(&doc->arena, value, value_len);
}

void mdy_set_number(mdy_doc *doc, mdy_node *el, const char *name, double value) {
    mdy_prop *p = new_prop(doc, el, name);
    if (!p) return;
    p->type = MDY_PROP_NUMBER;
    p->as.number = value;
}

void mdy_set_bool(mdy_doc *doc, mdy_node *el, const char *name, int value) {
    mdy_prop *p = new_prop(doc, el, name);
    if (!p) return;
    p->type = MDY_PROP_BOOL;
    p->as.boolean = value;
}

/*
 * `className` is the one list-valued property hast produces here, and it is
 * appended to rather than replaced — an element can pick up classes from more
 * than one rule.
 */
void mdy_add_class(mdy_doc *doc, mdy_node *el, const char *class_name) {
    mdy_prop *p = NULL;
    for (mdy_prop *q = el->props; q; q = q->next) {
        if (strcmp(q->name, "className") == 0) { p = q; break; }
    }
    if (!p) {
        p = new_prop(doc, el, "className");
        if (!p) return;
        p->type = MDY_PROP_LIST;
        p->list = NULL;
        p->list_len = 0;
    }
    const char **grown = mdy_alloc(&doc->arena, sizeof(char *) * (p->list_len + 1));
    if (!grown) return;
    for (size_t i = 0; i < p->list_len; i++) grown[i] = p->list[i];
    grown[p->list_len] = mdy_strdup_n(&doc->arena, class_name, strlen(class_name));
    p->list = grown;
    p->list_len++;
}

/**
 * Drop whatever classes an element has.
 *
 * Properties are an OBJECT, so a second `class=` on the same tag replaces the
 * first rather than adding to it — `<i ClassName="a" CLASS="b">` is `["b"]`.
 * The append in mdy_add_class is for the parser's own classes, where more than
 * one rule can contribute; a repeated attribute is not that case.
 */
void mdy_clear_class(mdy_doc *doc, mdy_node *el) {
    (void)doc;
    for (mdy_prop *q = el->props; q; q = q->next) {
        if (strcmp(q->name, "className") == 0) { q->list = NULL; q->list_len = 0; return; }
    }
}

/* ---- a growable output buffer -------------------------------------------- */

typedef struct { char *s; size_t len, cap; int positions; } Out;

static int out_put(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char *grown = realloc(o->s, cap);
        if (!grown) return -1;
        o->s = grown;
        o->cap = cap;
    }
    memcpy(o->s + o->len, s, n);
    o->len += n;
    o->s[o->len] = '\0';
    return 0;
}
static int out_str(Out *o, const char *s) { return out_put(o, s, strlen(s)); }

/*
 * A JSON string, escaped the way JSON.stringify escapes: the seven short
 * forms, \u00XX for the other control characters, and everything else —
 * including all of UTF-8 — passed through as its own bytes. Matching this
 * exactly is what lets the comparison be a byte diff.
 */
static int out_json_string(Out *o, const char *s) {
    if (out_put(o, "\"", 1) < 0) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  if (out_put(o, "\\\"", 2) < 0) return -1; break;
            case '\\': if (out_put(o, "\\\\", 2) < 0) return -1; break;
            case '\b': if (out_put(o, "\\b", 2) < 0) return -1; break;
            case '\f': if (out_put(o, "\\f", 2) < 0) return -1; break;
            case '\n': if (out_put(o, "\\n", 2) < 0) return -1; break;
            case '\r': if (out_put(o, "\\r", 2) < 0) return -1; break;
            case '\t': if (out_put(o, "\\t", 2) < 0) return -1; break;
            default:
                if (*p < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", *p);
                    if (out_str(o, buf) < 0) return -1;
                } else if (out_put(o, (const char *)p, 1) < 0) return -1;
        }
    }
    return out_put(o, "\"", 1);
}

/** A number the way JSON.stringify writes one: an integer with no `.0`. */
static int out_number(Out *o, double v) {
    char buf[40];
    if (v == (double)(long long)v) snprintf(buf, sizeof buf, "%lld", (long long)v);
    else snprintf(buf, sizeof buf, "%.17g", v);
    return out_str(o, buf);
}

static int emit(Out *o, const mdy_node *n) {
    switch (n->type) {
        case MDY_TEXT:
            if (out_str(o, "{\"type\":\"text\",\"value\":") < 0) return -1;
            if (out_json_string(o, n->text ? n->text : "") < 0) return -1;
            return out_put(o, "}", 1);

        case MDY_DOCTYPE:
            return out_str(o, "{\"type\":\"doctype\"}");

        /* Neither is produced by this parser — see mdy_node_type — but the
         * tree model can hold them, so the emitter has to be able to write
         * them or a tree that round-trips through JSON would lose them. */
        case MDY_COMMENT:
            if (out_str(o, "{\"type\":\"comment\",\"value\":") < 0) return -1;
            if (out_json_string(o, n->text ? n->text : "") < 0) return -1;
            return out_put(o, "}", 1);

        case MDY_RAW:
            if (out_str(o, "{\"type\":\"raw\",\"value\":") < 0) return -1;
            if (out_json_string(o, n->text ? n->text : "") < 0) return -1;
            return out_put(o, "}", 1);

        case MDY_ROOT:
            if (out_str(o, "{\"type\":\"root\",\"children\":[") < 0) return -1;
            break;

        case MDY_ELEMENT:
            if (out_str(o, "{\"type\":\"element\",\"tagName\":") < 0) return -1;
            if (out_json_string(o, n->tag) < 0) return -1;
            if (out_str(o, ",\"properties\":{") < 0) return -1;
            for (const mdy_prop *p = n->props; p; p = p->next) {
                if (p != n->props && out_put(o, ",", 1) < 0) return -1;
                if (out_json_string(o, p->name) < 0) return -1;
                if (out_put(o, ":", 1) < 0) return -1;
                switch (p->type) {
                    case MDY_PROP_STRING: if (out_json_string(o, p->as.string) < 0) return -1; break;
                    case MDY_PROP_NUMBER: if (out_number(o, p->as.number) < 0) return -1; break;
                    case MDY_PROP_BOOL:   if (out_str(o, p->as.boolean ? "true" : "false") < 0) return -1; break;
                    case MDY_PROP_LIST:
                        if (out_put(o, "[", 1) < 0) return -1;
                        for (size_t i = 0; i < p->list_len; i++) {
                            if (i && out_put(o, ",", 1) < 0) return -1;
                            if (out_json_string(o, p->list[i]) < 0) return -1;
                        }
                        if (out_put(o, "]", 1) < 0) return -1;
                        break;
                }
            }
            if (out_str(o, "},\"children\":[") < 0) return -1;
            break;
    }

    for (const mdy_node *c = n->first; c; c = c->next) {
        if (c != n->first && out_put(o, ",", 1) < 0) return -1;
        if (emit(o, c) < 0) return -1;
    }
    if (out_put(o, "]", 1) < 0) return -1;

    /*
     * The unist position, LAST — which is where JSON.stringify puts it, since
     * hast builds the node before attaching one. Only block elements have it;
     * a zero line means none.
     */
    if (o->positions && n->line) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 ",\"position\":{\"start\":{\"line\":%u,\"column\":%u},"
                 "\"end\":{\"line\":%u,\"column\":%u}}",
                 n->line, n->column, n->end_line, n->end_column);
        if (out_str(o, buf) < 0) return -1;
    }
    return out_put(o, "}", 1);
}

char *mdy_to_json(const mdy_node *node) {
    Out o = { .positions = 1 };
    if (!node || emit(&o, node) < 0) { free(o.s); return NULL; }
    return o.s ? o.s : calloc(1, 1);
}

/** The same, without positions — structure alone. */
char *mdy_to_json_bare(const mdy_node *node) {
    Out o = { 0 };
    if (!node || emit(&o, node) < 0) { free(o.s); return NULL; }
    return o.s ? o.s : calloc(1, 1);
}
