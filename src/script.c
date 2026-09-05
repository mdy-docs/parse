/*
 * The MDY script layer — a port of mdy-docs' src/parse/script.js.
 *
 * The contract is in include/mdyscript.h. This is text in, JavaScript out,
 * and it depends on nothing: not the parser, not the writer, not a JS engine.
 * That is what lets the native backend compile a document ONCE and call the
 * result per render — the statements never mention the request.
 */
#include <stdlib.h>
#include <string.h>

#include "mdyscript.h"

/* ---- a growable string ----------------------------------------------------- */

typedef struct { char *s; size_t len, cap; int ok; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (!b->ok) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown) { b->ok = 0; return; }
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void put1(Buf *b, char c) { put(b, &c, 1); }
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static void put_size(Buf *b, size_t v) {
    char tmp[24];
    size_t n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) put1(b, tmp[--n]);
}

/* ---- lines ----------------------------------------------------------------- */

typedef struct { const char *s; size_t len; } Line;

/** `String(document).split(/\r\n|\r|\n/)` — the same three line endings, and
 * the same trailing empty line when the text ends with one. */
static Line *split_lines(const char *text, size_t len, size_t *count) {
    size_t n = 1;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') n++;
        else if (text[i] == '\r') { n++; if (i + 1 < len && text[i + 1] == '\n') i++; }
    }
    Line *lines = malloc(sizeof *lines * n);
    if (!lines) { *count = 0; return NULL; }

    size_t out = 0, start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len) { lines[out].s = text + start; lines[out].len = i - start; out++; break; }
        if (text[i] == '\r' || text[i] == '\n') {
            lines[out].s = text + start;
            lines[out].len = i - start;
            out++;
            if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') i++;
            start = i + 1;
        }
    }
    *count = out;
    return lines;
}

/*
 * `/^[ \t]*%(.*)$/` and `/^[ \t]*%%(.*)$/`.
 *
 * Both return what was written behind the sigil. A `%%` line matches BOTH, and
 * the caller that wants the block form has to ask for it first — the plain
 * form's capture on `%% x` is `% x`, not ` x`.
 */
static int match_script(const Line *l, size_t sigils, const char **rest, size_t *rest_len) {
    size_t i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++;
    for (size_t k = 0; k < sigils; k++) {
        if (i >= l->len || l->s[i] != '%') return 0;
        i++;
    }
    if (rest) { *rest = l->s + i; *rest_len = l->len - i; }
    return 1;
}

int mdy_script_is_line(const char *line, size_t len) {
    Line l = { line, len };
    return match_script(&l, 1, NULL, NULL);
}

/* ---- the scanner ----------------------------------------------------------- */

/*
 * Enough of JavaScript to know a bracket from a character that only looks like
 * one: quotes of all three kinds, and comments of both.
 *
 * NOT regular expressions — a `/` is a character here, so a pattern holding a
 * lone bracket throws the count off. That costs nothing, because a count that
 * never comes back to even takes no lines at all, and the `%%` line is left to
 * fail as the one line it is.
 */
typedef enum { M_SINGLE, M_DOUBLE, M_TEMPLATE, M_BLOCK, M_EXPRESSION } Mode;

enum { SCAN_MAX = 64 };

typedef struct {
    int depth;
    Mode modes[SCAN_MAX];
    int mode_count;
    int braces[SCAN_MAX];
    int brace_count;
} State;

static Mode quote_mode(char c) {
    return c == '\'' ? M_SINGLE : c == '"' ? M_DOUBLE : M_TEMPLATE;
}

static char quote_end(Mode m) {
    return m == M_SINGLE ? '\'' : m == M_DOUBLE ? '"' : '`';
}

static void push_mode(State *st, Mode m) {
    if (st->mode_count < SCAN_MAX) st->modes[st->mode_count++] = m;
}

