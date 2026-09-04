/*
 * Footnotes. See internal.h for the three rules that make this a
 * document-level pass rather than an inline one.
 *
 * The ids and class names here are GitHub's, which is what mdy-docs emits and
 * what anything styling the output will expect: `user-content-fn-<id>` for a
 * definition, `user-content-fnref-<id>` for the reference that points at it,
 * and `-2`, `-3` … on the second and later references to the same note.
 */
#include <stdio.h>
#include <string.h>

#include "internal.h"

mdy_footnote *mdy_footnote_find(mdy_doc *doc, const char *id, size_t len) {
    for (size_t i = 0; i < doc->note_count; i++) {
        if (strlen(doc->notes[i].id) == len && memcmp(doc->notes[i].id, id, len) == 0) {
            return &doc->notes[i];
        }
    }
    return NULL;
}

int mdy_footnote_reference(mdy_doc *doc, mdy_footnote *note) {
    if (note->number == 0) note->number = ++doc->next_number;
    return ++note->refs;
}

/** `prefix` + id, and `-n` when n > 1 — the id scheme above. */
static const char *ref_id(mdy_doc *doc, const char *prefix, const char *id, int n) {
    char buf[256];
    if (n > 1) snprintf(buf, sizeof buf, "%s%s-%d", prefix, id, n);
    else snprintf(buf, sizeof buf, "%s%s", prefix, id);
    return mdy_strdup_n(&doc->arena, buf, strlen(buf));
}

void mdy_footnote_section(mdy_doc *doc, mdy_node *parent) {
    int referenced = 0;
    for (size_t i = 0; i < doc->note_count; i++) if (doc->notes[i].number) referenced++;
    if (!referenced) return;

    mdy_node *section = mdy_new_element(doc, "section", 7);
    mdy_set_bool(doc, section, "dataFootnotes", 1);
    mdy_add_class(doc, section, "footnotes");

    mdy_append(section, mdy_new_text(doc, "\n", 1));
    mdy_node *h2 = mdy_new_element(doc, "h2", 2);
    mdy_add_class(doc, h2, "sr-only");
    mdy_set_string(doc, h2, "id", "footnote-label", 14);
    mdy_append(h2, mdy_new_text(doc, "Footnotes", 9));
    mdy_append(section, h2);
    mdy_append(section, mdy_new_text(doc, "\n", 1));

    mdy_node *ol = mdy_new_element(doc, "ol", 2);
    mdy_append(ol, mdy_new_text(doc, "\n", 1));

    /* In order of first reference, which is what `number` records. */
    for (int want = 1; want <= doc->next_number; want++) {
        mdy_footnote *note = NULL;
        for (size_t i = 0; i < doc->note_count; i++) {
            if (doc->notes[i].number == want) { note = &doc->notes[i]; break; }
        }
        if (!note) continue;

        mdy_node *li = mdy_new_element(doc, "li", 2);
        mdy_set_string(doc, li, "id", ref_id(doc, "user-content-fn-", note->id, 1),
                       strlen(ref_id(doc, "user-content-fn-", note->id, 1)));
        mdy_append(li, mdy_new_text(doc, "\n", 1));

        mdy_node *p = mdy_new_element(doc, "p", 1);
        mdy_parse_inline(doc, p, note->content, note->content_len);

        /*
         * One backref per reference, each preceded by a space. With a single
         * reference the arrow stands alone; with more than one each carries a
         * <sup> saying which it goes back to.
         */
        for (int n = 1; n <= note->refs; n++) {
            mdy_append(p, mdy_new_text(doc, " ", 1));
            mdy_node *back = mdy_new_element(doc, "a", 1);
            char href[256];
            snprintf(href, sizeof href, "#%s", ref_id(doc, "user-content-fnref-", note->id, n));
            mdy_set_string(doc, back, "href", href, strlen(href));
            mdy_set_bool(doc, back, "dataFootnoteBackref", 1);
            mdy_set_string(doc, back, "ariaLabel", "Back to content", 15);
            mdy_add_class(doc, back, "data-footnote-backref");
            mdy_append(back, mdy_new_text(doc, "↩", 3));   /* ↩ */
            if (note->refs > 1) {
                mdy_node *sup = mdy_new_element(doc, "sup", 3);
                char num[16];
                snprintf(num, sizeof num, "%d", n);
                mdy_append(sup, mdy_new_text(doc, num, strlen(num)));
                mdy_append(back, sup);
            }
            mdy_append(p, back);
        }

        mdy_append(li, p);
        mdy_append(li, mdy_new_text(doc, "\n", 1));
        mdy_append(ol, li);
        mdy_append(ol, mdy_new_text(doc, "\n", 1));
    }

    mdy_append(section, ol);
    mdy_append(section, mdy_new_text(doc, "\n", 1));

    /*
     * The block loop has already emitted the trailing separator for whatever
     * came before, so the section slots into that gap and supplies the next
     * one — `… "\n" section "\n"`. Prepending another produced a doubled
     * newline before every footnotes section in the corpus, which is invisible
     * in a node count and the first text divergence in most documents.
     */
    if (parent->type == MDY_ELEMENT &&
        !(parent->last && parent->last->type == MDY_TEXT &&
          parent->last->text && strcmp(parent->last->text, "\n") == 0)) {
        mdy_append(parent, mdy_new_text(doc, "\n", 1));
    }
    mdy_append(parent, section);
    if (parent->type == MDY_ELEMENT) mdy_append(parent, mdy_new_text(doc, "\n", 1));
}
