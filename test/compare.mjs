/*
 * Does the C agree with the JavaScript?
 *
 * That is the only question worth asking of this repo, and it is not one the
 * C tests can answer. A 4,441-line parser is not ported by reading it: it is
 * ported by producing the same tree for a real corpus, document by document,
 * and diffing — so this runs both over the same input, canonicalises each
 * side identically, and reports where they part company.
 *
 * The report is a coverage measure, not a pass/fail. This implementation is
 * deliberately partial; what matters is knowing exactly how partial, and what
 * the next divergence is. `--first` prints the first mismatching document with
 * the two trees side by side, which is how you pick the next thing to do.
 *
 *   node test/compare.mjs --mdy-docs <path> --corpus <path> [--first] [--bench]
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i === -1 ? fallback : argv[i + 1];
};
const has = (name) => argv.includes(`--${name}`);

const mdyDocs = flag('mdy-docs');
const corpusDir = flag('corpus');
if (!mdyDocs) {
  console.error('usage: compare.mjs --mdy-docs <path to mdy-docs> [--corpus <dir>]');
  process.exit(2);
}

const { fromMdy } = await import(join(mdyDocs, 'src/parse/block.js'));

/*
 * Both trees reduced to the same shape, so a difference in the report is a
 * difference in the PARSE and never in how one side happened to spell it.
 *
 * The JS tree carries `position` (source offsets) and `data` on some nodes;
 * the C does not produce those yet, so they are dropped from both. Everything
 * that survives — type, tagName, properties, children, text — is what
 * mdy-docs' own processing actually reads.
 */
function canon(node) {
  if (node.type === 'text') return { type: 'text', value: node.value };
  if (node.type === 'root') return { type: 'root', children: (node.children ?? []).map(canon) };
  const props = {};
  for (const key of Object.keys(node.properties ?? {}).sort()) {
    const v = node.properties[key];
    if (v === undefined || v === null || v === false) continue;
    props[key] = v;
  }
  return {
    type: 'element',
    tagName: node.tagName,
    properties: props,
    children: (node.children ?? []).map(canon),
  };
}

/** The same reduction over the C's JSON, whose property order is insertion
 * order rather than sorted. */
const canonJson = (json) => canon(JSON.parse(json));

function findFiles(dir) {
  const out = [];
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name.endsWith('.mdy')) out.push(p);
    }
  };
  walk(dir);
  return out.sort();
}

