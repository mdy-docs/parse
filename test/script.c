/*
 * The MDY script layer, compiled — on documents, with no engine anywhere.
 *
 * Every expectation below was TAKEN FROM mdy-docs' own `compileScript` rather
 * than reasoned out: the cases were run through it and its output written in
 * here verbatim. That matters more for this stage than for any other, because
 * the source this produces is compiled and RUN. A difference is not a
 * different tree; it is different behaviour.
 *
 * The corpus harness (make check-script) covers the documents that exist.
 * These cover the ones that do not: a brace inside a string, inside a comment,
 * inside a `${}`; a `%%` that never closes; an escaped sigil; CRLF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyscript.h"

static int failures = 0;

/*
 * `code` is one character per line of the document — `1` where the line went
 * in as code, `0` where it became content. A host maps a runtime error back
 * through it to the line somebody wrote.
 */
static void check(const char *what, const char *source, const char *expected,
                  const char *code) {
    mdy_script *s = mdy_script_compile(source, strlen(source));
    if (!s) { printf("  FAIL  %s (compile returned NULL)\n", what); failures++; return; }

    size_t len = 0;
    const char *got = mdy_script_source(s, &len);
    int ok = strlen(expected) == len && memcmp(got, expected, len) == 0;

    char lines[128];
    size_t n = mdy_script_line_count(s);
    if (n >= sizeof lines) n = sizeof lines - 1;
    for (size_t i = 0; i < n; i++) lines[i] = mdy_script_is_code(s, i) ? '1' : '0';
    lines[n] = '\0';
    int code_ok = strcmp(lines, code) == 0;

    printf("  %s  %s\n", ok && code_ok ? "ok  " : "FAIL", what);
    if (!ok) printf("      expected %s\n      actual   %s\n", expected, got);
    if (!code_ok) printf("      code map expected %s, got %s\n", code, lines);
    if (!ok || !code_ok) failures++;
    mdy_script_free(s);
}

int main(void) {
    printf("--- mdyscript: documents to statements ---\n");
    check("plain content", "hello",
          "const __out = []\n__out.push([0, `hello`])", "0");
    check("a % line", "% const x = 1\ntext",
          "const __out = []\n const x = 1\n__out.push([1, `text`])", "10");
    check("leading space carries no meaning", "   % const x = 1\ntext",
          "const __out = []\n const x = 1\n__out.push([1, `text`])", "10");
    check("a loop encloses the content under it", "% for (const n of [1,2]) {\n- {{ n }}\n% }",
          "const __out = []\n for (const n of [1,2]) {\n__out.push([1, `- ${ n }`])\n }", "101");
    check("an escaped sigil is content", "\\% not code",
          "const __out = []\n__out.push([0, `% not code`])", "0");
    check("…however far in it is written", "  \\% not code",
          "const __out = []\n__out.push([0, `  % not code`])", "0");
    check("an escaped opener is a literal `{{`", "a \\{{ b }} c",
          "const __out = []\n__out.push([0, `a {{ b }} c`])", "0");
    check("an unclosed opener stays text", "a {{ b c",
          "const __out = []\n__out.push([0, `a {{ b c`])", "0");
    check("a template literal's three escapes", "back`tick and $dollar and back\\slash",
          "const __out = []\n__out.push([0, `back\\`tick and \\$dollar and back\\\\slash`])", "0");
    check("an interpolation may hold braces", "{{ o.map(x => ({y: x})).length }}",
          "const __out = []\n__out.push([0, `${ o.map(x => ({y: x})).length }`])", "0");
    check("…and a template of its own", "{{ `a ${ b } c` }}",
          "const __out = []\n__out.push([0, `${ `a ${ b } c` }`])", "0");
    check("%% runs on to the line that closes it", "%% transform((tree) => {\n  visit(tree)\n})\ntail",
          "const __out = []\n transform((tree) => {\n  visit(tree)\n})\n__out.push([3, `tail`])", "1110");
    check("…and takes nothing when it never closes", "%% transform((tree) => {\n  visit(tree)\ntail",
          "const __out = []\n transform((tree) => {\n__out.push([1, `  visit(tree)`])\n__out.push([2, `tail`])", "100");
    check("…another code line ends the search", "%% f({\n% const x = 1\ntail",
          "const __out = []\n f({\n const x = 1\n__out.push([2, `tail`])", "110");
    check("a brace inside a string is not a brace", "%% f(\"{\", {\n  a: 1\n})\ntail",
          "const __out = []\n f(\"{\", {\n  a: 1\n})\n__out.push([3, `tail`])", "1110");
    check("…nor one inside a line comment", "%% f( // {\n  1)\ntail",
          "const __out = []\n f( // {\n  1)\n__out.push([2, `tail`])", "110");
    check("…nor one inside a block comment", "%% f(/* { */ 1,\n  2)\ntail",
          "const __out = []\n f(/* { */ 1,\n  2)\n__out.push([2, `tail`])", "110");
    check("…nor one inside a ${} in a template", "%% f(`${ {a:1}.a }`, {\n  b: 2\n})\ntail",
          "const __out = []\n f(`${ {a:1}.a }`, {\n  b: 2\n})\n__out.push([3, `tail`])", "1110");
    check("an escaped quote does not end its string", "%% f('it\\'s {', {\n  a: 1\n})\ntail",
          "const __out = []\n f('it\\'s {', {\n  a: 1\n})\n__out.push([3, `tail`])", "1110");
    check("a regex is NOT understood, deliberately", "%% f(/[{]/, {\n  a: 1\n})\ntail",
          "const __out = []\n f(/[{]/, {\n__out.push([1, `  a: 1`])\n__out.push([2, `})`])\n__out.push([3, `tail`])", "1000");
    check("a % line's brackets are not counted", "% if (x) {\ncontent\n% }",
          "const __out = []\n if (x) {\n__out.push([1, `content`])\n }", "101");
    check("CRLF is a line ending too", "a\r\n% const x = 1\r\nb",
          "const __out = []\n__out.push([0, `a`])\n const x = 1\n__out.push([2, `b`])", "010");
    check("a trailing newline is a last empty line", "a\n",
          "const __out = []\n__out.push([0, `a`])\n__out.push([1, ``])", "00");
    check("an empty document", "",
          "const __out = []\n__out.push([0, ``])", "0");
    check("a document that is only code", "% const x = 1",
          "const __out = []\n const x = 1", "1");
    check("two interpolations on one line", "{{ a }} and {{ b }}",
          "const __out = []\n__out.push([0, `${ a } and ${ b }`])", "0");
    check("an interpolation holding a closer", "{{ \"}}\" }}",
          "const __out = []\n__out.push([0, `${ \"}\" }}`])", "0");
    check("utf-8 passes through", "— café 𒀁 {{ x }}",
          "const __out = []\n__out.push([0, `— café 𒀁 ${ x }`])", "0");

    printf("--- mdyscript: is a line code? ---\n");
    {
        struct { const char *line; int want; } cases[] = {
            { "% x", 1 }, { "  % x", 1 }, { "\t% x", 1 }, { "%% x", 1 },
            { "%", 1 }, { "x % y", 0 }, { "", 0 }, { " x", 0 }, { "\\% x", 0 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            int got = mdy_script_is_line(cases[i].line, strlen(cases[i].line));
            int ok = (got != 0) == (cases[i].want != 0);
            printf("  %s  %-14s is %s code\n", ok ? "ok  " : "FAIL",
                   cases[i].line[0] ? cases[i].line : "(empty)",
                   cases[i].want ? "" : "not");
            if (!ok) failures++;
        }
    }

    if (failures) {
        printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
