/*
 * The HTML writer, on trees built by hand.
 *
 * Not one of these cases parses anything. That is the point: src/html.c takes
 * a tree and returns a string, and nothing about it needs the parser to exist
 * — so the trees here are plain C structs, assembled from the public types in
 * include/mdyast.h with no arena, no document and no source text anywhere.
 *
 * Every expectation was taken FROM `hast-util-to-html` rather than reasoned
 * out, by building the same tree in JavaScript and printing what it produced.
 * Two of them would have been wrong otherwise: `id: true` writes a bare `id`
 * even though `id` is not a boolean property, and a void element that has
 * children gets a closing tag after all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyast.h"
#include "mdyhtml.h"

static int failures = 0;

/* ---- building a tree without a parser ------------------------------------- */

#define POOL 128
static mdy_node nodes[POOL];
static mdy_prop props[POOL];
static size_t node_used, prop_used;

static void reset(void) { node_used = prop_used = 0; memset(nodes, 0, sizeof nodes); memset(props, 0, sizeof props); }

static mdy_node *node(mdy_node_type type) {
    mdy_node *n = &nodes[node_used++];
    n->type = type;
    return n;
}

static mdy_node *text(const char *value) {
    mdy_node *n = node(MDY_TEXT);
    n->text = value;
    return n;
}

static mdy_node *element(const char *tag) {
    mdy_node *n = node(MDY_ELEMENT);
    n->tag = tag;
    return n;
}

static mdy_node *child(mdy_node *parent, mdy_node *c) {
    if (parent->last) parent->last->next = c;
    else parent->first = c;
    parent->last = c;
    return parent;
}

static mdy_prop *prop(mdy_node *el, const char *name) {
    mdy_prop *p = &props[prop_used++];
    p->name = name;
    if (el->props_tail) el->props_tail->next = p;
    else el->props = p;
    el->props_tail = p;
    return p;
}

static void attr(mdy_node *el, const char *name, const char *value) {
    mdy_prop *p = prop(el, name);
    p->type = MDY_PROP_STRING;
    p->as.string = value;
}

static void attr_bool(mdy_node *el, const char *name, int value) {
    mdy_prop *p = prop(el, name);
    p->type = MDY_PROP_BOOL;
    p->as.boolean = value;
}

static void attr_number(mdy_node *el, const char *name, double value) {
    mdy_prop *p = prop(el, name);
    p->type = MDY_PROP_NUMBER;
    p->as.number = value;
}

static void attr_list(mdy_node *el, const char *name, const char **items, size_t n) {
    mdy_prop *p = prop(el, name);
    p->type = MDY_PROP_LIST;
    p->list = items;
    p->list_len = n;
}

/* ---- checking -------------------------------------------------------------- */

