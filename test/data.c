/*
 * ```data fences — the YAML a document declares in its body.
 *
 * Every expectation was taken from mdy-docs' own extractDataBlocks, which
 * locates fences with a real CommonMark parse. That matters for the case this
 * exists to get right: a ```data shown INSIDE a longer outer fence is an
 * example, not data, and only a scanner that tracks fence state can tell.
 *
 * The corpus proves nothing here — this project has no data fences at all, in
 * 189 files — so these constructed cases are the whole of the coverage, and
 * they were checked against the reference rather than reasoned out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdydata.h"

static int failures = 0;

/* `sources` is every fence's YAML joined with a NUL, which is how the
 * generator wrote what the reference produced. */
static void check(const char *what, const char *source, size_t count,
                  const char *sources, const char *body) {
    mdy_data *data = mdy_data_extract(source, strlen(source));
    if (!data) { printf("  FAIL  %s (extract returned NULL)\n", what); failures++; return; }

    int ok = mdy_data_count(data) == count;
    if (ok) {
        const char *want = sources;
        for (size_t i = 0; i < count && ok; i++) {
            const mdy_data_fence *f = mdy_data_at(data, i);
            size_t n = strlen(want);
            ok = f->source_len == n && memcmp(f->source, want, n) == 0;
            if (!ok) printf("      fence %zu: expected %s, got %.*s\n", i, want,
                            (int)f->source_len, f->source);
            want += n + 1;
        }
    } else {
        printf("      expected %zu fence(s), got %zu\n", count, mdy_data_count(data));
    }

    size_t blen = 0;
    const char *got = mdy_data_body(data, &blen);
    if (ok && (strlen(body) != blen || memcmp(got, body, blen) != 0)) {
        ok = 0;
        printf("      body expected %s\n           got      %.*s\n", body, (int)blen, got);
    }

    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
    mdy_data_free(data);
}

int main(void) {
    printf("--- mdydata: ```data fences ---\n");
    check("a plain data fence", "text\n```data\na: 1\n```\ntail", 1,
          "a: 1", "text\ntail");
    check("two fences, in order", "```data\na: 1\n```\nmid\n```data\nb: 2\n```", 2,
          "a: 1\0" "b: 2", "mid");
    check("an empty fence is still removed", "```data\n```\ntail", 1,
          "", "tail");
    check("extra info keeps it as display", "```data foo\na: 1\n```\ntail", 0,
          "", "```data foo\na: 1\n```\ntail");
    check("another language is untouched", "```yaml\na: 1\n```\ntail", 0,
          "", "```yaml\na: 1\n```\ntail");
    check("uppercase DATA counts", "```DATA\na: 1\n```", 1,
          "a: 1", "");
    check("a tilde fence", "~~~data\na: 1\n~~~\ntail", 1,
          "a: 1", "tail");
    check("inside a longer outer fence it is display", "``````\n```data\na: 1\n```\n``````\ntail", 0,
          "", "``````\n```data\na: 1\n```\n``````\ntail");
    check("a longer data fence still counts", "``````data\na: 1\n``````\ntail", 1,
          "a: 1", "tail");
    check("an unclosed fence takes the rest", "text\n```data\na: 1", 1,
          "a: 1", "text");
    check("the content is dedented by the opener", "  ```data\n  a: 1\n  ```\ntail", 1,
          "a: 1", "tail");
    check("no fences at all", "just text\nand more", 0,
          "", "just text\nand more");

    if (failures) {
        printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
