# How it works

Read [../README.md](../README.md) first for what this is and why. This is the
inside.

## The shape of the problem

The tree was measured before anything was written, across the whole reference
corpus:

```
 3 node types      root, element, text
32 tag names       a, sup, p, li, em, td, img, figure, tr, br, h2-h4, …
19 property names  href, id, className, dataFootnoteRef, src, width, …
```

That is a much smaller thing than hast in general — no comments, no doctypes,
no raw nodes — and it is what makes a C implementation tractable rather than a
research project. Two consequences run through the whole design:

- **A closed vocabulary can be interned.** Tag and property names become one
  pointer each, so comparing a tag is a pointer compare and the emitter never
  copies a name.
- **A closed node model can be a plain struct.** No polymorphism, no visitor
  indirection, three cases in every switch.

## Where the time goes

Also measured first, by turning options off one at a time:

```
2012 ms   everything on (the real configuration)
1240 ms   block structure only, no inline at all
```

So block structure is 62% and inline the other 38%, and within each the cost is
spread thin — no single rule is more than a tenth. **There is no hot spot to
move.** A port has to cover the whole front end to be worth anything, which is
the main thing this measurement settled.

## The arena

`src/arena.c`. A bump allocator: nodes, text, property names and property
values all live in it, and `mdy_free` drops the whole thing in one call.

This is not only about tidiness. The JavaScript builds 285k nodes for the
reference corpus, each a separate heap object with its own properties object,
and that allocation traffic is a real share of what the port is trying to
remove. An arena is how a C implementation actually wins rather than
reproducing the same cost in a different language.

## The stages

**Lines first** (`src/block.c`). Indentation is structural in MDY — every two
columns is one level — so the source is split into a measured line array before
any rule runs. Every rule needs the width before it needs the content, and
measuring once is cheaper than measuring per rule.

**Block rules** consume runs of lines, at a **column** passed in rather than
inferred. That parameter carries the one rule everything else hangs off:
indentation is structural, so a line further in than its run is a block of its
own. At the root the column is 0 — a document whose first line is indented
opens with a `<div>` — while inside an element it is that element's children's
own indentation, or every child would get one. Inferring it from the first line
gets the root case wrong; inferring it from the parent gets the element case
wrong, and a first attempt that inferred it made 755 divs where the JavaScript
makes 40.

Three constructs claim indentation before that rule sees it: an element opener
takes its indented lines as children, a list item absorbs its continuation
lines, and a paragraph stops at any change of column. What is left over is a
div.

 The one subtlety worth knowing is that
block children of an *element* are separated by newline text nodes (`"\n" p
"\n" p "\n"`) and block children of the *root* are not. That asymmetry is the
JavaScript's, it is what makes stringified HTML come out one block per line
inside a container, and getting it wrong is the difference between an identical
tree and a nearly identical one.

**Inline** (`src/inline.c`). MDY's inline model is **toggling, not nesting**,
and that is the single most important thing about this stage. A marker sequence
opens a span; the next occurrence of the same sequence closes it. There is no
left-flanking/right-flanking analysis, no delimiter stack, none of CommonMark's
emphasis machinery. All nine markers are exactly two characters and none
prefixes another, so two bytes decide with no lookbehind — a single `*` is
literal text, and `a *b* c` is three words.

A marker only opens if its closer appears later, which is what makes an
unmatched `**` come out as two asterisks rather than swallowing the rest of the
line.

## Footnotes are a document pass

`src/footnote.c`, and the reason it is not an inline rule is worth stating: a
`[[ ^id ]]` becomes a footnote only if a definition exists somewhere in the
same document, so definitions are collected out of the line stream before any
parsing runs. Three rules follow from asking the JavaScript rather than reading
it:

- Numbering is by order of **first reference**, not of definition, and the list
  at the end is in that order too.
- A second reference to the same note gets id `…-2`, and the definition grows a
  second backref carrying a `<sup>` that says which.
- A definition nothing references is dropped, and a document with no referenced
  footnotes gets no section at all.

## Sanitisation is not optional

`src/attrs.c`. It is easy to read the schema as a safety feature that could be
skipped for a first cut, and that is wrong: `<td scope="col">` produces no
`scope` in the tree, because `scope` is allowed on `<th>` and not on `<td>`.
Skipping the check does not produce "slightly more" tree — it produces a
different one.

## Text is UTF-8, and the questions are about characters

`src/unicode.c`. UTF-8 everywhere — the source, the tree's text, the JSON —
with no conversion unless something asks for one.

That is safe for the same reason it is fast: **UTF-8 is self-synchronising**.
No byte of a multi-byte character can be mistaken for ASCII, so every rule that
scans for `|`, `-`, `[[` or a marker can walk bytes and be right, and most of
this parser does.

What cannot walk bytes is anything asking a question *about* a character — is
it a letter, what is its lowercase, should it be deleted. Each of the three
places that did was a bug:

- "Non-ASCII is a letter" kept en dashes in URLs, because `\p{L}` says they are
  not.
- A hand-written lowercase table covering Latin-1 and Latin Extended-A left `Ń`
  and `Ḫ` uppercase.
- Deleting a character by advancing one byte left the other two behind —
  mojibake in a URL, and invisible to anything counting nodes.

So the classification and case tables are **not ours**. They are baru-re's
generated UCD data, reached directly (`ucd_gc_Letter_ranges`,
`ucd_gc_Number_ranges`, `UCD_SIMPLE_LOWERCASE`) rather than through
`lookup_unicode_property`, which would pull in every property Unicode has: 8 KB
against 619 KB.

