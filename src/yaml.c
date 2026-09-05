/*
 * YAML 1.2, core schema — the contract and the boundaries are in
 * include/mdyyaml.h.
 *
 * A line-oriented block parser with a recursive descent for flow collections,
 * which is the shape the language actually has: indentation decides structure,
 * and only inside `[` or `{` does that stop being true.
 *
 * It depends on nothing else here. Front matter is data, and a YAML reader has
 * no business knowing what a hast tree is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mdyyaml.h"

/* ---- allocation ------------------------------------------------------------
 *
 * One arena for the whole document, so freeing is one call and no node owns
 * anything. A 2.3 MB file of image metadata makes a lot of small nodes.
 */
typedef struct Chunk { struct Chunk *next; size_t used, cap; char data[]; } Chunk;

typedef struct { Chunk *head; } Arena;

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        Chunk *c = malloc(sizeof *c + cap);
        if (!c) return NULL;
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

static void arena_free(Arena *a) {
    for (Chunk *c = a->head; c;) { Chunk *next = c->next; free(c); c = next; }
    a->head = NULL;
}

/* ---- the tree --------------------------------------------------------------- */

typedef struct { const char *key; size_t key_len; mdy_yaml_node *value; } Pair;

struct mdy_yaml_node {
    mdy_yaml_type type;
    union {
        struct { const char *s; size_t len; } string;
        double number;
        int boolean;
        struct { mdy_yaml_node **items; size_t count; } seq;
        struct { Pair *pairs; size_t count; } map;
    } as;
};

struct mdy_yaml {
    Arena arena;
    mdy_yaml_node *root;
};

/* ---- a growable byte buffer, arena-backed at the end ------------------------ */

typedef struct { char *s; size_t len, cap; int ok; } Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown) { b->ok = 0; return; }
        b->s = grown; b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}
static void buf_putc(Buf *b, char c) { buf_put(b, &c, 1); }

/* ---- lines ------------------------------------------------------------------ */

typedef struct {
    const char *s;      /* after the indentation */
    size_t len;
    size_t indent;      /* spaces before it; a tab in indentation is an error */
    const char *raw;    /* including the indentation, for block scalars */
    size_t raw_len;
    int blank;          /* nothing but whitespace */
} Line;

typedef struct {
    Line *lines;
    size_t count, at;
    /* A text ending in a newline splits to one more line than it has, and that
     * phantom is not a blank line — it decides how many newlines a `|+` block
     * scalar keeps. */
    int trailing_newline;
    Arena *arena;
    char *error;
    size_t error_len;
    int failed;
} P;

static void fail(P *p, size_t line, const char *what) {
    if (p->failed) return;                 /* the first one is the useful one */
    p->failed = 1;
    if (p->error && p->error_len)
        snprintf(p->error, p->error_len, "line %zu: %s", line + 1, what);
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* A comment starts at a `#` that begins the line or follows whitespace — which
 * is why `a#b` is a value and not a value with a comment. */
static size_t comment_at(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '#') continue;
        if (i == 0 || is_space(s[i - 1])) return i;
    }
    return len;
}

/* ---- scalar resolution (the core schema) ------------------------------------ */

static int matches(const char *s, size_t len, const char *want) {
    return strlen(want) == len && memcmp(s, want, len) == 0;
}

/* `[-+]?[0-9]+`, `0o[0-7]+`, `0x[0-9a-fA-F]+` */
static int core_int(const char *s, size_t len, double *out) {
    if (len == 0) return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i + 2 < len && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'x')) {
        int base = s[i + 1] == 'o' ? 8 : 16;
        double v = 0;
        for (size_t k = i + 2; k < len; k++) {
            int d;
            char c = s[k];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            if (d >= base) return 0;
            v = v * base + d;
        }
        *out = neg ? -v : v;
        return 1;
    }
    if (i >= len) return 0;
    double v = 0;
    for (size_t k = i; k < len; k++) {
        if (s[k] < '0' || s[k] > '9') return 0;
        v = v * 10 + (s[k] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

/* `[-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?`, `.inf`, `.nan` */
static int core_float(const char *s, size_t len, double *out) {
    if (len == 0) return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }

    if (matches(s + i, len - i, ".inf") || matches(s + i, len - i, ".Inf") ||
        matches(s + i, len - i, ".INF")) {
        *out = neg ? -INFINITY : INFINITY;
        return 1;
    }
    if (i == 0 && (matches(s, len, ".nan") || matches(s, len, ".NaN") || matches(s, len, ".NAN"))) {
        *out = NAN;
        return 1;
    }

    size_t digits = 0, start = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    }
    if (digits == 0) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '-' || s[i] == '+')) i++;
        size_t ed = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; ed++; }
        if (ed == 0) return 0;
    }
    if (i != len) return 0;

    char tmp[64];
    size_t n = len - start < sizeof tmp - 1 ? len - start : sizeof tmp - 1;
    memcpy(tmp, s + start, n);
    tmp[n] = '\0';
    *out = neg ? -strtod(tmp, NULL) : strtod(tmp, NULL);
    return 1;
}

static mdy_yaml_node *new_node(P *p, mdy_yaml_type type) {
    mdy_yaml_node *n = arena_alloc(p->arena, sizeof *n);
    if (!n) { fail(p, p->at, "out of memory"); return NULL; }
    memset(n, 0, sizeof *n);
    n->type = type;
    return n;
}

