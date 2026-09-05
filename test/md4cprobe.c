/*
 * Does md4c read the corpus, and what does it see?
 *
 * Not a front end — that is the next piece of work. This is the check that
 * comes before it: md4c is vendored, it builds here, and it parses every
 * borrowed document without refusing or crashing. Counting the blocks and
 * spans it reports also says something the reference cannot, which is how
 * much STRUCTURE is in the corpus rather than how many bytes.
 *
 * MD_DIALECT_GITHUB is the closest preset to what mdy-docs asks remark for:
 * permissive autolinks, tables, strikethrough, task lists and footnotes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md4c.h"

typedef struct { long blocks, spans, text, bytes; } Counts;

static int enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    (void)type; (void)detail;
    ((Counts *)ud)->blocks++;
    return 0;
}
static int leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    (void)type; (void)detail; (void)ud;
    return 0;
}
static int enter_span(MD_SPANTYPE type, void *detail, void *ud) {
    (void)type; (void)detail;
    ((Counts *)ud)->spans++;
    return 0;
}
static int leave_span(MD_SPANTYPE type, void *detail, void *ud) {
    (void)type; (void)detail; (void)ud;
    return 0;
}
static int text_cb(MD_TEXTTYPE type, const MD_CHAR *s, MD_SIZE size, void *ud) {
    (void)type; (void)s;
    Counts *c = ud;
    c->text++;
    c->bytes += size;
    return 0;
}

int main(int argc, char **argv) {
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t n = fread(buf + len, 1, 65536, stdin);
        len += n;
        if (n < 65536) break;
    }

    MD_PARSER parser = {
        .abi_version = 0,
        .flags = MD_DIALECT_GITHUB,
        .enter_block = enter_block,
        .leave_block = leave_block,
        .enter_span = enter_span,
        .leave_span = leave_span,
        .text = text_cb,
    };

    Counts c = {0};
    int rc = md_parse(buf, (MD_SIZE)len, &parser, &c);
    if (rc != 0) { fprintf(stderr, "md4c refused this document (%d)\n", rc); return 1; }

    /* One line a shell can total up. */
    printf("%ld %ld %ld %ld\n", c.blocks, c.spans, c.text, c.bytes);
    (void)argc; (void)argv;
    free(buf);
    return 0;
}
