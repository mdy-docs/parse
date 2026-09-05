/* Split a source into documents, and each into front matter and body. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdydoc.h"
int main(void) {
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) { if (len + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
               size_t n = fread(buf + len, 1, 65536, stdin); len += n; if (n < 65536) break; }
    buf[len] = '\0';
    mdy_documents *d = mdy_split_documents(buf, len);
    printf("%zu\n", mdy_documents_count(d));
    for (size_t i = 0; i < mdy_documents_count(d); i++) {
        mdy_chunk c = mdy_documents_at(d, i);
        mdy_chunk m, b;
        mdy_split_frontmatter(c.text, c.len, &m, &b);
        printf("--matter %zu--\n%.*s\n--body %zu--\n%.*s\n", i, (int)m.len, m.text ? m.text : "",
               i, (int)b.len, b.text ? b.text : "");
    }
    mdy_documents_free(d);
    free(buf);
    return 0;
}