static void scan(const char *text, size_t len, State *st) {
    size_t i = 0;
    while (i < len) {
        char c = text[i];
        char next = i + 1 < len ? text[i + 1] : '\0';
        int has_mode = st->mode_count > 0;
        Mode mode = has_mode ? st->modes[st->mode_count - 1] : M_SINGLE;

        if (has_mode && mode == M_BLOCK) {
            if (c == '*' && next == '/') { st->mode_count--; i++; }
            i++;
            continue;
        }

        if (has_mode && (mode == M_SINGLE || mode == M_DOUBLE || mode == M_TEMPLATE)) {
            if (c == '\\') { i += 2; continue; }
            if (c == quote_end(mode)) st->mode_count--;
            else if (mode == M_TEMPLATE && c == '$' && next == '{') {
                push_mode(st, M_EXPRESSION);
                if (st->brace_count < SCAN_MAX) st->braces[st->brace_count++] = 0;
                i++;
            }
            i++;
            continue;
        }

        /* A line comment runs to the end of whatever it was handed. */
        if (c == '/' && next == '/') return;

        if (c == '/' && next == '*') { push_mode(st, M_BLOCK); i += 2; continue; }

        if (c == '\'' || c == '"' || c == '`') { push_mode(st, quote_mode(c)); i++; continue; }

        if (c == '(' || c == '[') {
            st->depth++;
        } else if (c == ')' || c == ']') {
            if (st->depth > 0) st->depth--;
        } else if (c == '{') {
            st->depth++;
            if (has_mode && mode == M_EXPRESSION && st->brace_count > 0)
                st->braces[st->brace_count - 1]++;
        } else if (c == '}') {
            if (st->depth > 0) st->depth--;
            /* The `}` that closes a `${` hands the line back to its template. */
            if (has_mode && mode == M_EXPRESSION && st->brace_count > 0) {
                if (st->braces[st->brace_count - 1] == 0) {
                    st->mode_count--;
                    st->brace_count--;
                } else st->braces[st->brace_count - 1]--;
            }
        }
        i++;
    }
}

static int still_open(const State *st) { return st->depth > 0 || st->mode_count > 0; }

/* ---- which lines are code -------------------------------------------------- */

/*
 * A `%` line is one line of code, always. What it leaves open encloses the
 * markup under it — that is the whole of how a loop is written — so its
 * brackets are not counted and must not be.
 *
 * A `%%` line says the opposite, and runs on into the lines under it as far as
 * the line that brings its brackets back to even. Nothing is taken unless the
 * closing line is really there: an unclosed bracket leaves the `%%` line on its
 * own rather than swallowing the document behind it.
 */
static void mark_code(const Line *lines, size_t count, unsigned char *code) {
    for (size_t i = 0; i < count; i++) {
        if (!match_script(&lines[i], 1, NULL, NULL)) continue;
        code[i] = 1;

        const char *rest = NULL;
        size_t rest_len = 0;
        if (!match_script(&lines[i], 2, &rest, &rest_len)) continue;

        State st = {0};
        scan(rest, rest_len, &st);
        if (!still_open(&st)) continue;

        size_t last = i;
        for (size_t ahead = i + 1; ahead < count; ahead++) {
            /* Another code line starts its own; this one never closed. */
            if (match_script(&lines[ahead], 1, NULL, NULL)) break;
            scan(lines[ahead].s, lines[ahead].len, &st);
            if (!still_open(&st)) { last = ahead; break; }
        }
        if (last == i) continue;

        for (size_t line = i + 1; line <= last; line++) code[line] = 1;
        i = last;
    }
}

/* ---- content lines to template literals ------------------------------------ */

/*
 * `{{ expr }}` becomes `${expr}`, `\{{` becomes a literal `{{`, and the three
 * characters a template literal cannot hold plainly are escaped.
 *
 * An unclosed `{{` is not an interpolation: it falls through and is written
 * out as the two characters it is, which is what makes `{{cite book` in a
 * quoted passage survive.
 */
