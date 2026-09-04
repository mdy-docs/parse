/*
 * The checks that do not need node — enough to know the library is not broken
 * before test/compare.mjs asks the harder question, which is whether it agrees
 * with the JavaScript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyast.h"

static int failures = 0;

static void check(const char *what, const char *source, const char *expected, const mdy_options *o) {
    mdy_doc *doc = mdy_parse(source, 0, o);
    char *json = mdy_to_json(mdy_root(doc));
    int ok = json && strcmp(json, expected) == 0;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      expected %s\n      actual   %s\n", expected, json ? json : "(null)");
        failures++;
    }
    free(json);
    mdy_free(doc);
}

#define EL(tag, props, kids) "{\"type\":\"element\",\"tagName\":\"" tag "\",\"properties\":{" props "},\"children\":[" kids "]}"
#define TX(v) "{\"type\":\"text\",\"value\":\"" v "\"}"
#define ROOT(kids) "{\"type\":\"root\",\"children\":[" kids "]}"

int main(void) {
    mdy_options o;
    mdy_options_default(&o);

    printf("--- mdyast: the tree ---\n");
    check("a paragraph", "hello world", ROOT(EL("p", "", TX("hello world"))), &o);
    check("blank lines separate blocks", "one\n\ntwo",
          ROOT(EL("p", "", TX("one")) "," EL("p", "", TX("two"))), &o);
    check("adjacent lines join with a space", "one\ntwo",
          ROOT(EL("p", "", TX("one two"))), &o);
    check("a heading, with its slug", "= Title",
          ROOT(EL("h1", "\"id\":\"title\"", TX("Title"))), &o);
    check("one = per level", "=== Deep",
          ROOT(EL("h3", "\"id\":\"deep\"", TX("Deep"))), &o);
    check("trailing = are decoration", "== Title ==",
          ROOT(EL("h2", "\"id\":\"title\"", TX("Title"))), &o);
    check("a thematic break", "***", ROOT(EL("hr", "", "")), &o);

    printf("--- mdyast: inline is toggling, not nesting ---\n");
    check("a single * is literal", "a *b* c", ROOT(EL("p", "", TX("a *b* c"))), &o);
    check("** toggles strong", "a **b** c",
          ROOT(EL("p", "", TX("a ") "," EL("strong", "", TX("b")) "," TX(" c"))), &o);
    check("// toggles em", "a //b// c",
          ROOT(EL("p", "", TX("a ") "," EL("em", "", TX("b")) "," TX(" c"))), &o);
    check("an unclosed marker is text", "a ** b",
          ROOT(EL("p", "", TX("a ** b"))), &o);
    check("a backslash escapes", "a \\*\\*b\\*\\* c",
          ROOT(EL("p", "", TX("a **b** c"))), &o);
    check("`` is raw — nothing inside is markup", "a ``**b**`` c",
          ROOT(EL("p", "", TX("a ") "," EL("code", "", TX("**b**")) "," TX(" c"))), &o);

    printf("--- mdyast: lists ---\n");
    check("a bullet list, newlines and all", "- a\n- b",
          ROOT(EL("ul", "", TX("\\n") "," EL("li", "", TX("a")) "," TX("\\n") ","
                              EL("li", "", TX("b")) "," TX("\\n"))), &o);
    check("an ordered list", "1. a",
          ROOT(EL("ol", "", TX("\\n") "," EL("li", "", TX("a")) "," TX("\\n"))), &o);

    printf("--- mdyast: autolink ---\n");
    check("a bare URL becomes a link", "go to https://example.com now",
          ROOT(EL("p", "", TX("go to ") ","
                  EL("a", "\"href\":\"https://example.com\"", TX("https://example.com")) ","
                  TX(" now"))), &o);
    check("a trailing full stop is not part of it", "see https://example.com.",
          ROOT(EL("p", "", TX("see ") ","
                  EL("a", "\"href\":\"https://example.com\"", TX("https://example.com")) ","
                  TX("."))), &o);

    printf("--- mdyast: front matter and documents ---\n");
    check("front matter is not content", "+++\ntitle: x\n+++\nbody",
          ROOT(EL("p", "", TX("body"))), &o);
    o.documents = 1;
    /* An element container separates its block children with newline text
     * nodes; the root does not. See `separate` in src/block.c. */
    check("--- starts a new article", "one\n---\ntwo",
          ROOT(EL("article", "", TX("\\n") "," EL("p", "", TX("one")) "," TX("\\n")) ","
               EL("article", "", TX("\\n") "," EL("p", "", TX("two")) "," TX("\\n"))), &o);
    o.documents = 0;

    printf("--- mdyast: fences ---\n");
    check("a fence keeps its content verbatim", "```js\nlet x = 1\n```",
          ROOT(EL("pre", "", EL("code", "\"className\":[\"language-js\"]", TX("let x = 1\\n")))), &o);

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
