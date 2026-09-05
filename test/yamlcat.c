/* Read YAML on stdin, write the tree as JSON. The comparison harness's other
 * half; nothing else uses it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdyyaml.h"

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
    char err[256];
    mdy_yaml *doc = mdy_yaml_parse(buf, len, err, sizeof err);
    if (!doc) { fprintf(stderr, "%s\n", err); return 1; }
    char *json = mdy_yaml_to_json(mdy_yaml_root(doc));
    if (json) { fputs(json, stdout); fputc('\n', stdout); free(json); }
    mdy_yaml_free(doc);
    free(buf);
    return 0;
}
