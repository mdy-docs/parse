/*
 * ```data fences — the contract is in include/mdydata.h.
 *
 * Fence state is tracked rather than pattern-matched, and that is the whole
 * subtlety: a ```data shown INSIDE a longer outer fence is an example, not
 * data, and only a scanner that knows it is already inside one can tell.
 */
#include <stdlib.h>
#include <string.h>

#include "mdydata.h"

typedef struct { char *s; size_t len, cap; int ok; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown) { b->ok = 0; return; }
        b->s = grown; b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

struct mdy_data {
    mdy_data_fence *fences;
    size_t count;
    char *body;
    size_t body_len;
    char *sources;        /* every fence's YAML, back to back */
};

typedef struct { const char *s; size_t len; size_t indent; } Line;

static size_t split(const char *text, size_t len, Line **out) {
    size_t n = 1;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') n++;
    Line *lines = malloc(sizeof *lines * n);
    if (!lines) return 0;
    size_t count = 0, start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n') continue;
        Line *l = &lines[count++];
        l->s = text + start;
        l->len = i - start;
        size_t k = 0;
        while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t')) k++;
        l->indent = k;
        start = i + 1;
    }
    *out = lines;
    return count;
}

/*
 * A fence opener: three or more backticks or tildes. A backtick fence may not
 * carry a backtick in its info, which is what stops a line of prose holding a
 * stray pair from opening one.
 *
 * `first`/`first_len` is the info's first word — its language — and `extra`
 * says whether anything follows it, because ```data foo is display content.
 */
static size_t fence_open(const Line *l, char *marker, const char **first,
                         size_t *first_len, int *extra) {
    size_t i = l->indent;
    if (i >= l->len) return 0;
    char c = l->s[i];
    if (c != '`' && c != '~') return 0;
    size_t width = 0;
    while (i + width < l->len && l->s[i + width] == c) width++;
    if (width < 3) return 0;

    const char *info = l->s + i + width;
    size_t info_len = l->len - i - width;
    while (info_len && (*info == ' ' || *info == '\t')) { info++; info_len--; }
    while (info_len && (info[info_len - 1] == ' ' || info[info_len - 1] == '\t')) info_len--;
    if (c == '`' && memchr(info, '`', info_len)) return 0;

    size_t word = 0;
    while (word < info_len && info[word] != ' ' && info[word] != '\t') word++;
    *first = info;
    *first_len = word;
    *extra = word < info_len;
    *marker = c;
    return width;
}

static int fence_closes(const Line *l, char marker, size_t width) {
    size_t i = l->indent;
    size_t n = 0;
    while (i + n < l->len && l->s[i + n] == marker) n++;
    if (n < width) return 0;
    for (size_t k = i + n; k < l->len; k++)
        if (l->s[k] != ' ' && l->s[k] != '\t') return 0;
    return 1;
}

static int is_data(const char *s, size_t len) {
    if (len != 4) return 0;
    static const char want[] = "data";
    for (size_t i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != want[i]) return 0;
    }
    return 1;
}

mdy_data *mdy_data_extract(const char *text, size_t len) {
    if (!text) return NULL;
    if (len == 0) len = strlen(text);

    mdy_data *out = calloc(1, sizeof *out);
    if (!out) return NULL;

    Line *lines = NULL;
    size_t count = split(text, len, &lines);
    if (!lines) { free(out); return NULL; }

    unsigned char *drop = calloc(count ? count : 1, 1);
    if (!drop) { free(lines); free(out); return NULL; }

    size_t cap = 4;
    mdy_data_fence *fences = malloc(cap * sizeof *fences);
    Buf sources = { NULL, 0, 0, 1 };
    size_t nfences = 0;
    if (!fences) { free(drop); free(lines); free(out); return NULL; }

    /* Where each fence's YAML starts in `sources`, fixed up once the buffer
     * has stopped moving. */
    size_t *offsets = malloc(cap * sizeof *offsets);
    if (!offsets) { free(fences); free(drop); free(lines); free(out); return NULL; }

    for (size_t i = 0; i < count; i++) {
        char marker = 0;
        const char *first = NULL;
        size_t first_len = 0;
        int extra = 0;
        size_t width = fence_open(&lines[i], &marker, &first, &first_len, &extra);
        if (!width) continue;

        size_t open_indent = lines[i].indent;
        size_t close = i + 1;
        while (close < count && !fence_closes(&lines[close], marker, width)) close++;
        /* An unclosed fence runs to the end, which is what a CommonMark parse
         * does with one. */
        size_t last = close < count ? close : count - 1;

        if (is_data(first, first_len) && !extra) {
            if (nfences == cap) {
                cap *= 2;
                mdy_data_fence *g = realloc(fences, cap * sizeof *g);
                size_t *go = realloc(offsets, cap * sizeof *go);
                if (!g || !go) { free(g ? g : fences); free(go ? go : offsets); goto fail; }
                fences = g; offsets = go;
            }
            offsets[nfences] = sources.len;
            for (size_t k = i + 1; k < (close < count ? close : count); k++) {
                /* The opener's own indentation is not content. */
                size_t strip = lines[k].indent < open_indent ? lines[k].indent : open_indent;
                if (sources.len > offsets[nfences]) put(&sources, "\n", 1);
                put(&sources, lines[k].s + strip, lines[k].len - strip);
            }
            put(&sources, "", 1);          /* a NUL between fences */
            fences[nfences].source_len = sources.len - offsets[nfences] - 1;
            fences[nfences].open_line = (uint32_t)i + 1;
            fences[nfences].close_line = (uint32_t)last + 1;
            nfences++;
            for (size_t k = i; k <= last; k++) drop[k] = 1;
        }
        i = last;                          /* past this fence either way */
    }

    /* The body without them, rejoined exactly as it was split. */
    Buf body = { NULL, 0, 0, 1 };
    for (size_t i = 0, written = 0; i < count; i++) {
        if (drop[i]) continue;
        if (written) put(&body, "\n", 1);
        put(&body, lines[i].s, lines[i].len);
        written++;
    }
    if (!body.ok || !sources.ok) { free(body.s); goto fail; }

    for (size_t i = 0; i < nfences; i++)
        fences[i].source = (sources.s ? sources.s : "") + offsets[i];

    out->fences = fences;
    out->count = nfences;
    out->sources = sources.s;
    out->body = body.s ? body.s : calloc(1, 1);
    out->body_len = body.len;
    free(offsets);
    free(drop);
    free(lines);
    return out;

fail:
    free(sources.s);
    free(offsets);
    free(drop);
    free(lines);
    free(out);
    return NULL;
}

size_t mdy_data_count(const mdy_data *d) { return d ? d->count : 0; }

const mdy_data_fence *mdy_data_at(const mdy_data *d, size_t i) {
    if (!d || i >= d->count) return NULL;
    return &d->fences[i];
}

const char *mdy_data_body(const mdy_data *d, size_t *len) {
    if (!d) return NULL;
    if (len) *len = d->body_len;
    return d->body;
}

void mdy_data_free(mdy_data *d) {
    if (!d) return;
    free(d->fences);
    free(d->sources);
    free(d->body);
    free(d);
}
