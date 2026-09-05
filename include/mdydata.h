/*
 * The data a document declares in its body.
 *
 * A ```data fence is YAML, merged over the document's front matter — so the
 * code in a document may reference data declared anywhere, even below it. The
 * fences come out of the body before a line of that code runs, which is what
 * makes them order-independent.
 *
 * This is a pre-pass on RAW TEXT, like front matter and unlike everything else
 * here: it happens before the script layer, which happens before the parser.
 * So it takes text and gives back text, and knows about neither.
 *
 * A fence counts only when its info is exactly `data` — ```data foo stays
 * display content — and only when it is not already inside another fence, so
 * a ```data example shown inside a longer outer fence is an example.
 */
#ifndef MDYDATA_H
#define MDYDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *source;   /* the YAML between the fences, dedented */
    size_t source_len;
    uint32_t open_line;   /* 1-based, the opening fence */
    uint32_t close_line;  /* the closing fence, or the last line when unclosed */
} mdy_data_fence;

typedef struct mdy_data mdy_data;

/* Never fails on malformed input — an unclosed fence takes the rest of the
 * document, which is what a CommonMark parser does with one. NULL only on
 * allocation failure. */
mdy_data *mdy_data_extract(const char *text, size_t len);

size_t mdy_data_count(const mdy_data *data);
const mdy_data_fence *mdy_data_at(const mdy_data *data, size_t index);

/* The document with its data fences removed — what the script layer compiles. */
const char *mdy_data_body(const mdy_data *data, size_t *len);

void mdy_data_free(mdy_data *data);

#ifdef __cplusplus
}
#endif
#endif
