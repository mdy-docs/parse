/*
 * YAML, read.
 *
 * The corpus harness (make check-yaml) covers the 4.9 MB of YAML this project
 * actually holds. These cover the language: every construct the parser claims
 * to read, and every one it claims to refuse.
 *
 * The expectations were taken from a reference implementation and then checked
 * against the specification where the two could differ — `Yes` is the one that
 * matters here, and it is a STRING. YAML 1.1 made it a boolean; 1.2's core
 * schema does not, and real front matter in this corpus says `public-access:
 * Yes` and means the word.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyyaml.h"

static int failures = 0;

static void check(const char *what, const char *source, const char *expected) {
    char err[256];
    mdy_yaml *doc = mdy_yaml_parse(source, strlen(source), err, sizeof err);
    if (!doc) {
        printf("  FAIL  %s\n      expected %s\n      actual   error: %s\n", what, expected, err);
        failures++;
        return;
    }
    char *json = mdy_yaml_to_json(mdy_yaml_root(doc));
    int ok = json && strcmp(json, expected) == 0;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) printf("      expected %s\n      actual   %s\n", expected, json ? json : "(null)");
    if (!ok) failures++;
    free(json);
    mdy_yaml_free(doc);
}

/* A construct this refuses, and the line it names. */
static void refuses(const char *what, const char *source, const char *expected) {
    char err[256];
    mdy_yaml *doc = mdy_yaml_parse(source, strlen(source), err, sizeof err);
    int ok = !doc && strcmp(err, expected) == 0;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      expected error %s\n      actual   %s\n", expected,
               doc ? "(it parsed)" : err);
        failures++;
    }
    mdy_yaml_free(doc);
}