static mdy_yaml_node *new_string(P *p, const char *s, size_t len) {
    mdy_yaml_node *n = new_node(p, MDY_YAML_STRING);
    if (!n) return NULL;
    char *copy = arena_alloc(p->arena, len + 1);
    if (!copy) { fail(p, p->at, "out of memory"); return NULL; }
    memcpy(copy, s, len);
    copy[len] = '\0';
    n->as.string.s = copy;
    n->as.string.len = len;
    return n;
}

/* A PLAIN scalar, resolved. Quoted scalars never come here: they are strings
 * whatever they spell. */
static mdy_yaml_node *resolve(P *p, const char *s, size_t len) {
    double v;
    if (len == 0 || matches(s, len, "~") || matches(s, len, "null") ||
        matches(s, len, "Null") || matches(s, len, "NULL"))
        return new_node(p, MDY_YAML_NULL);

    if (matches(s, len, "true") || matches(s, len, "True") || matches(s, len, "TRUE")) {
        mdy_yaml_node *n = new_node(p, MDY_YAML_BOOL);
        if (n) n->as.boolean = 1;
        return n;
    }
    if (matches(s, len, "false") || matches(s, len, "False") || matches(s, len, "FALSE")) {
        mdy_yaml_node *n = new_node(p, MDY_YAML_BOOL);
        if (n) n->as.boolean = 0;
        return n;
    }
    if (core_int(s, len, &v) || core_float(s, len, &v)) {
        mdy_yaml_node *n = new_node(p, MDY_YAML_NUMBER);
        if (n) n->as.number = v;
        return n;
    }
    return new_string(p, s, len);
}

/* ---- quoted scalars ---------------------------------------------------------
 *
 * Both kinds may run across lines, and a line break inside one FOLDS: it
 * becomes a space, and a blank line becomes a newline instead. That is the
 * same rule plain scalars follow, and it is why a 2 MB file of wrapped prose
 * reads back as the sentences somebody wrote.
 */

static void fold_break(Buf *out, size_t breaks) {
    /* One break is a space; every break after the first is kept as itself. */
    if (breaks == 0) return;
    if (breaks == 1) buf_putc(out, ' ');
    else for (size_t i = 1; i < breaks; i++) buf_putc(out, '\n');
}

