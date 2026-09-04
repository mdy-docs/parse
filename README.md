# mdyast

MDY document text to a hast tree, in C.

An investigation, and it is worth saying so up front: this is not a finished
parser and does not yet claim to be one. It exists to answer a specific
question with running code rather than an estimate — *what would it take to
parse MDY natively, and what would it be worth?*

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
  JavaScript (node/V8) :  2022 ms   284872 nodes
  C                    :    87 ms   176100 nodes  (62% of them)
```

**Read that as an order of magnitude, not a figure.** The C produces 62% of the
nodes because it implements about that much of the language, so it is doing
less work and the ratio flatters it. Even halving it for the missing 38%, and
even before the QuickJS-versus-V8 factor that is the actual comparison, the
answer to "is this worth doing" is not close.

## What it does today

Verified against mdy-docs' own parser, document by document
(`make compare`) — that is the only way a 4,441-line parser gets ported safely,
and the harness is as much the point of this repo as the parser is.

| | |
| --- | --- |
| **Block** | documents (`---` → `<article>`), front matter, headings with slugged ids, thematic breaks, fenced code, bullet and ordered lists, paragraphs with line joining |
| **Inline** | the nine toggling markers, backslash escapes, raw spans, autolink, wiki links (`[[ label ]]`, `[[ label \| url ]]`) |
| **Tree** | the three node types the corpus actually produces, arena-allocated, with interned tag and property names |

**Not implemented**, in rough order of how much of the remaining 38% each is
worth:

- **Footnotes.** `[[ ^id ]]` references and their definitions, collected into a
  `<section>` with backrefs. The corpus is Wikipedia-derived, so this is the
  single biggest gap: 15,846 `<sup>` and a large share of the 49,211 `<a>`.
  It needs a whole-document pass rather than an inline rule, which is why it is
  not here — a reference only becomes one if a definition exists.
- **Syntax highlighting** in fenced code. mdy-docs uses lowlight; a C
  implementation would need its own, and the corpus barely exercises it.
- **The `<element` block syntax**, which is where `<blockquote>`, `<table>` and
  `<figure>` come from.
- **Tables**, both the pipe syntax and alignment.
- **Tags and mentions** (`#tag`, `@user`), **emoji**, **em dash**, **ellipsis**,
  **arrows** — all small, all independent.
- **Sanitisation**, which mdy-docs applies to author-written HTML.
- **Positions.** Nodes carry `line`/`column` fields but the emitter does not
  write them, and the harness drops them from both sides. mdy-docs reports
  warnings against them, so they are part of the contract eventually.

## Build

```sh
make            # the library and the CLI
make test       # 20 checks, no node needed
make compare    # diff against mdy-docs' JavaScript over a real corpus
make bench      # how long each takes on the same input
```

`make compare` and `make bench` need mdy-docs and a corpus:

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

## Notes

**Every exported symbol is `mdy_`-prefixed, deliberately.** Two C projects
already in this family export unprefixed names like `compile_into` and
`vm_execute_internal`, and linking both into one binary silently bound one
library's calls to the other's differently versioned implementation. A prefix
is cheap; finding that is not.

**No dependencies**, not even libc beyond `malloc`/`free`/`str*`/`snprintf`.
No platform `#ifdef`s. It should build anywhere a C11 compiler does.
