/* Markdown on stdin, the hast tree as canonical JSON on stdout — the other
 * half of make check-markdown. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdymarkdown.h"

int main(void) {
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t n = fread(buf + len, 1, 65536, stdin);
        len += n;
        if (n < 65536) break;
    }
    buf[len] = '\0';
    mdy_doc *doc = mdy_markdown_parse(buf, len);
    if (!doc) { fprintf(stderr, "md4c refused this document\n"); return 1; }
    char *json = mdy_to_json_bare(mdy_root(doc));
    if (json) { fputs(json, stdout); fputc('\n', stdout); free(json); }
    mdy_free(doc);
    free(buf);
    return 0;
}