static void check(const char *what, const mdy_node *tree, const char *expected) {
    char *html = mdy_to_html(tree, NULL);
    int ok = html && strcmp(html, expected) == 0;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      expected %s\n      actual   %s\n", expected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

int main(void) {
    printf("--- mdyhtml: text ---\n");
    reset();
    check("only `<` and `&` are escaped in text", text("a < b & c > d"),
          "a &#x3C; b &#x26; c > d");

    reset();
    {
        mdy_node *s = element("script");
        child(s, text("a < b && c"));
        check("a script holds a program, not markup", s, "<script>a < b && c</script>");
    }
    reset();
    {
        mdy_node *s = element("style");
        child(s, text("a > b & c"));
        check("…and so does a style", s, "<style>a > b & c</style>");
    }

    printf("--- mdyhtml: attributes ---\n");
    reset();
    {
        mdy_node *a = element("a");
        attr(a, "href", "x\"y'z`w<v>u&t");
        child(a, text("l"));
        /* `<` and `>` are safe inside a quoted value and are NOT escaped. */
        check("a value escapes the quotes and the backtick", a,
              "<a href=\"x&#x22;y&#x27;z&#x60;w<v>u&#x26;t\">l</a>");
    }
    reset();
    {
        mdy_node *p = element("p");
        attr(p, "a\"b", "1");
        child(p, text("x"));
        check("…and a name has its own subset", p, "<p a&#x22;b=\"1\">x</p>");
    }
    reset();
    {
        mdy_node *p = element("p");
        static const char *cls[] = { "a", "b" };
        attr_list(p, "className", cls, 2);
        child(p, text("x"));
        check("className is a space separated list", p, "<p class=\"a b\">x</p>");
    }
    reset();
    {
        mdy_node *i = element("input");
        static const char *types[] = { ".png", ".jpg" };
        attr_list(i, "accept", types, 2);
        check("…and accept is a comma separated one", i, "<input accept=\".png, .jpg\">");
    }
    reset();
    {
        mdy_node *o = element("ol");
        attr_number(o, "start", 1931);
        child(o, text("x"));
        check("a number is written as one", o, "<ol start=\"1931\">x</ol>");
    }
    reset();
    {
        mdy_node *p = element("p");
        attr(p, "id", "");
        child(p, text("x"));
        check("an empty value still gets its quotes", p, "<p id=\"\">x</p>");
    }

    printf("--- mdyhtml: booleans ---\n");
    reset();
    {
        mdy_node *i = element("input");
        attr(i, "type", "checkbox");
        attr_bool(i, "disabled", 1);
        check("a boolean that is true is written bare", i,
              "<input type=\"checkbox\" disabled>");
    }
    reset();
    {
        mdy_node *i = element("input");
        attr(i, "type", "checkbox");
        attr_bool(i, "checked", 0);
        attr_bool(i, "disabled", 1);
        check("…and one that is false is not written at all", i,
              "<input type=\"checkbox\" disabled>");
    }
    reset();
    {
        mdy_node *a = element("a");
        attr(a, "href", "/f");
        attr(a, "download", "");
        child(a, text("d"));
        /* `download` is an OVERLOADED boolean: an empty value, or one equal to
         * its own name, means the bare form. */
        check("an overloaded boolean collapses to its name", a,
              "<a href=\"/f\" download>d</a>");
    }
    reset();
    {
        mdy_node *p = element("p");
        attr_bool(p, "id", 1);
        child(p, text("x"));
        /* `if (value === true) return name` runs for EVERY property, not only
         * the boolean ones — so `id` set to true is a bare `id`. */
        check("true on a non-boolean property is bare too", p, "<p id>x</p>");
    }

    printf("--- mdyhtml: property names ---\n");
    reset();
    {
        mdy_node *p = element("p");
        attr(p, "dataFooBar", "1");
        attr(p, "dataX", "2");
        child(p, text("x"));
        check("a data property is kebab-cased back", p,
              "<p data-foo-bar=\"1\" data-x=\"2\">x</p>");
    }
    reset();
    {
        mdy_node *p = element("p");
        attr(p, "FOO", "1");
        attr(p, "Foo-Bar", "2");
        child(p, text("x"));
        check("an unknown property is written as it is named", p,
              "<p FOO=\"1\" Foo-Bar=\"2\">x</p>");
    }

    printf("--- mdyhtml: node types ---\n");
    reset();
    check("a doctype", node(MDY_DOCTYPE), "<!doctype html>");
    reset();
    {
        mdy_node *c = node(MDY_COMMENT);
        c->text = " a note ";
        check("a comment", c, "<!-- a note -->");
    }
    reset();
    {
        mdy_node *r = node(MDY_RAW);
        r->text = "<b>raw</b>";
        check("a raw node passes through", r, "<b>raw</b>");
        mdy_html_options safe = { .allow_dangerous_html = 0 };
        char *html = mdy_to_html(r, &safe);
        int ok = html && strcmp(html, "&#x3C;b>raw&#x3C;/b>") == 0;
        printf("  %s  %s\n", ok ? "ok  " : "FAIL", "…and is escaped when that is refused");
        if (!ok) { printf("      actual   %s\n", html ? html : "(null)"); failures++; }
        free(html);
    }

    printf("--- mdyhtml: elements ---\n");
    reset();
    check("a void element has no closing tag", element("hr"), "<hr>");
    reset();
    {
        mdy_node *hr = element("hr");
        child(hr, text("odd"));
        /* A void element that turns out to have children is not void after
         * all — which is how the lists disagreeing about `<menuitem>` still
         * serialises. */
        check("…unless it has children", hr, "<hr>odd</hr>");
    }
    reset();
    {
        mdy_node *d = element("div");
        static const char *cls[] = { "w" };
        attr_list(d, "className", cls, 1);
        mdy_node *a = element("p"); child(a, text("a"));
        mdy_node *b = element("p"); child(b, text("b"));
        child(child(d, a), b);
        check("nested elements", d, "<div class=\"w\"><p>a</p><p>b</p></div>");
    }
    reset();
    {
        mdy_node *root = node(MDY_ROOT);
        mdy_node *h = element("h1"); child(h, text("A"));
        mdy_node *p = element("p"); child(p, text("B"));
        child(child(child(root, h), text(" ")), p);
        check("a root writes its children and nothing of itself", root,
              "<h1>A</h1> <p>B</p>");
    }
    reset();
    check("an empty root is an empty string", node(MDY_ROOT), "");

    if (failures) {
        printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
