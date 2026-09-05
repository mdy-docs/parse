/*
 * mdyast — parse MDY on stdin or from files, write the tree as JSON.
 *
 *   mdyast [--documents] [--no-frontmatter] [--no-autolink] [--no-sanitize] [file …]
 *
 * One JSON tree per file, one per line, so the output can be diffed against
 * the JavaScript's line by line. See test/compare.mjs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdyast.h"
#include "mdyhtml.h"
#include "mdyscript.h"

static char *read_all(FILE *f, size_t *len) {
    size_t cap = 1 << 16, used = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (used == cap) {
            char *grown = realloc(buf, cap *= 2);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        size_t got = fread(buf + used, 1, cap - used, f);
        used += got;
        if (got == 0) break;
    }
    *len = used;
    return buf;
}

static int emit_file(const char *path, const mdy_options *options, int stats, int messages, int html, int script) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "mdyast: cannot read %s\n", path); return 1; }
    size_t len = 0;
    char *text = read_all(f, &len);
    if (path) fclose(f);
    if (!text) { fprintf(stderr, "mdyast: out of memory\n"); return 1; }

    if (script) {
        /* The script layer runs BEFORE the parser — the statements produce the
         * lines the parser then reads — so this path never builds a tree. */
        mdy_script *s = mdy_script_compile(text, len);
        if (s) {
            size_t n = 0;
            const char *src = mdy_script_source(s, &n);
            fwrite(src, 1, n, stdout);
            fputc('\n', stdout);
            mdy_script_free(s);
        }
        free(text);
        return s ? 0 : 1;
    }

    mdy_doc *doc = mdy_parse(text, len, options);
    free(text);
    if (!doc) { fprintf(stderr, "mdyast: parse failed\n"); return 1; }

    if (messages) {
        for (size_t i = 0; i < mdy_frontmatter_count(doc); i++) {
            const mdy_frontmatter *m = mdy_frontmatter_at(doc, i);
            if (m->source) printf("frontmatter %u-%u: %s\n", m->open_line, m->close_line, m->source);
            else printf("frontmatter: (none)\n");
        }
        for (size_t i = 0; i < mdy_reference_count(doc); i++) {
            const mdy_reference *r = mdy_reference_at(doc, i);
            static const char *const KIND[] = { "tag", "mention", "link" };
            printf("ref[%u] %s %.*s\n", r->document, KIND[r->kind],
                   (int)r->name_len, r->name);
        }
        /* `<line>:<col>-<line>:<col>: <reason>`, the shape a vfile message
         * prints in — and just the reason when there is no place, which is
         * how the inline warnings are raised. */
        for (size_t i = 0; i < mdy_message_count(doc); i++) {
            const mdy_message *m = mdy_message_at(doc, i);
            if (m->line) {
                printf("%u:%u-%u:%u: %s\n", m->line, m->column, m->end_line,
                       m->end_column, m->reason);
            } else {
                printf("%s\n", m->reason);
            }
        }
    } else if (html) {
        /* The tree as HTML, which is what src/html.c is for. Kept beside the
         * JSON rather than in a tool of its own so the comparison harness can
         * ask one binary for both. */
        char *out = mdy_to_html(mdy_root(doc), NULL);
        if (out) { fputs(out, stdout); free(out); }
    } else if (stats) {
        printf("%s\t%zu bytes of source\t%zu bytes of tree\n",
               path ? path : "(stdin)", len, mdy_bytes(doc));
    } else {
        char *json = mdy_to_json(mdy_root(doc));
        if (json) { fputs(json, stdout); fputc('\n', stdout); free(json); }
    }
    mdy_free(doc);
    return 0;
}

int main(int argc, char **argv) {
    mdy_options options;
    mdy_options_default(&options);
    int stats = 0;
    int messages = 0;
    int html = 0;
    int script = 0;
    int files = 0, rc = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--documents") == 0) { options.documents = 1; continue; }
        if (strcmp(a, "--no-frontmatter") == 0) { options.frontmatter = 0; continue; }
        if (strcmp(a, "--no-autolink") == 0) { options.autolink = 0; continue; }
        if (strcmp(a, "--no-sanitize") == 0) { options.sanitize = 0; continue; }
        if (strcmp(a, "--stats") == 0) { stats = 1; continue; }
        if (strcmp(a, "--messages") == 0) { messages = 1; continue; }
        if (strcmp(a, "--html") == 0) { html = 1; continue; }
        if (strcmp(a, "--script") == 0) { script = 1; continue; }
        if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "mdyast: unknown option %s\n", a);
            return 2;
        }
        rc |= emit_file(a, &options, stats, messages, html, script);
        files++;
    }
    if (files == 0) rc |= emit_file(NULL, &options, stats, messages, html, script);
    return rc;
}