Decoding is strict, because a lenient decoder is how one bad byte shifts every
offset after it. Overlong forms, surrogates encoded as three bytes, and
anything above U+10FFFF are all ill-formed, and each is one byte consumed and
one U+FFFD — never a resynchronisation that eats what follows.

### The UTF-16 boundary

A boundary, not a representation. Every JavaScript engine — QuickJS, lamassu,
V8 — holds strings as UTF-16, so a tree built here crosses that conversion on
its way into one, and unist positions are counted in those units rather than in
characters or bytes.

The corpus has 1,351 astral characters in it, all Sumerian cuneiform, and each
is **one code point and two UTF-16 units**. Anything that conflates those
counts is wrong on exactly those characters, which is why they are what the
tests use — along with an unpaired surrogate, which cannot be encoded at all
and becomes U+FFFD rather than failing the document.

## URLs

`src/linkify.c` — a port of linkify-it, which is what mdy-docs uses.

**Why a port and not a heuristic.** Five of the eight documents that once
differed came down to URL boundaries, and each time the hand-rolled rule was
"close": a trailing comma kept in one place and dropped in another, a hyphen in
a host's last label, a full stop ending a sentence versus one inside a path.
Those are not rules anyone guesses.

**Why not its regexes.** The obvious route is to compile linkify's own patterns
with baru-re, which lamassu already links and which speaks the dialect they
need. It does not work, and it is worth writing down why so nobody spends the
afternoon: the patterns inline the Unicode classes rather than using `\p{...}`,
so `http_validator` alone is 31 KB and wants ~460 character classes against
baru-re's `MAX_CLASSES` of 256. Raising that limit segfaults. Measured, then
reverted.

That inlining is also what makes the C port *small*. Those 31 KB are `Z`, `P`
and `Cc` written out longhand; here they are three table lookups, and what is
left is the grammar — a few hundred lines.

**What it covers** is what `new LinkifyIt()` does with no options, which is
what mdy-docs constructs. The most useful thing to know about that default is
that **`fuzzyLink` is false**: a bare `example.com` is not a link, which takes
the TLD list out of everything except fuzzy email.

The path grammar is where the boundaries live, and its alternatives are
conditional on purpose: `,` and `;` continue a path only when something
follows, `.` only when what follows is neither another dot nor the end, `!` and
`?` only when not doubled. That is what leaves the full stop out of
"see http://example.com." while keeping the comma inside "http://x.com/a,b".

`make check-links` diffs the two implementations over every line of the corpus
that could hold a link, plus edge cases aimed at each conditional alternative.
It reports 10514/10514.

## Positions

unist positions, on block elements only — inline ones do not carry them, nor
does the root, nor the synthesised `<article>` and footnotes `<section>`, which
come from no source line.

Two details decide whether they are right. **Columns are UTF-16 units**, so
`a 𒀀 b` ends at column 7 rather than 6: the cuneiform sign is one character and
two units, and a position that counted characters would point at the wrong
place in every editor. And **columns are measured from the start of the line
including its indentation**, which is why an indented block still *starts* at
column 1 and ends past its own indent.

`mdy_to_json_bare` omits them. That exists because most of the smoke checks are
about what the tree IS, and threading a position through every expectation
would bury the thing being tested.

## Emoji

`src/emoji.c`, over a table generated by `scripts/generate_emoji.mjs` from the
same `gemoji` and `emoticon` packages mdy-docs imports. Data copied by hand is
data that drifts; regenerating after an upgrade is one command.

The matching is the interesting half, and both rules exist to stop false
positives a naive scan produces constantly. A shortcode must be a name gemoji
knows — that is what keeps `12:30:45` from being one, and why this corpus's
`:text:` and `:cts:` stay as they are. An emoticon must stand on its own:
something must have ended before it, and a letter or number must not follow.

`at_boundary` is **tracked state**, not computed from the previous byte, because
what sets it is what the scanner just did: a marker or a wiki link leaves a
boundary, an em dash does not, and an ordinary character leaves one only when
it is whitespace. Computing it from the text — treating `(` or `[` as a
boundary too — made faces out of ordinary prose and cost two documents' worth
of agreement before it was noticed.

The generator emits **three-digit octal escapes, not hex**. A C hex escape is
greedy, so `\x22D` is one invalid escape rather than `"` followed by `D` — and
`:"D` is a real emoticon.

## What is not here, on purpose

**Syntax highlighting.** It is a decoration of a tree that is already correct —
a `<code>` element's single text node becomes a run of `<span class="hljs-…">`
— so it is the clearest example of a stage that does not need to be in C at
all. The parse produces the tree; the VM can decorate it afterwards.

That division is worth stating generally, because it is the argument for this
whole repo: **hast is the extension point, and moving the parse does not move
it.** Anything that reads a finished tree and returns another one stays where
it is.

## Emitting

`mdy_to_json` writes the tree with a fixed key order and JSON.stringify's exact
escaping. That precision is the point: it is what lets `test/compare.mjs` diff
the two implementations byte for byte.

A host embedding this would want a different emitter — QuickJS objects built
directly, or binjson for a WASM path — and the tree is deliberately independent
of all of them. JSON exists because verification needs it.

## Verification

`test/compare.mjs` is as much the point of this repo as the parser. A
4,441-line parser is not ported by reading it; it is ported by producing the
same tree for a real corpus, document by document, and diffing. The harness
canonicalises both sides identically, groups first-differences by shape so the
report names causes rather than instances, and `--first` prints one in full.

It reports **node-level agreement** alongside whole-document equality, because
the latter is the right bar eventually and a useless signal now: one
unimplemented construct anywhere in a 70 KB article makes the document differ,
so a partial implementation reads 0% however far along it is.
