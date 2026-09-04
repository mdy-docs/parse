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

**Block rules** consume runs of lines. The one subtlety worth knowing is that
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
