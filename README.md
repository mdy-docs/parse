# mdyast

MDY document text to a hast tree, in C — and the tree back out as HTML.

An investigation that grew into most of a parser. It answers a specific
question with running code rather than an estimate — *what would it take to
parse MDY natively, and what would it be worth?* — and it is checked against
mdy-docs' own parser over a real corpus, document by document.

## Why

mdy-docs parses MDY into [hast](https://github.com/syntax-tree/hast), and
everything downstream works on that tree: rehype plugins, the query engine, a
template's `$.render`. **hast is the extension point, and that is why the
native backend embeds a JavaScript engine at all.** Moving the parse into C
does not remove the extension point. It removes the largest single cost in
front of it.

That cost was measured, not guessed. Profiling a native corpus build
(`packages/mdy-native` in mdy-docs) put every frame in `JS_CallInternal`,
`js_array_flatten`, `js_array_every` and generators — the JavaScript layer,
with no native call appearing at all. The MDY front end is 4,441 lines of that
JavaScript and was measured at **8.8× slower under QuickJS than under V8**, the
worst ratio of any component.

## What the numbers say

The reference corpus: 87 Wikipedia-derived documents, 6.5 MB of text.

```
  JavaScript (node/V8) :  2030 ms   284872 nodes
  C                    :   163 ms   284175 nodes  (100% of them)
```

**12.5× faster, doing the same amount of work.** An earlier cut of this said
23×, and that number was flattered: it implemented 62% of the language, so it
was building 62% of the tree. This one builds the whole tree, and the honest
ratio is half of what the partial one advertised — which is worth recording,
because a benchmark against an incomplete implementation is a benchmark against
nothing.

The comparison that actually matters is against QuickJS, not V8. mdy-docs'
front end was measured at 8.8× slower under QuickJS, so a native backend that
parses in C rather than in its embedded JavaScript engine is looking at roughly
two orders of magnitude on this stage.

## How close is the tree?

`make compare` builds both and diffs them:

```
  87/87 documents byte-identical
  284872 nodes against the JavaScript's 284872
  87/87 documents with identical text
  20681 of the JavaScript's 20681 unist positions
  10514/10514 URL inputs agree with linkify-it exactly
```

**Every document, every node, every position.** The corpus is 87 Wikipedia
articles, 6.5 MB, and `make compare` diffs both implementations document by
document.

Whole-document equality is a hard bar for a 70 KB Wikipedia article — one rule
missing anywhere makes the whole document differ.

The last of them took a fix on the JavaScript side rather than this one:
mdy-docs numbered footnotes in the order the PARSER met them, and a paragraph
followed directly by a list has the list built first — so a reader saw markers
run 30 31 32 … 27 28 29. Numbering now happens once the tree is built, which is
the only place reading order exists. Fixed upstream; this never replicated it.

## URLs are linkify-it, ported

Every URL boundary is linkify-it's, because none of them is a rule anyone
guesses. `make check-links` runs both implementations over every line of the
corpus that could hold a link plus 48 edge cases aimed at the conditional
alternatives in its path grammar:

```
  10514/10514 inputs agree (100.0%)
```

See [src/linkify.c](src/linkify.c) for what the port covers and, more usefully,
why compiling linkify's own regexes was tried first and abandoned.

## What it does today

Verified against mdy-docs' own parser, document by document
(`make compare`) — that is the only way a 4,441-line parser gets ported safely,
and the harness is as much the point of this repo as the parser is.

| | |
| --- | --- |
| **Block** | documents (`---` → `<article>`), front matter, headings with slugged and de-duplicated ids, Setext underlining, thematic breaks, fenced code, paragraphs with line joining, the `<element` syntax, pipe tables with alignment |
| **Indentation** | structural, as MDY means it: a line further in than its run is a block of its own in a `<div>`, nesting as it goes; an element opener takes its indented lines as children; a list item absorbs its continuation |
| **Lists** | bullet and ordered, nested lists, continuation lines, and `[ ]`/`[x]` task items with their checkbox and list classes |
| **Inline** | the nine toggling markers, backslash escapes, raw spans, autolink (schemes and protocol-relative `//host`), wiki links, footnote references, `#tag` and `@user`, em dash, ellipsis, the six arrows |
| **Footnotes** | references, definitions, numbering by first reference, the collected `<section>` with per-reference backrefs |
| **Sanitisation** | the element allowlist, per-element attribute allowlists, and the `href`/`src` protocol check |
| **Attributes** | hast property naming (`class` → `className`, `colspan` → `colSpan`, `data-x` → `dataX`), with `className` as a list |
| **Emoji** | `:rocket:` and `:)`, from tables generated out of the same `gemoji` and `emoticon` packages mdy-docs imports — 2,235 entries, with the boundary rules that keep `12:30:45` and `http://x` from becoming faces |
| **Positions** | unist `{line, column}` on block elements, columns in UTF-16 units, `line_offset` honoured |
| **Unicode** | UTF-8 throughout, decoded strictly; `\p{L}`, `\p{N}` and case mapping from baru-re's generated UCD tables rather than approximated; a UTF-16 conversion for the JavaScript boundary, astral characters and all |
| **Tree** | the three node types the corpus produces, arena-allocated, with interned tag and property names |

**Still missing:**

- **Syntax highlighting** in fenced code — and deliberately, not for now.
  mdy-docs uses lowlight, and highlighting is a *decoration* of a tree that is
  already correct: it replaces a `<code>` element's single text node with a run
  of `<span class="hljs-…">`. That is exactly the shape of thing to do
  afterwards, in the lamassu VM, on the tree this produces. Reimplementing
  lowlight in C would be a large piece of work to move a stage that does not
  need to move.
- **`%` script lines**, which run JavaScript. That is the document engine's job
  rather than the parser's, and it needs a JS engine — which is exactly what
  the host embedding this already has.
- The last handful of nodes per tag, which the harness names precisely and
  which is what `make compare --first` is for.

## Build

```sh
make             # the library and the CLI
make test        # the C checks, no node needed
make compare     # diff the PARSER against mdy-docs' JavaScript over a corpus
make check-html  # diff the WRITER against hast-util-to-html
make check-links # diff the URL matching against linkify-it
make bench       # how long each takes on the same input
```

The `compare`/`check-*` targets need mdy-docs and a corpus:

```sh
make compare MDY_DOCS=~/projects/mdy-wikipedia-web/third-party/mdy-docs \
             CORPUS=~/projects/mdy-wikipedia-web/site/corpus
```

Nothing in the library depends on either; it builds and tests standalone.

## Using it

```c
#include "mdyast.h"

mdy_options options;
mdy_options_default(&options);
options.documents = 1;

mdy_doc *doc = mdy_parse(text, len, &options);
char *json = mdy_to_json(mdy_root(doc));   /* or walk mdy_root(doc) yourself */
free(json);
mdy_free(doc);                             /* frees every node at once */
```

The whole tree lives in one arena, so there is no per-node ownership anywhere
and `mdy_free` is the only cleanup.

JSON is the emitter that exists because it is what the comparison harness
needs. A host embedding this would want its own — building QuickJS objects
directly, or binjson for the WASM path — and the tree is deliberately
independent of any of them.

## The other direction: hast to HTML

```c
#include "mdyhtml.h"

char *html = mdy_to_html(mdy_root(doc), NULL);   /* NULL: mdy-docs' own settings */
free(html);
```

A port of [`hast-util-to-html`](https://github.com/syntax-tree/hast-util-to-html),
which is what mdy-docs writes its pages with — through `rehype-stringify`, with
`allowDangerousHtml: true` and every other option at its default.

**It shares nothing with the parser but the tree type.** The parser reads text
and produces a tree; `src/html.c` reads a tree and produces text; neither needs
the other. `test/html.c` proves it: every case there builds its tree from
plain C structs, with no arena, no document and no source text anywhere — 24
checks and not one of them parses anything.

`make check-html` is the differential harness, and it is careful about what it
is measuring. Comparing two whole pipelines would blame the writer for a
parser difference, so the SAME TREE goes through both: the C parses, emits its
tree as JSON, and writes its own HTML; node reads that JSON and writes HTML
from it with the original. A difference has nowhere else to have come from. A
second pass then compares end to end, which is the number an embedder cares
about. Both run over every document twice, sanitised and not.

```
290/290 trees written identically — the same tree through both writers
290/290 documents identical end to end, over 26 MB of HTML
```

Three options are deliberately absent, and `include/mdyhtml.h` says so at
length: `omitOptionalTags` (off in mdy-docs, and the largest part of the
original), the SVG schema, and `<template>`'s `content`. Each is a refusal to
guess rather than an oversight.

## Notes

**Every exported symbol is `mdy_`-prefixed, deliberately.** Two C projects
already in this family export unprefixed names like `compile_into` and
`vm_execute_internal`, and linking both into one binary silently bound one
library's calls to the other's differently versioned implementation. A prefix
is cheap; finding that is not.

**One dependency**, and only for its tables: `third_party/baru-re` supplies the
generated UCD data for `\p{L}`, `\p{N}` and simple lowercase. Referenced
directly rather than through its property lookup, so the linker keeps 8 KB of
it rather than 619 KB. Nothing else, not even libc beyond
`malloc`/`free`/`str*`/`snprintf`, and no platform `#ifdef`s — it should build
anywhere a C11 compiler does.