int main(void) {
    printf("--- mdyyaml: the language ---\n");
    check("null forms", "a: ~\nb: null\nc: Null\nd: NULL\ne:",
          "{\"a\":null,\"b\":null,\"c\":null,\"d\":null,\"e\":null}");
    check("booleans are only true/false", "a: true\nb: True\nc: TRUE\nd: false\ne: False",
          "{\"a\":true,\"b\":true,\"c\":true,\"d\":false,\"e\":false}");
    check("Yes and No are STRINGS in 1.2", "a: Yes\nb: no\nc: on\nd: off\ne: y",
          "{\"a\":\"Yes\",\"b\":\"no\",\"c\":\"on\",\"d\":\"off\",\"e\":\"y\"}");
    check("decimal integers", "a: 0\nb: 42\nc: -7\nd: +5\ne: 007",
          "{\"a\":0,\"b\":42,\"c\":-7,\"d\":5,\"e\":7}");
    check("octal and hex", "a: 0o17\nb: 0x1A\nc: 0xff",
          "{\"a\":15,\"b\":26,\"c\":255}");
    check("binary is not 1.2", "a: 0b101",
          "{\"a\":\"0b101\"}");
    check("floats", "a: 1.5\nb: .5\nc: -0.25\nd: 1e3\ne: 1.5e-3\nf: 2.0",
          "{\"a\":1.5,\"b\":0.5,\"c\":-0.25,\"d\":1000,\"e\":0.0015,\"f\":2}");
    check("underscores are not 1.2", "a: 1_000",
          "{\"a\":\"1_000\"}");
    check("a date is a string", "a: 2024-01-01\nb: 12:30",
          "{\"a\":\"2024-01-01\",\"b\":\"12:30\"}");
    check("a colon inside a value", "a: x:y\nb: a#b",
          "{\"a\":\"x:y\",\"b\":\"a#b\"}");
    check("single quotes", "a: 'it''s'\nb: 'x'",
          "{\"a\":\"it's\",\"b\":\"x\"}");
    check("double quotes and escapes", "a: \"x\\ny\"\nb: \"tab\\there\"\nc: \"q\\\"uote\"\nd: \"\\u00e9\"",
          "{\"a\":\"x\\ny\",\"b\":\"tab\\there\",\"c\":\"q\\\"uote\",\"d\":\"é\"}");
    check("a quoted scalar folds across lines", "a: \"one\n  two\"",
          "{\"a\":\"one two\"}");
    check("an escaped line break suppresses the fold", "a: \"one\\\n  two\"",
          "{\"a\":\"onetwo\"}");
    check("a plain scalar folds across lines", "a: one\n  two",
          "{\"a\":\"one two\"}");
    check("a blank line inside a plain scalar", "a: one\n\n  two",
          "{\"a\":\"one\\ntwo\"}");
    check("literal keeps its breaks", "a: |\n  one\n  two\n",
          "{\"a\":\"one\\ntwo\\n\"}");
    check("literal, stripped", "a: |-\n  one\n  two\n",
          "{\"a\":\"one\\ntwo\"}");
    check("literal, kept", "a: |+\n  one\n\n",
          "{\"a\":\"one\\n\\n\"}");
    check("folded folds", "a: >\n  one\n  two\n",
          "{\"a\":\"one two\\n\"}");
    check("folded keeps a blank line", "a: >\n  one\n\n  two\n",
          "{\"a\":\"one\\ntwo\\n\"}");
    check("folded keeps a more-indented line", "a: >\n  one\n   two\n  three\n",
          "{\"a\":\"one\\n two\\nthree\\n\"}");
    check("an explicit indentation indicator", "a: |2\n   one\n  two\n",
          "{\"a\":\" one\\ntwo\\n\"}");
    check("a block scalar with no content", "a: >\nb: 1",
          "{\"a\":\"\",\"b\":1}");
    check("a sequence at its key's indent", "k:\n- a\n- b",
          "{\"k\":[\"a\",\"b\"]}");
    check("a sequence indented under its key", "k:\n  - a\n  - b",
          "{\"k\":[\"a\",\"b\"]}");
    check("the compact `- key: value` form", "- a: 1\n  b: 2\n- a: 3",
          "[{\"a\":1,\"b\":2},{\"a\":3}]");
    check("a sequence inside a sequence", "- - a\n  - b",
          "[[\"a\",\"b\"]]");
    check("a bare dash takes what is under it", "-\n  a: 1",
          "[{\"a\":1}]");
    check("nested mappings", "a:\n  b:\n    c: 1",
          "{\"a\":{\"b\":{\"c\":1}}}");
    check("an empty value is null", "a:\nb: 1",
          "{\"a\":null,\"b\":1}");
    check("a flow sequence", "k: [1, two, \"three\"]",
          "{\"k\":[1,\"two\",\"three\"]}");
    check("a flow mapping", "k: {a: 1, b: two}",
          "{\"k\":{\"a\":1,\"b\":\"two\"}}");
    check("nested flow", "k: [{a: 1}, [2, 3]]",
          "{\"k\":[{\"a\":1},[2,3]]}");
    check("flow across lines", "k: [\n  1,\n  2\n]",
          "{\"k\":[1,2]}");
    check("empty flow collections", "k: {}\nj: []",
          "{\"k\":{},\"j\":[]}");
    check("a flow value with no value", "k: {a: , b: 1}",
          "{\"k\":{\"a\":null,\"b\":1}}");
    check("a comment line", "# note\nk: v",
          "{\"k\":\"v\"}");
    check("a comment after a value", "k: v # note",
          "{\"k\":\"v\"}");
    check("a hash inside a word is not a comment", "k: a#b",
          "{\"k\":\"a#b\"}");
    check("trailing whitespace", "k: v   ",
          "{\"k\":\"v\"}");
    check("blank lines between keys", "a: 1\n\n\nb: 2",
          "{\"a\":1,\"b\":2}");
    check("an empty document", "",
          "null");
    check("only a comment", "# nothing",
          "null");
    check("a bare scalar document", "hello",
          "\"hello\"");
    check("a top-level sequence", "- a\n- b",
          "[\"a\",\"b\"]");
    check("a leading document marker", "---\na: 1",
          "{\"a\":1}");
    check("quoted keys", "\"a b\": 1\n'c': 2",
          "{\"a b\":1,\"c\":2}");
    check("a key that looks like a number", "1: a\ntrue: b",
          "{\"1\":\"a\",\"true\":\"b\"}");
    /* "It is an error for two equal keys to appear in the same mapping." */
    refuses("a repeated key is an error", "a: 1\na: 2", "line 2: duplicate key in a mapping");

    printf("--- mdyyaml: what it refuses, and where ---\n");
    /*
     * Each of these is a feature to add rather than a corner to guess at, and
     * none appears in the 179 YAML blocks this was built for. What matters is
     * that a refusal is loud: a parser that silently mis-reads data is worse
     * than one that stops.
     */
    refuses("anchors", "a: &x 1\nb: 2", "line 1: anchors and aliases are not supported");
    refuses("aliases", "a: 1\nb: *x", "line 2: anchors and aliases are not supported");
    refuses("tags", "a: !!str 1", "line 1: tags are not supported");
    refuses("merge keys", "<<: *base\na: 1", "line 1: merge keys are not supported");
    refuses("a second document", "a: 1\n---\nb: 2",
            "line 2: more than one document in a stream is not supported");
    refuses("directives", "%YAML 1.2\n---\na: 1", "line 1: directives are not supported");
    refuses("a tab for indentation", "a:\n\tb: 1", "line 2: a tab cannot be used for indentation");
    refuses("an unterminated quote", "a: \"x", "line 1: unterminated double-quoted scalar");
    refuses("an unterminated flow sequence", "a: [1, 2", "line 1: unterminated flow sequence");
    refuses("an unknown escape", "a: \"\\q\"",
            "line 1: unknown escape in a double-quoted scalar");

    printf("--- mdyyaml: reading a tree ---\n");
    {
        const char *src = "title: Uruk\nyears: [4000, 3100]\nfacts:\n  founded: true\n";
        char err[256];
        mdy_yaml *doc = mdy_yaml_parse(src, strlen(src), err, sizeof err);
        const mdy_yaml_node *root = mdy_yaml_root(doc);
        int ok = doc && mdy_yaml_type_of(root) == MDY_YAML_MAPPING &&
                 mdy_yaml_count(root) == 3;
        printf("  %s  a mapping knows its size\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        size_t len = 0;
        const char *title = mdy_yaml_string(mdy_yaml_get(root, "title"), &len);
        ok = title && len == 4 && memcmp(title, "Uruk", 4) == 0;
        printf("  %s  a string is reachable by key\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        const mdy_yaml_node *years = mdy_yaml_get(root, "years");
        ok = mdy_yaml_type_of(years) == MDY_YAML_SEQUENCE && mdy_yaml_count(years) == 2 &&
             mdy_yaml_number(mdy_yaml_at(years, 0)) == 4000;
        printf("  %s  a sequence is indexable\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_yaml_bool(mdy_yaml_get(mdy_yaml_get(root, "facts"), "founded")) == 1;
        printf("  %s  a nested mapping\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        const char *key = mdy_yaml_key(root, 0, &len);
        ok = key && len == 5 && memcmp(key, "title", 5) == 0;
        printf("  %s  keys keep the order they were written\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = mdy_yaml_get(root, "absent") == NULL &&
             mdy_yaml_at(years, 9) == NULL &&
             mdy_yaml_string(years, NULL) == NULL;
        printf("  %s  asking for what is not there answers nothing\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        mdy_yaml_free(doc);
    }

    if (failures) {
        printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