/** `\x41`, `é`, `\U0001F600` — written back out as UTF-8. */
static void put_codepoint(Buf *out, unsigned cp) {
    if (cp < 0x80) buf_putc(out, (char)cp);
    else if (cp < 0x800) {
        buf_putc(out, (char)(0xC0 | (cp >> 6)));
        buf_putc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(out, (char)(0xE0 | (cp >> 12)));
        buf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(out, (char)(0xF0 | (cp >> 18)));
        buf_putc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex_digits(const char *s, size_t len, size_t n, unsigned *out) {
    if (len < n) return 0;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

/*
 * A quoted scalar starting at `line[from]`. Returns the line it ended on
 * through `end_line`, and where it ended on that line through `end_col`.
 */
static mdy_yaml_node *read_quoted(P *p, size_t line, size_t from, char quote,
                                  size_t *end_line, size_t *end_col) {
    Buf out = { NULL, 0, 0, 1 };
    size_t li = line;
    size_t i = from + 1;                 /* past the opening quote */
    size_t breaks = 0;
    int closed = 0;

    while (li < p->count) {
        const Line *l = &p->lines[li];
        while (i < l->len) {
            char c = l->s[i];
            if (quote == '\'') {
                if (c == '\'') {
                    if (i + 1 < l->len && l->s[i + 1] == '\'') { /* '' is one ' */
                        if (breaks) { fold_break(&out, breaks); breaks = 0; }
                        buf_putc(&out, '\'');
                        i += 2;
                        continue;
                    }
                    i++;
                    closed = 1;
                    break;
                }
            } else {
                /*
                 * A `\` at the end of a line is an ESCAPED LINE BREAK: it
                 * suppresses the fold entirely, so the next line joins with
                 * nothing between. Without it a URL broken across two lines
                 * comes back with a backslash and a space in the middle of it.
                 */
                if (c == '\\' && i + 1 == l->len) {
                    if (breaks) { fold_break(&out, breaks); breaks = 0; }
                    li++;
                    if (li >= p->count) { fail(p, line, "unterminated double-quoted scalar"); free(out.s); return NULL; }
                    i = 0;
                    /* The next line's own indentation is not content. */
                    while (i < p->lines[li].len && is_space(p->lines[li].s[i])) i++;
                    l = &p->lines[li];
                    continue;
                }
                if (c == '\\' && i + 1 < l->len) {
                    if (breaks) { fold_break(&out, breaks); breaks = 0; }
                    char e = l->s[i + 1];
                    i += 2;
                    unsigned cp = 0;
                    switch (e) {
                        case 'n': buf_putc(&out, '\n'); break;
                        case 't': buf_putc(&out, '\t'); break;
                        case 'r': buf_putc(&out, '\r'); break;
                        case 'b': buf_putc(&out, '\b'); break;
                        case 'f': buf_putc(&out, '\f'); break;
                        case '0': buf_putc(&out, '\0'); break;
                        case 'a': buf_putc(&out, '\a'); break;
                        case 'v': buf_putc(&out, '\v'); break;
                        case 'e': buf_putc(&out, 0x1B); break;
                        case '/': buf_putc(&out, '/'); break;
                        case '\\': buf_putc(&out, '\\'); break;
                        case '"': buf_putc(&out, '"'); break;
                        case ' ': buf_putc(&out, ' '); break;
                        case 'N': put_codepoint(&out, 0x85); break;
                        case '_': put_codepoint(&out, 0xA0); break;
                        case 'L': put_codepoint(&out, 0x2028); break;
                        case 'P': put_codepoint(&out, 0x2029); break;
                        case 'x': if (!hex_digits(l->s + i, l->len - i, 2, &cp)) {
                                      fail(p, li, "bad \\x escape"); free(out.s); return NULL;
                                  } i += 2; put_codepoint(&out, cp); break;
                        case 'u': if (!hex_digits(l->s + i, l->len - i, 4, &cp)) {
                                      fail(p, li, "bad \\u escape"); free(out.s); return NULL;
                                  } i += 4; put_codepoint(&out, cp); break;
                        case 'U': if (!hex_digits(l->s + i, l->len - i, 8, &cp)) {
                                      fail(p, li, "bad \\U escape"); free(out.s); return NULL;
                                  } i += 8; put_codepoint(&out, cp); break;
                        default:
                            fail(p, li, "unknown escape in a double-quoted scalar");
                            free(out.s);
                            return NULL;
                    }
                    continue;
                }
                if (c == '"') { i++; closed = 1; break; }
            }
            if (breaks) { fold_break(&out, breaks); breaks = 0; }
            buf_putc(&out, c);
            i++;
        }
        if (closed) break;

        /* The line ended without a closing quote: fold and continue. */
        breaks++;
        li++;
        if (li >= p->count) break;
        i = 0;
        /* Leading and trailing whitespace around a fold is dropped. */
        while (out.len && out.s[out.len - 1] == ' ') out.s[--out.len] = '\0';
    }

    if (!closed) {
        fail(p, line, quote == '\'' ? "unterminated single-quoted scalar"
                                    : "unterminated double-quoted scalar");
        free(out.s);
        return NULL;
    }

    mdy_yaml_node *n = new_string(p, out.s ? out.s : "", out.len);
    free(out.s);
    *end_line = li;
    *end_col = i;
    return n;
}

/* ---- block scalars ----------------------------------------------------------
 *
 * `|` keeps the line breaks it was written with; `>` folds them into spaces,
 * except around a blank line or a line indented further than the block, which
 * keep theirs. The chomping indicator decides the tail: `-` strips every
 * trailing newline, `+` keeps them all, and the default clips to one.
 */
static mdy_yaml_node *read_block_scalar(P *p, const char *header, size_t header_len,
                                        size_t parent_indent) {
    int folded = header[0] == '>';
    int chomp = 0;                        /* -1 strip, 0 clip, +1 keep */
    size_t explicit_indent = 0;

    for (size_t i = 1; i < header_len; i++) {
        char c = header[i];
        if (c == '-') chomp = -1;
        else if (c == '+') chomp = 1;
        else if (c >= '1' && c <= '9') explicit_indent = (size_t)(c - '0');
        else if (is_space(c)) break;
        else { fail(p, p->at, "bad block scalar header"); return NULL; }
    }

    size_t first = p->at + 1;
    size_t content_indent = explicit_indent ? parent_indent + explicit_indent : 0;

    if (!content_indent) {
        /* Auto-detected from the first non-empty line. */
        for (size_t i = first; i < p->count; i++) {
            if (p->lines[i].blank) continue;
            if (p->lines[i].indent <= parent_indent) break;
            content_indent = p->lines[i].indent;
            break;
        }
    }
    if (!content_indent) {
        /*
         * No content at all — `k: >` with nothing indented under it. The
         * cursor still has to move past the header, and forgetting that is
         * not a wrong value but an unbounded loop: the caller re-reads the
         * same key forever. A mutation fuzz found it in 400 inputs.
         */
        p->at = first;
        return new_string(p, "", 0);
    }

    Buf out = { NULL, 0, 0, 1 };
    size_t i = first;
    size_t pending_breaks = 0;
    int wrote_any = 0;
    int last_was_more_indented = 0;

    for (; i < p->count; i++) {
        const Line *l = &p->lines[i];
        if (l->blank) { pending_breaks++; continue; }
        if (l->indent < content_indent) break;

        /* The line, with the block's own indentation removed and any extra
         * kept — which is what makes a folded block able to hold a listing. */
        const char *s = l->raw + content_indent;
        size_t n = l->raw_len - content_indent;
        int more_indented = l->indent > content_indent;

        if (!wrote_any) {
            pending_breaks = 0;           /* leading blank lines are not content */
        } else if (!folded || more_indented || last_was_more_indented || pending_breaks) {
            /* Literal keeps every break. Folded keeps them around a blank line
             * and around a more-indented line, and folds only between two
             * ordinary ones. */
            size_t breaks = pending_breaks + 1;
            if (folded && !more_indented && !last_was_more_indented) {
                for (size_t k = 1; k < breaks; k++) buf_putc(&out, '\n');
            } else {
                for (size_t k = 0; k < breaks; k++) buf_putc(&out, '\n');
            }
            pending_breaks = 0;
        } else {
            buf_putc(&out, ' ');
        }

        buf_put(&out, s, n);
        wrote_any = 1;
        last_was_more_indented = more_indented;
    }

    p->at = i;

    /* The tail. `pending_breaks` counts the blank lines after the content, and
     * the content's own last line contributes one more. */
    if (wrote_any) {
        /* The phantom line the document's own final newline produced is not
         * one of the block's trailing blank lines. */
        if (i >= p->count && p->trailing_newline && pending_breaks) pending_breaks--;
        if (chomp > 0) for (size_t k = 0; k <= pending_breaks; k++) buf_putc(&out, '\n');
        else if (chomp == 0) buf_putc(&out, '\n');
    }

    mdy_yaml_node *node = new_string(p, out.s ? out.s : "", out.len);
    free(out.s);
    return node;
}

/* ---- flow collections --------------------------------------------------------
 *
 * Inside `[` or `{`, indentation stops deciding anything, so this is an
 * ordinary recursive descent over a cursor that walks off the end of one line
 * onto the next.
 */
typedef struct { P *p; size_t line, col; } Cur;

static void cur_skip(Cur *c) {
    for (;;) {
        const Line *l = &c->p->lines[c->line];
        while (c->col < l->len && is_space(l->s[c->col])) c->col++;
        if (c->col < l->len && l->s[c->col] == '#' &&
            (c->col == 0 || is_space(l->s[c->col - 1]))) c->col = l->len;
        if (c->col < l->len) return;
        if (c->line + 1 >= c->p->count) return;
        c->line++;
        c->col = 0;
    }
}

static int cur_at(Cur *c, char want) {
    const Line *l = &c->p->lines[c->line];
    return c->col < l->len && l->s[c->col] == want;
}

static mdy_yaml_node *parse_flow(Cur *c);

/* A plain scalar inside a flow collection ends at `,`, `]`, `}` or `: `. */
static mdy_yaml_node *flow_plain(Cur *c) {
    const Line *l = &c->p->lines[c->line];
    size_t start = c->col;
    size_t end = start;
    while (c->col < l->len) {
        char ch = l->s[c->col];
        if (ch == ',' || ch == ']' || ch == '}') break;
        if (ch == ':' && (c->col + 1 >= l->len || is_space(l->s[c->col + 1]) ||
                          l->s[c->col + 1] == ',' || l->s[c->col + 1] == ']' ||
                          l->s[c->col + 1] == '}')) break;
        if (ch == '#' && c->col > start && is_space(l->s[c->col - 1])) break;
        c->col++;
        if (!is_space(ch)) end = c->col;
    }
    return resolve(c->p, l->s + start, end - start);
}

static mdy_yaml_node *flow_scalar(Cur *c) {
    const Line *l = &c->p->lines[c->line];
    char ch = l->s[c->col];
    if (ch == '"' || ch == '\'') {
        size_t end_line = 0, end_col = 0;
        mdy_yaml_node *n = read_quoted(c->p, c->line, c->col, ch, &end_line, &end_col);
        c->line = end_line;
        c->col = end_col;
        return n;
    }
    if (ch == '&' || ch == '*' || ch == '!') {
        fail(c->p, c->line, ch == '!' ? "tags are not supported"
                                      : "anchors and aliases are not supported");
        return NULL;
    }
    return flow_plain(c);
}

static mdy_yaml_node *parse_flow(Cur *c) {
    cur_skip(c);
    if (c->p->failed) return NULL;

    if (cur_at(c, '[') || cur_at(c, '{')) {
        int is_map = cur_at(c, '{');
        char close = is_map ? '}' : ']';
        c->col++;

        mdy_yaml_node *node = new_node(c->p, is_map ? MDY_YAML_MAPPING : MDY_YAML_SEQUENCE);
        if (!node) return NULL;

        /* Collected loosely and copied into the arena once the count is known. */
        size_t cap = 8, count = 0;
        mdy_yaml_node **items = NULL;
        Pair *pairs = NULL;
        if (is_map) pairs = malloc(cap * sizeof *pairs);
        else items = malloc(cap * sizeof *items);
        if ((is_map && !pairs) || (!is_map && !items)) { fail(c->p, c->line, "out of memory"); return NULL; }

        for (;;) {
            cur_skip(c);
            if (c->p->failed) goto flow_fail;
            if (cur_at(c, close)) { c->col++; break; }
            if (c->col >= c->p->lines[c->line].len) {
                fail(c->p, c->line, is_map ? "unterminated flow mapping"
                                           : "unterminated flow sequence");
                goto flow_fail;
            }

            if (count == cap) {
                cap *= 2;
                void *grown = is_map ? (void *)realloc(pairs, cap * sizeof *pairs)
                                     : (void *)realloc(items, cap * sizeof *items);
                if (!grown) { fail(c->p, c->line, "out of memory"); goto flow_fail; }
                if (is_map) pairs = grown; else items = grown;
            }

            if (is_map) {
                mdy_yaml_node *k = flow_scalar(c);
                if (!k) goto flow_fail;
                cur_skip(c);
                if (!cur_at(c, ':')) { fail(c->p, c->line, "expected `:` in a flow mapping"); goto flow_fail; }
                c->col++;
                cur_skip(c);
                mdy_yaml_node *v;
                if (cur_at(c, ',') || cur_at(c, close)) v = new_node(c->p, MDY_YAML_NULL);
                else v = parse_flow(c);
                if (!v) goto flow_fail;
                size_t klen = 0;
                const char *ks = mdy_yaml_string(k, &klen);
                if (!ks) {
                    /* A non-string key is written as it resolved, which is what
                     * JSON does with an object key too. */
                    char tmp[64];
                    int n = 0;
                    if (k->type == MDY_YAML_NUMBER) n = snprintf(tmp, sizeof tmp, "%g", k->as.number);
                    else if (k->type == MDY_YAML_BOOL) n = snprintf(tmp, sizeof tmp, "%s", k->as.boolean ? "true" : "false");
                    else n = snprintf(tmp, sizeof tmp, "null");
                    mdy_yaml_node *s = new_string(c->p, tmp, (size_t)n);
                    if (!s) goto flow_fail;
                    ks = s->as.string.s;
                    klen = s->as.string.len;
                }
                pairs[count].key = ks;
                pairs[count].key_len = klen;
                pairs[count].value = v;
            } else {
                mdy_yaml_node *v = parse_flow(c);
                if (!v) goto flow_fail;
                items[count] = v;
            }
            count++;

            cur_skip(c);
            if (cur_at(c, ',')) { c->col++; continue; }
            if (cur_at(c, close)) { c->col++; break; }
            if (c->col >= c->p->lines[c->line].len) {
                fail(c->p, c->line, is_map ? "unterminated flow mapping"
                                           : "unterminated flow sequence");
            } else {
                fail(c->p, c->line, "expected `,` or a closing bracket in a flow collection");
            }
            goto flow_fail;
        }

        if (is_map) {
            Pair *out = arena_alloc(c->p->arena, (count ? count : 1) * sizeof *out);
            if (!out) { fail(c->p, c->line, "out of memory"); goto flow_fail; }
            memcpy(out, pairs, count * sizeof *out);
            node->as.map.pairs = out;
            node->as.map.count = count;
            free(pairs);
        } else {
            mdy_yaml_node **out = arena_alloc(c->p->arena, (count ? count : 1) * sizeof *out);
            if (!out) { fail(c->p, c->line, "out of memory"); goto flow_fail; }
            memcpy(out, items, count * sizeof *out);
            node->as.seq.items = out;
            node->as.seq.count = count;
            free(items);
        }
        return node;

    flow_fail:
        free(pairs);
        free(items);
        return NULL;
    }

    return flow_scalar(c);
}

/* ---- block structure --------------------------------------------------------- */

static mdy_yaml_node *parse_block(P *p, size_t indent);

/** The next line that is neither blank nor only a comment, or count. */
static size_t next_content(P *p, size_t from) {
    while (from < p->count) {
        const Line *l = &p->lines[from];
        if (!l->blank && !(l->len && l->s[0] == '#')) return from;
        from++;
    }
    return p->count;
}

/** A `- ` item, or a bare `-`. */
static int is_seq_item(const Line *l) {
    return l->len && l->s[0] == '-' && (l->len == 1 || is_space(l->s[1]));
}

/*
 * Where a mapping key ends: the `:` that is followed by whitespace or ends the
 * line, skipping over anything quoted or bracketed. Returns 0 when the line
 * does not open a mapping — `a:b` is a plain scalar, not a key.
 */
static size_t key_end(const Line *l) {
    size_t i = 0;
    int depth = 0;
    if (l->len && (l->s[0] == '"' || l->s[0] == '\'')) {
        char q = l->s[0];
        i = 1;
        while (i < l->len) {
            if (q == '\'' && l->s[i] == '\'' && i + 1 < l->len && l->s[i + 1] == '\'') { i += 2; continue; }
            if (q == '"' && l->s[i] == '\\') { i += 2; continue; }
            if (l->s[i] == q) { i++; break; }
            i++;
        }
    }
    for (; i < l->len; i++) {
        char c = l->s[i];
        if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') { if (depth) depth--; }
        else if (c == '#' && i && is_space(l->s[i - 1])) return 0;
        else if (c == ':' && depth == 0 && (i + 1 == l->len || is_space(l->s[i + 1])))
            return i;
    }
    return 0;
}

/*
 * A value written after `key:` or `- `, which may be a flow collection, a
 * block scalar header, a quoted scalar, or a plain one — and any of the last
 * three may run on to the lines below.
 */
static mdy_yaml_node *parse_value_from(P *p, size_t line, size_t col, size_t indent) {
    const Line *l = &p->lines[line];
    while (col < l->len && is_space(l->s[col])) col++;

    size_t end = comment_at(l->s, l->len);
    if (col >= end) {
        /* Nothing on this line: the value is whatever is indented below it, or
         * a sequence at this key's own indent. */
        p->at = line + 1;
        size_t next = next_content(p, p->at);
        if (next >= p->count) return new_node(p, MDY_YAML_NULL);
        const Line *n = &p->lines[next];
        if (n->indent > indent || (is_seq_item(n) && n->indent == indent)) {
            p->at = next;
            return parse_block(p, n->indent);
        }
        return new_node(p, MDY_YAML_NULL);
    }

    char c = l->s[col];

    if (c == '|' || c == '>') {
        p->at = line;
        return read_block_scalar(p, l->s + col, end - col, indent);
    }

    if (c == '&' || c == '*') { fail(p, line, "anchors and aliases are not supported"); return NULL; }
    if (c == '!') { fail(p, line, "tags are not supported"); return NULL; }

    if (c == '[' || c == '{') {
        Cur cur = { p, line, col };
        mdy_yaml_node *n = parse_flow(&cur);
        p->at = cur.line + 1;
        return n;
    }

    if (c == '"' || c == '\'') {
        size_t end_line = 0, end_col = 0;
        mdy_yaml_node *n = read_quoted(p, line, col, c, &end_line, &end_col);
        p->at = end_line + 1;
        return n;
    }

    /*
     * A plain scalar, which continues onto any following line that is indented
     * past the key and does not itself open something. Line breaks fold: one
     * becomes a space, and a blank line becomes a newline.
     */
    Buf out = { NULL, 0, 0, 1 };
    size_t stop = end;
    while (stop > col && is_space(l->s[stop - 1])) stop--;
    buf_put(&out, l->s + col, stop - col);

    size_t i = line + 1;
    size_t breaks = 0;
    for (; i < p->count; i++) {
        const Line *cont = &p->lines[i];
        if (cont->blank) { breaks++; continue; }
        if (cont->indent <= indent) break;
        if (is_seq_item(cont) || key_end(cont)) break;
        size_t cend = comment_at(cont->s, cont->len);
        while (cend > 0 && is_space(cont->s[cend - 1])) cend--;
        if (cend == 0) { breaks++; continue; }
        fold_break(&out, breaks + 1);
        breaks = 0;
        buf_put(&out, cont->s, cend);
    }
    p->at = i - breaks;

    mdy_yaml_node *n = resolve(p, out.s ? out.s : "", out.len);
    free(out.s);
    return n;
}

static mdy_yaml_node *parse_mapping(P *p, size_t indent) {
    mdy_yaml_node *node = new_node(p, MDY_YAML_MAPPING);
    if (!node) return NULL;

    size_t cap = 8, count = 0;
    Pair *pairs = malloc(cap * sizeof *pairs);
    if (!pairs) { fail(p, p->at, "out of memory"); return NULL; }

    for (;;) {
        size_t at = next_content(p, p->at);
        if (at >= p->count) break;
        const Line *l = &p->lines[at];
        if (l->indent != indent) break;
        if (is_seq_item(l)) break;

        size_t ke = key_end(l);
        if (!ke) { fail(p, at, "expected `key: value`"); goto map_fail; }
        if (l->s[0] == '?') { fail(p, at, "explicit keys are not supported"); goto map_fail; }

        /* The key, which may be quoted. */
        const char *ks;
        size_t klen;
        mdy_yaml_node *kn = NULL;
        if (l->s[0] == '"' || l->s[0] == '\'') {
            size_t el = 0, ec = 0;
            kn = read_quoted(p, at, 0, l->s[0], &el, &ec);
            if (!kn) goto map_fail;
            ks = kn->as.string.s;
            klen = kn->as.string.len;
        } else {
            size_t kend = ke;
            while (kend > 0 && is_space(l->s[kend - 1])) kend--;
            if (kend >= 2 && l->s[0] == '<' && l->s[1] == '<') {
                fail(p, at, "merge keys are not supported");
                goto map_fail;
            }
            kn = new_string(p, l->s, kend);
            if (!kn) goto map_fail;
            ks = kn->as.string.s;
            klen = kn->as.string.len;
        }

        p->at = at;
        mdy_yaml_node *v = parse_value_from(p, at, ke + 1, indent);
        if (!v) goto map_fail;

        if (count == cap) {
            cap *= 2;
            Pair *grown = realloc(pairs, cap * sizeof *pairs);
            if (!grown) { fail(p, at, "out of memory"); goto map_fail; }
            pairs = grown;
        }
        /*
         * "It is an error for two equal keys to appear in the same mapping."
         * The specification is explicit, and the alternative — quietly keeping
         * one of them — is a document that means something its author did not
         * write.
         */
        for (size_t k = 0; k < count; k++) {
            if (pairs[k].key_len == klen && memcmp(pairs[k].key, ks, klen) == 0) {
                fail(p, at, "duplicate key in a mapping");
                goto map_fail;
            }
        }
        pairs[count].key = ks;
        pairs[count].key_len = klen;
        pairs[count].value = v;
        count++;
    }

    {
        Pair *out = arena_alloc(p->arena, (count ? count : 1) * sizeof *out);
        if (!out) { fail(p, p->at, "out of memory"); goto map_fail; }
        memcpy(out, pairs, count * sizeof *out);
        node->as.map.pairs = out;
        node->as.map.count = count;
    }
    free(pairs);
    return node;

map_fail:
    free(pairs);
    return NULL;
}

static mdy_yaml_node *parse_sequence(P *p, size_t indent) {
    mdy_yaml_node *node = new_node(p, MDY_YAML_SEQUENCE);
    if (!node) return NULL;

    size_t cap = 8, count = 0;
    mdy_yaml_node **items = malloc(cap * sizeof *items);
    if (!items) { fail(p, p->at, "out of memory"); return NULL; }

    for (;;) {
        size_t at = next_content(p, p->at);
        if (at >= p->count) break;
        Line *l = &p->lines[at];
        if (l->indent != indent || !is_seq_item(l)) break;

        size_t after = 1;
        while (after < l->len && is_space(l->s[after])) after++;
        size_t rest = comment_at(l->s, l->len);

        mdy_yaml_node *v;
        if (after >= rest) {
            /* `-` alone: the item is what is indented below it. */
            p->at = at + 1;
            size_t next = next_content(p, p->at);
            if (next < p->count && p->lines[next].indent > indent) {
                p->at = next;
                v = parse_block(p, p->lines[next].indent);
            } else {
                v = new_node(p, MDY_YAML_NULL);
            }
        } else {
            /*
             * `- key: value` and `- - x` are the compact forms: the rest of the
             * line is a block of its own, starting at the column the content
             * does. Rewriting the line in place is exactly that statement — the
             * dash is consumed and what follows stands on its own.
             */
            Line saved = *l;
            l->s += after;
            l->len -= after;
            l->indent += after;
            l->raw = l->s;
            l->raw_len = l->len;
            p->at = at;
            v = parse_block(p, l->indent);
            *l = saved;
        }
        if (!v) goto seq_fail;

        if (count == cap) {
            cap *= 2;
            mdy_yaml_node **grown = realloc(items, cap * sizeof *items);
            if (!grown) { fail(p, at, "out of memory"); goto seq_fail; }
            items = grown;
        }
        items[count++] = v;
    }

    {
        mdy_yaml_node **out = arena_alloc(p->arena, (count ? count : 1) * sizeof *out);
        if (!out) { fail(p, p->at, "out of memory"); goto seq_fail; }
        memcpy(out, items, count * sizeof *out);
        node->as.seq.items = out;
        node->as.seq.count = count;
    }
    free(items);
    return node;

seq_fail:
    free(items);
    return NULL;
}

static mdy_yaml_node *parse_block(P *p, size_t indent) {
    size_t at = next_content(p, p->at);
    if (at >= p->count) return new_node(p, MDY_YAML_NULL);
    p->at = at;
    const Line *l = &p->lines[at];

    if (is_seq_item(l)) return parse_sequence(p, indent);
    if (key_end(l)) return parse_mapping(p, indent);

    /* A bare scalar document. */
    return parse_value_from(p, at, 0, indent == 0 ? 0 : indent - 1);
}

/* ---- the stream --------------------------------------------------------------- */

static Line *split_lines(const char *text, size_t len, size_t *count, P *p) {
    size_t n = 1;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') n++;
    Line *lines = malloc(sizeof *lines * n);
    if (!lines) return NULL;

    size_t out = 0, start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n') continue;
        size_t end = i;
        if (end > start && text[end - 1] == '\r') end--;

        Line *l = &lines[out++];
        l->raw = text + start;
        l->raw_len = end - start;

        size_t k = 0;
        while (k < l->raw_len && l->raw[k] == ' ') k++;
        /* A tab may not be indentation — the spec is explicit, and the failure
         * it otherwise causes is a structure that silently changes shape. */
        if (k < l->raw_len && l->raw[k] == '\t' && p)
            fail(p, out - 1, "a tab cannot be used for indentation");
        l->indent = k;
        l->s = l->raw + k;
        l->len = l->raw_len - k;

        l->blank = 1;
        for (size_t j = 0; j < l->len; j++)
            if (!is_space(l->s[j])) { l->blank = 0; break; }

        start = i + 1;
    }
    *count = out;
    return lines;
}

mdy_yaml *mdy_yaml_parse(const char *text, size_t len, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (!text) return NULL;
    if (len == 0) len = strlen(text);

    mdy_yaml *doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;

    P p = {0};
    p.arena = &doc->arena;
    p.error = error;
    p.error_len = error_len;
    p.trailing_newline = len > 0 && text[len - 1] == '\n';
    p.lines = split_lines(text, len, &p.count, &p);
    if (!p.lines) { free(doc); return NULL; }

    /* Directives and document markers: one document per stream here. */
    for (size_t i = 0; i < p.count && !p.failed; i++) {
        const Line *l = &p.lines[i];
        if (l->indent == 0 && l->len && l->s[0] == '%')
            fail(&p, i, "directives are not supported");
        if (l->indent == 0 && l->len >= 3 && memcmp(l->s, "---", 3) == 0 &&
            (l->len == 3 || is_space(l->s[3])) && i > 0)
            fail(&p, i, "more than one document in a stream is not supported");
        if (l->indent == 0 && l->len >= 3 && memcmp(l->s, "...", 3) == 0 && l->len == 3)
            fail(&p, i, "more than one document in a stream is not supported");
    }

    /* A leading `---` opening the one document is fine. */
    if (!p.failed && p.count && p.lines[0].indent == 0 && p.lines[0].len >= 3 &&
        memcmp(p.lines[0].s, "---", 3) == 0 && (p.lines[0].len == 3 || is_space(p.lines[0].s[3])))
        p.at = 1;

    if (!p.failed) doc->root = parse_block(&p, 0);

    if (!p.failed && doc->root) {
        /* Anything left over means the structure did not describe the file. */
        size_t at = next_content(&p, p.at);
        if (at < p.count) fail(&p, at, "unexpected content after the document");
    }

    free(p.lines);
    if (p.failed || !doc->root) { arena_free(&doc->arena); free(doc); return NULL; }
    return doc;
}

const mdy_yaml_node *mdy_yaml_root(const mdy_yaml *doc) { return doc ? doc->root : NULL; }

void mdy_yaml_free(mdy_yaml *doc) {
    if (!doc) return;
    arena_free(&doc->arena);
    free(doc);
}

mdy_yaml_type mdy_yaml_type_of(const mdy_yaml_node *n) { return n ? n->type : MDY_YAML_NULL; }

const char *mdy_yaml_string(const mdy_yaml_node *n, size_t *len) {
    if (!n || n->type != MDY_YAML_STRING) return NULL;
    if (len) *len = n->as.string.len;
    return n->as.string.s;
}

double mdy_yaml_number(const mdy_yaml_node *n) {
    return n && n->type == MDY_YAML_NUMBER ? n->as.number : 0;
}

int mdy_yaml_bool(const mdy_yaml_node *n) {
    return n && n->type == MDY_YAML_BOOL ? n->as.boolean : 0;
}

size_t mdy_yaml_count(const mdy_yaml_node *n) {
    if (!n) return 0;
    if (n->type == MDY_YAML_SEQUENCE) return n->as.seq.count;
    if (n->type == MDY_YAML_MAPPING) return n->as.map.count;
    return 0;
}

const mdy_yaml_node *mdy_yaml_at(const mdy_yaml_node *n, size_t i) {
    if (!n || n->type != MDY_YAML_SEQUENCE || i >= n->as.seq.count) return NULL;
    return n->as.seq.items[i];
}

const char *mdy_yaml_key(const mdy_yaml_node *n, size_t i, size_t *len) {
    if (!n || n->type != MDY_YAML_MAPPING || i >= n->as.map.count) return NULL;
    if (len) *len = n->as.map.pairs[i].key_len;
    return n->as.map.pairs[i].key;
}

const mdy_yaml_node *mdy_yaml_value(const mdy_yaml_node *n, size_t i) {
    if (!n || n->type != MDY_YAML_MAPPING || i >= n->as.map.count) return NULL;
    return n->as.map.pairs[i].value;
}

const mdy_yaml_node *mdy_yaml_get(const mdy_yaml_node *n, const char *k) {
    if (!n || n->type != MDY_YAML_MAPPING || !k) return NULL;
    size_t len = strlen(k);
    for (size_t i = 0; i < n->as.map.count; i++)
        if (n->as.map.pairs[i].key_len == len && memcmp(n->as.map.pairs[i].key, k, len) == 0)
            return n->as.map.pairs[i].value;
    return NULL;
}

/* ---- JSON, for the comparison harness ----------------------------------------- */

static void json_string(Buf *b, const char *s, size_t len) {
    buf_putc(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  buf_put(b, "\\\"", 2); break;
            case '\\': buf_put(b, "\\\\", 2); break;
            case '\b': buf_put(b, "\\b", 2); break;
            case '\f': buf_put(b, "\\f", 2); break;
            case '\n': buf_put(b, "\\n", 2); break;
            case '\r': buf_put(b, "\\r", 2); break;
            case '\t': buf_put(b, "\\t", 2); break;
            default:
                if (c < 0x20) { char tmp[8]; snprintf(tmp, sizeof tmp, "\\u%04x", c); buf_put(b, tmp, 6); }
                else buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

/*
 * `JSON.stringify` writes an integral double without a fraction, has no way to
 * write an infinity or a NaN — both become `null` — and otherwise writes the
 * SHORTEST decimal that reads back as the same double.
 *
 * That last one is not a detail: `%.17g` turns 26.185 into 26.184999999999999,
 * which is the same number and a different file. The shortest form is found by
 * asking for fewer digits and checking the answer still round-trips, which is
 * what a full Grisu implementation computes directly and what this arrives at
 * in at most three tries.
 */
static void json_number(Buf *b, double v) {
    if (isnan(v) || isinf(v)) { buf_put(b, "null", 4); return; }
    char tmp[40];
    if (v == (double)(long long)v && v < 9.2e18 && v > -9.2e18) {
        snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    } else {
        for (int digits = 15; digits <= 17; digits++) {
            snprintf(tmp, sizeof tmp, "%.*g", digits, v);
            if (strtod(tmp, NULL) == v) break;
        }
    }
    buf_put(b, tmp, strlen(tmp));
}

static void json_node(Buf *b, const mdy_yaml_node *n) {
    if (!n) { buf_put(b, "null", 4); return; }
    switch (n->type) {
        case MDY_YAML_NULL: buf_put(b, "null", 4); return;
        case MDY_YAML_BOOL: buf_put(b, n->as.boolean ? "true" : "false", n->as.boolean ? 4 : 5); return;
        case MDY_YAML_NUMBER: json_number(b, n->as.number); return;
        case MDY_YAML_STRING: json_string(b, n->as.string.s, n->as.string.len); return;
        case MDY_YAML_SEQUENCE:
            buf_putc(b, '[');
            for (size_t i = 0; i < n->as.seq.count; i++) {
                if (i) buf_putc(b, ',');
                json_node(b, n->as.seq.items[i]);
            }
            buf_putc(b, ']');
            return;
        case MDY_YAML_MAPPING:
            buf_putc(b, '{');
            for (size_t i = 0; i < n->as.map.count; i++) {
                if (i) buf_putc(b, ',');
                json_string(b, n->as.map.pairs[i].key, n->as.map.pairs[i].key_len);
                buf_putc(b, ':');
                json_node(b, n->as.map.pairs[i].value);
            }
            buf_putc(b, '}');
            return;
    }
}

char *mdy_yaml_to_json(const mdy_yaml_node *n) {
    Buf b = { NULL, 0, 0, 1 };
    buf_put(&b, "", 0);
    json_node(&b, n);
    if (!b.ok) { free(b.s); return NULL; }
    return b.s;
}
