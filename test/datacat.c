/* Data fences in, their YAML and the remaining body out — for the comparison. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdydata.h"
int main(void) {
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) { size_t n = fread(buf + len, 1, 65536, stdin); len += n; if (n < 65536) break;
               if (len + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); } }
    buf[len] = 0;
    mdy_data *d = mdy_data_extract(buf, len);
    if (!d) return 1;
    printf("%zu\n", mdy_data_count(d));
    for (size_t i = 0; i < mdy_data_count(d); i++) {
        const mdy_data_fence *f = mdy_data_at(d, i);
        printf("--fence %u-%u--\n%.*s\n", f->open_line, f->close_line, (int)f->source_len, f->source);
    }
    size_t bl = 0;
    const char *b = mdy_data_body(d, &bl);
    printf("--body--\n%.*s\n", (int)bl, b);
    mdy_data_free(d);
    free(buf);
    return 0;
}
