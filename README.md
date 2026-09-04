# mdyast

MDY document text to a hast tree, in C.

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
  27/87 documents byte-identical
  284487 nodes against the JavaScript's 284872 (99.9%)
  33/87 documents with identical text
  99.98% of hrefs are ones the JavaScript also produces
```

Whole-document equality is a hard bar for a 70 KB Wikipedia article — one rule
missing anywhere makes the whole document differ — so the other three numbers
are what movement looks like while the work is in progress. What is left is
small and the harness names it: `a` −135, `p` +26, `em` −19 and `sup` +5, out
of 49,211, 14,217, 5,842 and 15,846.

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
| **Tree** | the three node types the corpus produces, arena-allocated, with interned tag and property names |

**Still missing:**

- **Syntax highlighting** in fenced code — and deliberately, not for now.
  mdy-docs uses lowlight, and highlighting is a *decoration* of a tree that is
  already correct: it replaces a `<code>` element's single text node with a run
  of `<span class="hljs-…">`. That is exactly the shape of thing to do
  afterwards, in the lamassu VM, on the tree this produces. Reimplementing
  lowlight in C would be a large piece of work to move a stage that does not
  need to move.
- **Emoji.** `:rocket:` and `:)` both become characters, and both need tables —
  the shortcode one is large. The corpus has 20 of them.
- **Positions.** Nodes carry `line`/`column` fields but the emitter does not
  write them, and the harness drops them from both sides. mdy-docs reports
  warnings against them, so they are part of the contract eventually.
- The last **0.3%** of links and paragraphs, which the harness names precisely
  and which is what `make compare --first` is for.

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
