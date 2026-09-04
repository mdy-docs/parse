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
    /* An unclosed marker still opens, and runs to the end of the input. This
     * expectation was written the other way round first, on intuition rather
     * than by asking the JavaScript — and it passed, because the C had the
     * same wrong idea. */
    check("an unclosed marker still opens", "a ** b",
          ROOT(EL("p", "", TX("a ") "," EL("strong", "", TX(" b")))), &o);
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

    printf("--- mdyast: tables ---\n");
    check("a pipe table", "a | b\n--- | ---\n1 | 2",
          ROOT(EL("table", "",
                  TX("\\n") ","
                  EL("thead", "", TX("\\n") ","
                     EL("tr", "", TX("\\n") "," EL("th", "", TX("a")) ","
                        TX("\\n") "," EL("th", "", TX("b")) "," TX("\\n")) ","
                     TX("\\n")) ","
                  TX("\\n") ","
                  EL("tbody", "", TX("\\n") ","
                     EL("tr", "", TX("\\n") "," EL("td", "", TX("1")) ","
                        TX("\\n") "," EL("td", "", TX("2")) "," TX("\\n")) ","
                     TX("\\n")) ","
                  TX("\\n"))), &o);
    /* A table needs a pipe: `a\\n:-:\\n1` is a paragraph, and `:-:` in it is an
     * emoticon. Written the other way round first, on the assumption that one
     * column is a degenerate table — it is not. */
    check("alignment is a style attribute", "a | b\n:-: | --:\n1 | 2",
          ROOT(EL("table", "",
                  TX("\\n") ","
                  EL("thead", "", TX("\\n") ","
                     EL("tr", "", TX("\\n") ","
                        EL("th", "\"style\":\"text-align: center\"", TX("a")) "," TX("\\n") ","
                        EL("th", "\"style\":\"text-align: right\"", TX("b")) "," TX("\\n")) ","
                     TX("\\n")) ","
                  TX("\\n") ","
                  EL("tbody", "", TX("\\n") ","
                     EL("tr", "", TX("\\n") ","
                        EL("td", "\"style\":\"text-align: center\"", TX("1")) "," TX("\\n") ","
                        EL("td", "\"style\":\"text-align: right\"", TX("2")) "," TX("\\n")) ","
                     TX("\\n")) ","
                  TX("\\n"))), &o);

    printf("--- mdyast: typography and references ---\n");
    check("-- is an em dash", "a -- b", ROOT(EL("p", "", TX("a — b"))), &o);
    check("--- is not", "a---b", ROOT(EL("p", "", TX("a---b"))), &o);
    check("... is an ellipsis", "x...y", ROOT(EL("p", "", TX("x…y"))), &o);
    check("--> is an arrow", "a --> b", ROOT(EL("p", "", TX("a → b"))), &o);
    /* A tag keeps its case, unlike almost everything else that becomes a URL. */
    check("#tag keeps its case", "see #Tag-One",
          ROOT(EL("p", "", TX("see ") ","
                  EL("a", "\"href\":\"/tags/Tag-One\"", TX("#Tag-One")))), &o);
    check("@mention", "ask @dan",
          ROOT(EL("p", "", TX("ask ") ","
                  EL("a", "\"href\":\"/users/dan\"", TX("@dan")))), &o);

    printf("--- mdyast: footnotes ---\n");
    check("a reference with no definition stays text", "body [[ ^7 ]] end",
          ROOT(EL("p", "", TX("body [[ ^7 ]] end"))), &o);
    check("an unreferenced definition produces nothing", "body\n\n[[ ^9 ]]: never used",
          ROOT(EL("p", "", TX("body"))), &o);

    printf("--- mdyast: wiki links ---\n");
    /* defaultResolve DELETES what it cannot keep; slugify would hyphenate it.
     * `Umm el-Qa'ab` is umm-el-qaab, not umm-el-qa-ab. */
    check("a bare label resolves to its own slug", "[[ Umm el-Qa\'ab ]]",
          ROOT(EL("p", "", EL("a", "\"href\":\"umm-el-qaab\"", TX("Umm el-Qa\'ab")))), &o);
    check("…keeping a full stop", "[[ Edward R. Ayrton ]]",
          ROOT(EL("p", "", EL("a", "\"href\":\"edward-r.-ayrton\"", TX("Edward R. Ayrton")))), &o);
    check("an explicit target is used verbatim", "[[ label | /url ]]",
          ROOT(EL("p", "", EL("a", "\"href\":\"/url\"", TX("label")))), &o);

    printf("--- mdyast: the element syntax ---\n");
    check("a bare < is a div", "<\n  inside",
          ROOT(EL("div", "", TX("\\n") "," EL("p", "", TX("inside")) "," TX("\\n"))), &o);
    check("attributes become hast properties", "<img src=\"a.jpg\" height=\"10\"",
          ROOT(EL("img", "\"src\":\"a.jpg\",\"height\":\"10\"", "")), &o);
    /* scope is allowed on th and not on td — sanitisation is not optional. */
    check("a disallowed attribute is dropped", "<td scope=\"col\" colspan=\"2\"",
          ROOT(EL("td", "\"colSpan\":\"2\"", "")), &o);
    check("a closing > makes the rest inline content", "<figcaption>caption text",
          ROOT(EL("figcaption", "", TX("caption text"))), &o);

    printf("--- mdyast: block structure ---\n");
    /* Indentation is structural: a line further in than its run is a block of
     * its own. It applies at the start of a document too. */
    check("an indented line gets a div", "top\n  in",
          ROOT(EL("p", "", TX("top")) ","
               EL("div", "", TX("\\n") "," EL("p", "", TX("in")) "," TX("\\n"))), &o);
    check("…including the first line", "  only indented",
          ROOT(EL("div", "", TX("\\n") "," EL("p", "", TX("only indented")) "," TX("\\n"))), &o);
    check("a list item absorbs its continuation", "- one\n  two",
          ROOT(EL("ul", "", TX("\\n") "," EL("li", "", TX("one two")) "," TX("\\n"))), &o);
    check("a deeper marker nests a list", "- a\n  - b",
          ROOT(EL("ul", "", TX("\\n") ","
                  EL("li", "", TX("a") "," TX("\\n") ","
                     EL("ul", "", TX("\\n") "," EL("li", "", TX("b")) "," TX("\\n")) ","
                     TX("\\n")) ","
                  TX("\\n"))), &o);

    printf("--- mdyast: setext underlining ---\n");
    check("= underlines to h1", "Title\n=====",
          ROOT(EL("h1", "\"id\":\"title\"", TX("Title"))), &o);
    /* FOUR or more hyphens. Three is a thematic break, and the paragraph above
     * it stands on its own. */
    check("four - underline to h2", "Title\n----",
          ROOT(EL("h2", "\"id\":\"title\"", TX("Title"))), &o);
    check("three - is a break, not an underline", "Title\n---",
          ROOT(EL("p", "", TX("Title")) "," EL("hr", "", "")), &o);

    printf("--- mdyast: task lists ---\n");
    check("a task list", "- [ ] todo\n- [x] done",
          ROOT(EL("ul", "\"className\":[\"contains-task-list\"]", TX("\\n") ","
                  EL("li", "\"className\":[\"task-list-item\"]",
                     EL("input", "\"type\":\"checkbox\",\"checked\":false,\"disabled\":true", "") ","
                     TX(" ") "," TX("todo")) "," TX("\\n") ","
                  EL("li", "\"className\":[\"task-list-item\"]",
                     EL("input", "\"type\":\"checkbox\",\"checked\":true,\"disabled\":true", "") ","
                     TX(" ") "," TX("done")) "," TX("\\n"))), &o);

    printf("--- mdyast: heading ids are unique ---\n");
    check("a repeated heading gets a suffix", "= Same\n\n= Same",
          ROOT(EL("h1", "\"id\":\"same\"", TX("Same")) ","
               EL("h1", "\"id\":\"same-1\"", TX("Same"))), &o);

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
