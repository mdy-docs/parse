# CLAUDE.md

Onboarding for agents and humans. Read this first.

## What this is

A C implementation of mdy-docs' MDY front end, which turns document text into
a hast tree. It began as an investigation into whether porting the front end
out of JavaScript is worth doing; it now implements most of the language.

On the reference corpus it produces 100% of the JavaScript's nodes at 12.5×
its speed, and 26 of 87 documents come out byte-identical. See README.md for
what is still missing and for the numbers in full.

## Read these next, in order

1. `README.md` — what it does, what it does not, and the numbers. Ground truth
   for "is this worth continuing".
2. `docs/ARCHITECTURE.md` — the arena, the three stages, and the two
   measurements that shaped them (where the time goes, and what the tree
   actually contains).
3. `test/compare.mjs` — how correctness is established. Read this before
   changing any parsing rule.

## The rule for adding anything

**Measure against the JavaScript, do not read it and translate.** `make
compare` diffs both implementations over a real corpus and groups the first
differences by shape; `--first` shows one in full. Pick the most frequent
shape, implement it, watch the number move. That loop is why this repo is
laid out the way it is.

**Never write an expectation you have not asked the JavaScript for.** This has
gone wrong three times here and each cost real work:

- "An unclosed marker is text" — it is not; it opens a span that runs to the
  end. The wrong expectation went into a test, the C had the same wrong idea,
  the test passed, and 199 spurious `<em>` sat in the corpus until a histogram
  found them.
- "A single-column table is a degenerate table" — it is a paragraph, and `:-:`
  in it is an emoticon.
- "Non-ASCII is a letter" — an en dash is not, and `\p{L}` says so.

Getting a construct's exact output is a question for the JavaScript, asked
directly:

```sh
node --input-type=module -e "
import { fromMdy } from '<mdy-docs>/src/parse/block.js';
console.log(JSON.stringify(fromMdy('= Title', { script: false }), null, 1));
"
```

That is how every rule here was written, and it is faster and more reliable
than reading 37 KB of `block.js`.

## Conventions

- **Every exported symbol is `mdy_`-prefixed.** Not style: two C libraries
  already in this family export unprefixed names and linking both into one
  binary silently bound one's calls to the other's implementation.
- **No dependencies and no platform `#ifdef`s.** It builds anywhere C11 does.
- **The arena owns everything.** If you allocate outside it, you have
  introduced the first ownership rule in the codebase — do not.
- Comments say *why*, and record what was measured or what went wrong. The
  density here matches the rest of this family of repos.
