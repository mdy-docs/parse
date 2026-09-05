/* The contract is in include/mdydoc.h. */
#include <stdlib.h>
#include <string.h>

#include "mdydoc.h"

struct mdy_documents {
    mdy_chunk *chunks;
    size_t count;
    char *joined;      /* the chunks, rebuilt with their own newlines */
};

static int blank_run(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') return 0;
    return 1;
}

/* `/^---[ \t]*$/` on a line that has already had its ending removed. */
static int is_separator(const char *s, size_t len) {
    if (len < 3 || s[0] != '-' || s[1] != '-' || s[2] != '-') return 0;
    for (size_t i = 3; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

/* `/^\+\+\+[ \t]*$/` */
static int is_fence(const char *s, size_t len) {
    if (len < 3 || s[0] != '+' || s[1] != '+' || s[2] != '+') return 0;
    for (size_t i = 3; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

mdy_documents *mdy_split_documents(const char *text, size_t len) {
    if (!text) return NULL;
    if (len == 0) len = strlen(text);

    mdy_documents *out = calloc(1, sizeof *out);
    if (!out) return NULL;

    /*
     * `source.split('\n')` then rejoin per chunk, which is what mdy-docs
     * does — so a chunk's own line endings are `\n` whatever the file used,
     * and the boundaries land where the separators were.
     */
    size_t cap = 4;
    mdy_chunk *chunks = malloc(cap * sizeof *chunks);
    char *joined = malloc(len + 2);
    if (!chunks || !joined) { free(chunks); free(joined); free(out); return NULL; }

    size_t written = 0;
    size_t chunk_start = 0;
    size_t count = 0;
    size_t line_start = 0;
    int wrote_line = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n') continue;
        size_t line_end = i;
        if (line_end > line_start && text[line_end - 1] == '\r') line_end--;

        if (is_separator(text + line_start, line_end - line_start)) {
            if (count == cap) {
                cap *= 2;
                mdy_chunk *g = realloc(chunks, cap * sizeof *g);
                if (!g) { free(chunks); free(joined); free(out); return NULL; }
                chunks = g;
            }
            chunks[count].text = joined + chunk_start;
            chunks[count].len = written - chunk_start;
            count++;
            joined[written++] = '\0';
            chunk_start = written;
            wrote_line = 0;
        } else {
            if (wrote_line) joined[written++] = '\n';
            memcpy(joined + written, text + line_start, line_end - line_start);
            written += line_end - line_start;
            wrote_line = 1;
        }
        line_start = i + 1;
    }

    if (count == cap) {
        cap += 1;
        mdy_chunk *g = realloc(chunks, cap * sizeof *g);
        if (!g) { free(chunks); free(joined); free(out); return NULL; }
        chunks = g;
    }
    chunks[count].text = joined + chunk_start;
    chunks[count].len = written - chunk_start;
    count++;
    joined[written] = '\0';

    /* Whitespace-only chunks are not documents. */
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (!blank_run(chunks[i].text, chunks[i].len)) chunks[kept++] = chunks[i];

    /* …unless nothing survives, in which case the source is ONE empty one. */
    if (kept == 0) {
        chunks[0].text = joined + written;
        chunks[0].len = 0;
        joined[written] = '\0';
        kept = 1;
    }

    out->chunks = chunks;
    out->count = kept;
    out->joined = joined;
    return out;
}

size_t mdy_documents_count(const mdy_documents *d) { return d ? d->count : 0; }

mdy_chunk mdy_documents_at(const mdy_documents *d, size_t i) {
    mdy_chunk none = { NULL, 0 };
    if (!d || i >= d->count) return none;
    return d->chunks[i];
}

void mdy_documents_free(mdy_documents *d) {
    if (!d) return;
    free(d->chunks);
    free(d->joined);
    free(d);
}

void mdy_split_frontmatter(const char *text, size_t len,
                           mdy_chunk *matter, mdy_chunk *body) {
    matter->text = NULL;
    matter->len = 0;
    body->text = text;
    body->len = len;
    if (!text) return;
    if (len == 0) len = strlen(text);
    body->len = len;

    /* Blank lines above the fence carry no meaning. */
    size_t i = 0;
    size_t open_start = 0, open_end = 0;
    for (;;) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        size_t trimmed = end;
        if (trimmed > i && text[trimmed - 1] == '\r') trimmed--;
        if (!blank_run(text + i, trimmed - i)) { open_start = i; open_end = trimmed; break; }
        if (end >= len) return;                 /* nothing but blank lines */
        i = end + 1;
    }

    if (!is_fence(text + open_start, open_end - open_start)) return;

    size_t after_open = open_end;
    while (after_open < len && text[after_open] != '\n') after_open++;
    if (after_open < len) after_open++;

    size_t at = after_open;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        size_t trimmed = end;
        if (trimmed > at && text[trimmed - 1] == '\r') trimmed--;
        if (is_fence(text + at, trimmed - at)) {
            matter->text = text + after_open;
            matter->len = at > after_open ? at - after_open - 1 : 0;   /* less the newline */
            size_t body_at = end < len ? end + 1 : len;
            body->text = text + body_at;
            body->len = len - body_at;
            return;
        }
        if (end >= len) break;
        at = end + 1;
    }
    /* An opening fence with no partner: the document is all body. */
}