static void compile_line(Buf *b, const char *s, size_t len) {
    /* `/^([ \t]*)\\(%)/` -> `$1$2`: a leading `\%` is an escaped sigil. */
    size_t indent = 0;
    while (indent < len && (s[indent] == ' ' || s[indent] == '\t')) indent++;
    if (indent + 1 < len && s[indent] == '\\' && s[indent + 1] == '%') {
        put(b, s, indent);
        s += indent + 1;
        len -= indent + 1;
    }

    size_t i = 0;
    while (i < len) {
        char c = s[i];

        /* `character === '\\' && line.startsWith(opener, index + 1)` */
        if (c == '\\' && i + 2 < len && s[i + 1] == '{' && s[i + 2] == '{') {
            puts_(b, "{{");
            i += 3;
            continue;
        }

        if (c == '{' && i + 1 < len && s[i + 1] == '{') {
            /* `line.indexOf(closer, index + 2)` */
            size_t close = (size_t)-1;
            for (size_t k = i + 2; k + 1 < len; k++) {
                if (s[k] == '}' && s[k + 1] == '}') { close = k; break; }
            }
            if (close != (size_t)-1) {
                puts_(b, "${");
                put(b, s + i + 2, close - (i + 2));
                put1(b, '}');
                i = close + 2;
                continue;
            }
        }

        switch (c) {
            case '\\': puts_(b, "\\\\"); break;
            case '`':  puts_(b, "\\`");  break;
            case '$':  puts_(b, "\\$");  break;
            default:   put1(b, c);
        }
        i++;
    }
}

/* ---- the whole document ---------------------------------------------------- */

struct mdy_script {
    char *source;
    size_t source_len;
    unsigned char *code;
    size_t line_count;
};

mdy_script *mdy_script_compile(const char *text, size_t len) {
    if (!text) return NULL;
    if (len == 0) len = strlen(text);

    size_t count = 0;
    Line *lines = split_lines(text, len, &count);
    if (!lines) return NULL;

    unsigned char *code = calloc(count ? count : 1, 1);
    mdy_script *out = calloc(1, sizeof *out);
    if (!code || !out) { free(lines); free(code); free(out); return NULL; }

    mark_code(lines, count, code);

    Buf b = { NULL, 0, 0, 1 };
    puts_(&b, "const __out = []");

    for (size_t i = 0; i < count; i++) {
        put1(&b, '\n');
        if (code[i]) {
            /*
             * A line a `%%` took up is code ENTIRE; a `%` or `%%` line is what
             * it wrote behind its sigil. The block form is tried first because
             * a `%%` line matches the plain one too, and the plain one's
             * capture would leave the second `%` in the source.
             */
            const char *rest = NULL;
            size_t rest_len = 0;
            if (match_script(&lines[i], 2, &rest, &rest_len) ||
                match_script(&lines[i], 1, &rest, &rest_len)) {
                put(&b, rest, rest_len);
            } else {
                put(&b, lines[i].s, lines[i].len);
            }
        } else {
            puts_(&b, "__out.push([");
            put_size(&b, i);
            puts_(&b, ", `");
            compile_line(&b, lines[i].s, lines[i].len);
            puts_(&b, "`])");
        }
    }

    free(lines);
    if (!b.ok) { free(b.s); free(code); free(out); return NULL; }

    out->source = b.s ? b.s : calloc(1, 1);
    out->source_len = b.len;
    out->code = code;
    out->line_count = count;
    return out;
}

const char *mdy_script_source(const mdy_script *script, size_t *len) {
    if (!script) return NULL;
    if (len) *len = script->source_len;
    return script->source;
}

size_t mdy_script_line_count(const mdy_script *script) {
    return script ? script->line_count : 0;
}

int mdy_script_is_code(const mdy_script *script, size_t line) {
    if (!script || line >= script->line_count) return 0;
    return script->code[line];
}

void mdy_script_free(mdy_script *script) {
    if (!script) return;
    free(script->source);
    free(script->code);
    free(script);
}
