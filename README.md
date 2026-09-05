# mdyast

The MDY front end in C: YAML, a document's script layer to JavaScript, its text
to a hast tree, and the tree back out as HTML.

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
make check-script # diff the SCRIPT layer against compileScript
make check-yaml  # read every YAML block the project holds, and compare
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

## The data a document declares

```c
#include "mdydata.h"

mdy_data *data = mdy_data_extract(body, len);
for (size_t i = 0; i < mdy_data_count(data); i++)
    mdy_yaml_parse(mdy_data_at(data, i)->source, ...);   /* merged over front matter */
const char *without = mdy_data_body(data, &len);         /* what the script compiles */
mdy_data_free(data);
```

A ` ```data ` fence is YAML the document declares in its body, merged over its
front matter. It comes out before a line of the document's code runs, which is
what makes the fences order-independent — code may reference data declared
anywhere, even below it.

The subtlety is fence STATE, not pattern matching: a ` ```data ` shown inside a
longer outer fence is an example, and only a scanner that knows it is already
inside one can tell. Its info must be exactly `data`, so ` ```data foo ` stays
display content.

**This project contains no data fences at all** — 189 files, none — so the
corpus proves nothing here and `test/data.c`'s twelve constructed cases are
the whole of the coverage. Every expectation came from mdy-docs'
`extractDataBlocks`, which locates them with a real CommonMark parse.

## YAML

```c
#include "mdyyaml.h"

char error[256];
mdy_yaml *doc = mdy_yaml_parse(text, len, error, sizeof error);
if (!doc) fprintf(stderr, "%s\n", error);   /* `line 12: what went wrong` */
const mdy_yaml_node *title = mdy_yaml_get(mdy_yaml_root(doc), "title");
mdy_yaml_free(doc);
```

For a document's front matter, its ` ```data ` fences, and the `.yaml` files a
site is built from. **YAML 1.2, core schema — correct rather than compatible.**
Where an implementation and the specification disagree this follows the
specification, and where a construct is not supported it says so with an error
naming the line rather than guessing. A parser that silently mis-reads data is
worse than one that refuses it: the data is what a site is built from.

The one that decides real files here is `Yes`. YAML 1.1 made it a boolean; 1.2's
core schema does not, and this corpus has `public-access: Yes` meaning the word.

It reads block mappings and sequences (including a sequence at its key's own
indent and the compact `- key: value` form), plain, single- and double-quoted
scalars that fold across lines, literal and folded block scalars with chomping
and explicit indentation, nested flow collections spanning lines, and comments.
It refuses anchors, aliases, merge keys, tags, explicit keys, multiple
documents and directives — none of which appears in the 179 YAML blocks
surveyed before a line was written, and each of which is a feature to add
rather than a corner to guess at.

```
make check-yaml   179/179 blocks read identically, 4.9 MB
```

`test/yaml.c` covers the language itself, construct by construct, and every
refusal with the line it names.

Two bugs were found by testing rather than by reading. A **mutation fuzz** —
2,000 truncated and corrupted inputs through an AddressSanitizer build — found
a block scalar with no content failing to advance the line cursor, which is not
a wrong value but an unbounded loop. And a **duplicate key** was quietly
replacing rather than failing, which the specification calls an error.

## The script layer: a document to JavaScript

```c
#include "mdyscript.h"

mdy_script *script = mdy_script_compile(text, len);
size_t n = 0;
const char *statements = mdy_script_source(script, &n);   /* hand to a sandbox */
mdy_script_free(script);
```

A document's `%` and `%%` lines are JavaScript and its content lines are
template literals, so a document compiles to the one run of statements that
produces its lines:

```
% for (const name of names) {        const __out = []
- {{ name }}                 ──►     for (const name of names) {
% }                                    __out.push([1, `- ${name}`])
                                     }
```

A port of mdy-docs' `src/parse/script.js`. **What runs the statements is
somebody else's business** — this produces source and knows about no engine,
which is what lets a host compile a document ONCE and call the result per
render, because the statements never mention the request.

`make check-script` holds it against the original: 145/145 documents, 14 MB of
JavaScript, byte-identical, plus the `code` map that says which lines went in
as code. `test/script.c` covers what a corpus does not — a brace inside a
string, inside a comment, inside a `${}`; a `%%` that never closes; an escaped
sigil; CRLF — and every expectation there was taken from `compileScript`
rather than reasoned out. A difference at this stage is not a different tree,
it is different behaviour.

`scriptBrackets` is not ported: it pairs brackets up for an editor to fold and
highlight, which is tooling rather than compilation.

## Markdown: vendored, and a corpus to check it against

mdy-docs speaks two markup languages. `.md` goes through a second front end —
`remarkParse → remarkGfm → remarkAlert → remarkRehype → rehypeRaw` — and stops
at the tree, so a `.md` document arrives as hast exactly as an `.mdy` one
does.

[md4c](https://github.com/mity/md4c) is vendored at `third_party/md4c` (MIT,
CommonMark-compliant, callback-based rather than AST-based, which suits
building a tree directly). `MD_DIALECT_GITHUB` covers permissive autolinks,
tables, strikethrough, task lists, admonitions and **footnotes**.

**The corpus came first, deliberately.** Every other C stage here is
trustworthy because it is diffed against the JavaScript over real input, and a
markdown front end had nothing to be diffed against: this project contains
zero `.md` files. So one is borrowed — `make corpus`:

```
  commonmark    652   spec 0.31.2      (CC-BY-SA 4.0, via third_party/md4c)
  ext-*         213   md4c's own extension specs, 16 of them
  gfm            39   spec 0.29        (CC-BY-SA 4.0, github/cmark-gfm)
  real          486   documents found on this machine

  1390 documents, 13.2 MB → build/corpus/
```

Three kinds of source because they fail differently. **Spec examples** are
dense in the corners a hand-written parser gets wrong — lazy continuation,
link reference definitions, HTML blocks, tight and loose lists. **Extension
specs** are where md4c and remark-gfm may simply disagree, which is worth
knowing. **Real documents** are neither: long, mundane, and full of what no
spec example bothers with. The GFM spec contributes only 39 because it is a
superset of CommonMark and the rest deduplicate.

Nothing is committed — the corpus is build output, and the spec files stay
inside md4c's own vendored copy with their licence.

Two baselines are established before a line of the front end is written:

```
make check-markdown   1390 documents, 290051 hast nodes in 10.9 s, remark
                      refusing none — the reference to be measured against
build/md4cprobe       md4c reads 1390/1390: 68591 blocks, 42286 spans,
                      483296 text runs, 12.2 MB of text, refusing none
```

`make check-markdown TOOL=<binary>` compares trees once there is something to
compare — trees rather than HTML, because what a `.md` document becomes is a
tree that composition and `transform` then work on.

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
