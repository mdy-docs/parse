/*
 * A differential probe: print every link this finds in the text on stdin, one
 * per line as `start end text`. test/linkify.mjs runs the real linkify-it over
 * the same input and diffs.
 *
 * Separate from the parser's CLI on purpose — a URL boundary is decided before
 * any tree exists, and a bug here should be visible without one in the way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int main(void) {
    static char buf[1 << 20];
    size_t len = fread(buf, 1, sizeof buf - 1, stdin);
    buf[len] = '\0';

    static mdy_link links[4096];
    size_t n = mdy_find_links(buf, len, links, 4096);
    for (size_t i = 0; i < n; i++) {
        printf("%zu %zu %.*s\n", links[i].start, links[i].end,
               (int)(links[i].end - links[i].start), buf + links[i].start);
    }
    return 0;
}
