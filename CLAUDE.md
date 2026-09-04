# CLAUDE.md

Onboarding for agents and humans. Read this first.

## What this is

An **investigation**, in running code: a C implementation of mdy-docs' MDY
front end, which turns document text into a hast tree. Not a finished parser,
and it does not claim to be — see README.md's coverage table for exactly how
far it goes.

The question it answers is whether porting the front end out of JavaScript is
worth doing. On the reference corpus it is 23× faster than V8 while producing
62% of the nodes, and the comparison that matters is against QuickJS, which is
another 8.8× slower again.

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
