/*
 * The checks that do not need node — enough to know the library is not broken
 * before test/compare.mjs asks the harder question, which is whether it agrees
 * with the JavaScript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyast.h"
#include "internal.h"

static int failures = 0;

static void check(const char *what, const char *source, const char *expected, const mdy_options *o) {
    mdy_doc *doc = mdy_parse(source, 0, o);
    /* Structure alone: these check what the tree IS, and threading a position
     * through every expectation would bury that. Positions have checks of
     * their own below. */
    char *json = mdy_to_json_bare(mdy_root(doc));
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
    /* Space after the `<` means nothing, which a real layout relies on:
     * `< html lang="en"` with the tag indented for reading. Unsanitised,
     * because <html> is not on the allowlist — with sanitising on, both this
     * and the JavaScript produce nothing at all. */
    {
        mdy_options raw = o;
        raw.sanitize = 0;
        check("space after < is nothing", "< html lang=\"en\"",
              ROOT(EL("html", "\"lang\":\"en\"", "")), &raw);
        check("…and <html> is not on the allowlist", "< html lang=\"en\"", ROOT(""), &o);
    }
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

    printf("--- mdyast: unicode ---\n");
    /* An astral character is ONE code point and TWO UTF-16 units. The corpus
     * has 1,351 of them, all cuneiform, so this is the case to get right. */
    {
        const char *sign = "\xf0\x92\x80\x80";          /* U+12000 CUNEIFORM SIGN A */
        uint32_t cp = 0;
        size_t width = mdy_utf8_decode(sign, 4, &cp);
        int ok = width == 4 && cp == 0x12000;
        printf("  %s  a cuneiform sign decodes to one code point\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        uint16_t units[8];
        size_t n = mdy_to_utf16(sign, 4, units, 8);
        ok = n == 2 && units[0] == 0xD808 && units[1] == 0xDC00;
        printf("  %s  …and to a surrogate PAIR in UTF-16\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_utf16_length(sign, 4) == 2;
        printf("  %s  …so its JavaScript length is 2, not 1\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        char back[8];
        size_t b = mdy_from_utf16(units, n, back, 8);
        ok = b == 4 && memcmp(back, sign, 4) == 0;
        printf("  %s  …and round trips back to the same bytes\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* An unpaired surrogate cannot be encoded; U+FFFD rather than a
         * refusal, since the character is already lost. */
        uint16_t lone[1] = { 0xD808 };
        b = mdy_from_utf16(lone, 1, back, 8);
        ok = b == 3 && (unsigned char)back[0] == 0xEF;
        printf("  %s  an unpaired surrogate becomes U+FFFD\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* Ill-formed input must not shift everything after it. */
        uint32_t bad = 0;
        ok = mdy_utf8_decode("\xC3", 1, &bad) == 1 && bad == 0xFFFD;
        printf("  %s  a truncated sequence is one byte and U+FFFD\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* An overlong form is ill-formed: C0 80 is not U+0000. */
        ok = mdy_utf8_decode("\xC0\x80", 2, &bad) == 1 && bad == 0xFFFD;
        printf("  %s  an overlong form is rejected\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_lower_cp(0x0143) == 0x0144 && mdy_lower_cp(0x1E2A) == 0x1E2B &&
             mdy_lower_cp(0x00C9) == 0x00E9;
        printf("  %s  case mapping is the real table, not a guess (Ń Ḫ É)\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_is_letter_or_number_cp(0x12000) && !mdy_is_letter_or_number_cp(0x2013) &&
             mdy_is_letter_or_number_cp(0x0661);
        printf("  %s  \\p{L} and \\p{N} are the real tables (cuneiform yes, en dash no)\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* Invalid bytes in the middle of a document must not derail the rest
         * of it: one byte consumed, one replacement character, everything
         * after it still at the right offset. */
        ok = mdy_utf8_decode("\xED\xA0\x80", 3, &bad) == 1 && bad == 0xFFFD;
        printf("  %s  a surrogate encoded as UTF-8 is rejected\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_utf8_decode("\xF5\x80\x80\x80", 4, &bad) == 1 && bad == 0xFFFD;
        printf("  %s  a code point above U+10FFFF is rejected\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;
    }

    /* Astral characters must survive the parse and the JSON, unmangled. */
    check("cuneiform survives a paragraph", "sign \xf0\x92\x80\x80 here",
          ROOT(EL("p", "", TX("sign \xf0\x92\x80\x80 here"))), &o);
    /* An en dash is not a letter, so defaultResolve deletes it — and deletes
     * all three of its bytes. */
    check("an en dash is deleted from a slug, whole", "[[ First Jewish\xe2\x80\x93Roman War ]]",
          ROOT(EL("p", "", EL("a", "\"href\":\"first-jewishroman-war\"",
                              TX("First Jewish\xe2\x80\x93Roman War")))), &o);
    check("a slug lowercases beyond ASCII", "[[ Kazimierz Micha\xc5\x81owski ]]",
          ROOT(EL("p", "", EL("a", "\"href\":\"kazimierz-micha\xc5\x82owski\"",
                              TX("Kazimierz Micha\xc5\x81owski")))), &o);

    printf("--- mdyast: links (the linkify-it port) ---\n");
    /* The conditional path rules, which are the whole reason for the port: a
     * full stop ending a sentence is not part of the URL, and a comma with
     * something after it is. */
    check("a trailing full stop is not in the URL", "see http://x.com.",
          ROOT(EL("p", "", TX("see ") ","
                  EL("a", "\"href\":\"http://x.com\"", TX("http://x.com")) "," TX("."))), &o);
    check("a comma inside a path is", "at http://x.com/a,b end",
          ROOT(EL("p", "", TX("at ") ","
                  EL("a", "\"href\":\"http://x.com/a,b\"", TX("http://x.com/a,b")) ","
                  TX(" end"))), &o);
    /* A protocol-relative URL needs a dotted host; `//word` is emphasis. */
    check("//host is a link", "x //example.com/p y",
          ROOT(EL("p", "", TX("x ") ","
                  EL("a", "\"href\":\"//example.com/p\"", TX("//example.com/p")) "," TX(" y"))), &o);
    check("…and a hyphen in the last label is not", "x //a.b-c// y",
          ROOT(EL("p", "", TX("x ") "," EL("em", "", TX("a.b-c")) "," TX(" y"))), &o);
    /* An underscore before a scheme disqualifies it — and then the `//` is
     * just a marker, which opens an emphasis that runs to the end. Checked
     * against the JavaScript rather than assumed: the obvious expectation is
     * that the whole thing stays literal text, and it does not. */
    check("_ before a scheme is not a link", "a _http://x.com b",
          ROOT(EL("p", "", TX("a _http:") "," EL("em", "", TX("x.com b")))), &o);

    printf("--- mdyast: doctype ---\n");
    /* A fourth node type, and one a scan of the corpus's DOCUMENTS misses —
     * a doctype only appears in a site's LAYOUTS. */
    check("<!doctype html> is its own node", "<!doctype html>",
          ROOT("{\"type\":\"doctype\"}"), &o);
    check("…case-insensitively", "<!DOCTYPE html>",
          ROOT("{\"type\":\"doctype\"}"), &o);
    /* A comment is NOT special — the ordinary element rule handles it, and
     * the JavaScript agrees. Its `a` and `comment` read as boolean attributes,
     * which the schema then drops; with sanitize off they survive. */
    check("a comment is an ordinary div", "<!-- a comment -->",
          ROOT(EL("div", "", "")), &o);
    {
        mdy_options raw = o;
        raw.sanitize = 0;
        check("…keeping its stray attributes when unsanitised", "<!-- a comment -->",
              ROOT(EL("div", "\"a\":true,\"comment\":true", "")), &raw);
    }

    printf("--- mdyast: hast property names ---\n");
    /*
     * `property-information`'s `find(html, name)`, which is what mdy-docs
     * calls. Each of these was wrong when the table was hand-written, and each
     * changed real output: one attribute resolving differently was enough to
     * reorder an entire element's properties downstream.
     */
    {
        mdy_options raw = o;
        raw.sanitize = 0;
        check("a known name is matched case-insensitively", "<img SRC=\"a\">",
              ROOT(EL("img", "\"src\":\"a\"", "")), &raw);
        check("…including the ones hast respells", "<i For=\"a\">",
              ROOT(EL("i", "\"htmlFor\":\"a\"", "")), &raw);
        check("an unknown name keeps the author's case", "<img FOO=\"1\">",
              ROOT(EL("img", "\"FOO\":\"1\"", "")), &raw);
        check("data- camel-cases only before a lowercase letter", "<img DATA-x-Y=\"e\">",
              ROOT(EL("img", "\"dataX-Y\":\"e\"", "")), &raw);
        check("…and does capitalise the first segment", "<img data-foo-bar=\"d\">",
              ROOT(EL("img", "\"dataFooBar\":\"d\"", "")), &raw);
        /* Properties are an object, so a repeated attribute REPLACES. The
         * append in mdy_add_class is for the parser's own classes. */
        check("a repeated class replaces rather than accumulating",
              "<i ClassName=\"a\" CLASS=\"b\">",
              ROOT(EL("i", "\"className\":[\"b\"]", "")), &raw);
    }

    printf("--- mdyast: ordered list start ---\n");
    /* A marker may be followed by the END OF THE LINE, and an ordered list
     * that does not begin at 1 records where it does. */
    check("1931. alone is a marker, and sets start", "1931.\nx",
          ROOT(EL("ol", "\"start\":1931", TX("\\n") ","
                  EL("li", "", TX(" x")) "," TX("\\n"))), &o);
    check("…and 1 is the default, so it is left off", "1. a",
          ROOT(EL("ol", "", TX("\\n") "," EL("li", "", TX("a")) "," TX("\\n"))), &o);
    check("a continuation needs no indentation", "- one\ntwo",
          ROOT(EL("ul", "", TX("\\n") "," EL("li", "", TX("one two")) "," TX("\\n"))), &o);

    printf("--- mdyast: loose and tight lists ---\n");
    check("a blank line between items makes the list loose", "- a\n\n- b",
          ROOT(EL("ul", "", TX("\\n") ","
                  EL("li", "", TX("\\n") "," EL("p", "", TX("a")) "," TX("\\n")) "," TX("\\n") ","
                  EL("li", "", TX("\\n") "," EL("p", "", TX("b")) "," TX("\\n")) "," TX("\\n"))), &o);

    printf("--- mdyast: indentation nests ---\n");
    /* Every TWO columns is one level, so four columns is two divs. */
    check("four columns is two nested divs", "top\n    in",
          ROOT(EL("p", "", TX("top")) ","
               EL("div", "", TX("\\n") ","
                  EL("div", "", TX("\\n") "," EL("p", "", TX("in")) "," TX("\\n")) ","
                  TX("\\n"))), &o);

    printf("--- mdyast: unist positions ---\n");
    {
        /* Block elements only, and every number here is what mdy-docs emits
         * for the same source. */
        mdy_doc *doc = mdy_parse("one two\n\n= Head\n\n- a\n- b", 0, &o);
        char *json = mdy_to_json(mdy_root(doc));
        struct { const char *what; const char *needle; } want[] = {
            { "a paragraph spans its line",
              "\"position\":{\"start\":{\"line\":1,\"column\":1},\"end\":{\"line\":1,\"column\":8}}" },
            { "a heading knows which line it was on",
              "\"position\":{\"start\":{\"line\":3,\"column\":1},\"end\":{\"line\":3,\"column\":7}}" },
            { "a list spans all of its items",
              "\"position\":{\"start\":{\"line\":5,\"column\":1},\"end\":{\"line\":6,\"column\":4}}" },
        };
        for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
            int ok = strstr(json, want[k].needle) != NULL;
            printf("  %s  %s\n", ok ? "ok  " : "FAIL", want[k].what);
            if (!ok) { failures++; printf("      wanted %s\n      in     %s\n", want[k].needle, json); }
        }
        free(json);
        mdy_free(doc);

        /* A column is UTF-16 units, so an astral character counts twice —
         * `a 𒀀 b` is six units, not five characters and not seven bytes. */
        doc = mdy_parse("a \xf0\x92\x80\x80 b", 0, &o);
        json = mdy_to_json(mdy_root(doc));
        int ok = strstr(json, "\"end\":{\"line\":1,\"column\":7}") != NULL;
        printf("  %s  a column counts UTF-16 units, not characters\n", ok ? "ok  " : "FAIL");
        if (!ok) { failures++; printf("      %s\n", json); }
        free(json);
        mdy_free(doc);

        /* An indented block still starts at column 1: the column is measured
         * from the start of the line, indentation included. */
        doc = mdy_parse("top\n  in", 0, &o);
        json = mdy_to_json(mdy_root(doc));
        ok = strstr(json, "\"start\":{\"line\":2,\"column\":1},\"end\":{\"line\":2,\"column\":5}") != NULL;
        printf("  %s  an indented block starts at column 1, ends past its indent\n", ok ? "ok  " : "FAIL");
        if (!ok) { failures++; printf("      %s\n", json); }
        free(json);
        mdy_free(doc);

        /* Inline elements carry none, and neither does the root. */
        doc = mdy_parse("a **b** c", 0, &o);
        json = mdy_to_json(mdy_root(doc));
        const char *strong = strstr(json, "\"tagName\":\"strong\"");
        const char *after = strong ? strstr(strong, "\"position\"") : NULL;
        const char *next_p = strong ? strstr(strong, "}]}") : NULL;
        ok = strong && (!after || (next_p && after > next_p));
        printf("  %s  an inline element carries no position\n", ok ? "ok  " : "FAIL");
        if (!ok) { failures++; printf("      %s\n", json); }
        free(json);
        mdy_free(doc);

        /* lineOffset points positions at the real file rather than at whatever
         * slice of it was handed over. */
        mdy_options shifted = o;
        shifted.line_offset = 10;
        doc = mdy_parse("hi", 0, &shifted);
        json = mdy_to_json(mdy_root(doc));
        ok = strstr(json, "\"start\":{\"line\":11,\"column\":1}") != NULL;
        printf("  %s  line_offset moves them to the original file\n", ok ? "ok  " : "FAIL");
        if (!ok) { failures++; printf("      %s\n", json); }
        free(json);
        mdy_free(doc);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