/** Where two canonical trees first differ, as a path like `children[3].tagName`. */
function firstDifference(a, b, path = '') {
  if (typeof a !== typeof b) return `${path}: ${typeof a} vs ${typeof b}`;
  if (a === null || b === null || typeof a !== 'object') {
    return a === b ? null : `${path}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
  }
  if (Array.isArray(a) !== Array.isArray(b)) return `${path}: array vs object`;
  if (Array.isArray(a)) {
    for (let i = 0; i < Math.max(a.length, b.length); i++) {
      if (i >= a.length) return `${path}[${i}]: missing on the C side, JS has ${JSON.stringify(b[i]).slice(0, 80)}`;
      if (i >= b.length) return `${path}[${i}]: extra on the C side: ${JSON.stringify(a[i]).slice(0, 80)}`;
      const d = firstDifference(a[i], b[i], `${path}[${i}]`);
      if (d) return d;
    }
    return null;
  }
  for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
    const d = firstDifference(a[k], b[k], path ? `${path}.${k}` : k);
    if (d) return d;
  }
  return null;
}

const files = corpusDir && statSync(corpusDir, { throwIfNoEntry: false })?.isDirectory()
  ? findFiles(corpusDir)
  : [];

if (files.length === 0) {
  console.error(`compare: no .mdy files under ${corpusDir ?? '(no --corpus given)'}`);
  process.exit(2);
}

const binary = join(here, '..', 'build', 'mdyast');
const options = { documents: true, script: false };
const cliArgs = ['--documents'];

if (has('bench')) {
  const texts = files.map((f) => readFileSync(f, 'utf8'));
  const chars = texts.reduce((n, t) => n + t.length, 0);

  let t0 = Date.now();
  for (const t of texts) fromMdy(t, options);
  const jsMs = Date.now() - t0;

  t0 = Date.now();
  execFileSync(binary, [...cliArgs, ...files], { maxBuffer: 1 << 30 });
  const cMs = Date.now() - t0;

  /*
   * Node counts on both sides, because a speed comparison between a complete
   * parser and a partial one is worth nothing without them. If the C produces
   * materially fewer nodes it is doing less work, and the ratio flatters it by
   * however much.
   */
  const count = (n) => 1 + (n.children ?? []).reduce((s, c) => s + count(c), 0);
  const jsNodes = texts.reduce((s, t) => s + count(fromMdy(t, options)), 0);
  const cOut = execFileSync(binary, [...cliArgs, ...files], { maxBuffer: 1 << 30, encoding: 'utf8' });
  const cNodes = cOut.split('\n').filter(Boolean).reduce((s, l) => s + count(JSON.parse(l)), 0);

  console.log(`${files.length} files, ${(chars / 1e6).toFixed(1)}M chars`);
  console.log(`  JavaScript (node/V8) : ${String(jsMs).padStart(5)} ms   ${jsNodes} nodes`);
  console.log(`  C                    : ${String(cMs).padStart(5)} ms   ${cNodes} nodes` +
              `  (${(cNodes / jsNodes * 100).toFixed(0)}% of them)`);
  console.log(`\n  ${(jsMs / cMs).toFixed(1)}x faster, process start included — and producing ` +
              `${(cNodes / jsNodes * 100).toFixed(0)}% of the nodes, so read it as an order of`);
  console.log('  magnitude rather than a figure. See README.md for what is not implemented.');
  process.exit(0);
}

// One process for the whole corpus, one JSON tree per line.
const out = execFileSync(binary, [...cliArgs, ...files], { maxBuffer: 1 << 30, encoding: 'utf8' });
const lines = out.split('\n').filter((l) => l.length > 0);

let same = 0;
const differences = new Map();   // first-difference path -> count
let firstShown = false;

for (let i = 0; i < files.length; i++) {
  const js = canon(fromMdy(readFileSync(files[i], 'utf8'), options));
  const c = canonJson(lines[i]);
  const diff = firstDifference(c, js);
  if (!diff) { same++; continue; }

  // Group by shape, not by exact text, so the report names causes.
  const shape = diff.replace(/\[\d+\]/g, '[]').replace(/: .*/, '');
  differences.set(shape, (differences.get(shape) ?? 0) + 1);

  if (has('first') && !firstShown) {
    firstShown = true;
    console.log(`\nfirst mismatch: ${files[i]}\n  ${diff}\n`);
  }
}

/*
 * Whole-document equality is the right bar eventually and a useless signal
 * now: one unimplemented construct anywhere in a 70 KB Wikipedia article makes
 * the document differ, so a partial implementation reads 0% however far along
 * it is. Node-level agreement is what moves while the work is in progress.
 */
const pct = ((same / files.length) * 100).toFixed(1);
console.log(`${same}/${files.length} documents identical (${pct}%)`);
const countNodes = (n) => 1 + (n.children ?? []).reduce((s, c) => s + countNodes(c), 0);
let jsTotal = 0, cTotal = 0;
for (let i = 0; i < files.length; i++) {
  jsTotal += countNodes(canon(fromMdy(readFileSync(files[i], 'utf8'), options)));
  cTotal += countNodes(canonJson(lines[i]));
}
console.log(`${cTotal} nodes produced against the JavaScript's ${jsTotal} ` +
            `(${(cTotal / jsTotal * 100).toFixed(0)}%)`);

/*
 * A tag histogram of both sides. This is the diagnostic that names what is
 * MISSING, where the first-difference report only says where the two trees
 * stopped agreeing — and for a partial implementation those are different
 * questions. The largest deficits are the work queue.
 */
const histogram = (tree, into) => {
  const walk = (n) => {
    if (n.type === 'element') into.set(n.tagName, (into.get(n.tagName) ?? 0) + 1);
    for (const c of n.children ?? []) walk(c);
  };
  walk(tree);
  return into;
};
const jsTags = new Map(), cTags = new Map();
for (let i = 0; i < files.length; i++) {
  histogram(fromMdy(readFileSync(files[i], 'utf8'), options), jsTags);
  histogram(canonJson(lines[i]), cTags);
}
const rows = [...new Set([...jsTags.keys(), ...cTags.keys()])]
  .map((tag) => ({ tag, js: jsTags.get(tag) ?? 0, c: cTags.get(tag) ?? 0 }))
  .map((r) => ({ ...r, gap: r.js - r.c }))
  .filter((r) => r.gap !== 0)
  .sort((a, b) => Math.abs(b.gap) - Math.abs(a.gap));

/*
 * Text accuracy: the concatenated text content of both trees, compared per
 * document. A tag histogram says the SHAPE is right; this says the words are —
 * and the two fail independently, since a missing typographic rule changes
 * every character of a paragraph without moving a single node count.
 */
const textOf = (n) => n.type === 'text' ? n.value : (n.children ?? []).map(textOf).join('');
let sameText = 0, textChars = 0, matchChars = 0;
for (let i = 0; i < files.length; i++) {
  const a = textOf(canonJson(lines[i]));
  const b = textOf(canon(fromMdy(readFileSync(files[i], 'utf8'), options)));
  if (a === b) sameText++;
  textChars += b.length;
  let k = 0;
  while (k < a.length && k < b.length && a[k] === b[k]) k++;
  matchChars += k;
}
console.log(`${sameText}/${files.length} documents with identical text ` +
            `(${(matchChars / textChars * 100).toFixed(2)}% of characters agree up to the first difference)`);

if (rows.length) {
  console.log('\nby tag, where the counts differ (JS - C):');
  for (const r of rows.slice(0, 14)) {
    console.log(`  ${r.tag.padEnd(12)} ${String(r.js).padStart(6)} vs ${String(r.c).padStart(6)}   ${r.gap > 0 ? '-' : '+'}${Math.abs(r.gap)}`);
  }
}
if (differences.size) {
  console.log('\nwhere they first differ, by frequency:');
  for (const [shape, n] of [...differences].sort((a, b) => b[1] - a[1]).slice(0, 12)) {
    console.log(`  ${String(n).padStart(4)}  ${shape}`);
  }
  console.log('\n  --first shows one in full.');
}
