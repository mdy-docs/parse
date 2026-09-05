/*
 * Does `mdy_to_html` agree with `hast-util-to-html`?
 *
 * The trap this avoids is comparing two whole pipelines and calling the result
 * a writer test. If the C parsed a document differently, the HTML would differ
 * and the writer would take the blame — so the SAME TREE goes to both sides:
 * the C parses, emits its tree as JSON, and writes its own HTML; node reads
 * that JSON and writes HTML from it with the original. Any difference is the
 * writer's, and there is nowhere else for it to have come from.
 *
 * A second pass then compares end to end — the JavaScript's own parse, written
 * by the JavaScript's own writer, against the C doing both — which is the
 * number that matters to anything embedding this.
 *
 * Every document is run twice, sanitised and not. The document engine turns
 * sanitizing OFF (src/mdy.js), so the unsanitised tree is the one a real build
 * writes, and it is the one carrying the attributes a schema would have
 * dropped.
 *
 *   node test/compare-html.mjs --mdy-docs <path> --corpus <path> [--first]
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
  console.error('usage: compare-html.mjs --mdy-docs <path to mdy-docs> [--corpus <dir>]');
  process.exit(2);
}

const { fromMdy } = await import(join(mdyDocs, 'src/parse/block.js'));
const { toHtml } = await import(join(mdyDocs, 'node_modules/hast-util-to-html/index.js'));

// What mdy-docs passes: `rehypeStringify` with `allowDangerousHtml: true` and
// every other option left at its default. See src/mdy.js.
const WRITE = { allowDangerousHtml: true };

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

const files = corpusDir && statSync(corpusDir, { throwIfNoEntry: false })?.isDirectory()
  ? findFiles(corpusDir)
  : [];

if (files.length === 0) {
  console.error(`compare-html: no .mdy files under ${corpusDir ?? '(no --corpus given)'}`);
  process.exit(2);
}

const binary = join(here, '..', 'build', 'mdyast');
const run = (args, file) =>
  execFileSync(binary, [...args, file], { encoding: 'utf8', maxBuffer: 1 << 28 });

/** Where two strings first differ, with a little either side. */
function firstDifference(a, b) {
  const n = Math.min(a.length, b.length);
  let i = 0;
  while (i < n && a[i] === b[i]) i++;
  if (i === n && a.length === b.length) return null;
  const from = Math.max(0, i - 60);
  return {
    at: i,
    c: a.slice(from, i + 90),
    js: b.slice(from, i + 90),
  };
}

const modes = [
  { name: 'sanitised', args: [] },
  { name: 'raw', args: ['--no-sanitize'], options: { sanitize: false } },
];

let writerSame = 0;
let writerTotal = 0;
let endToEndSame = 0;
let endToEndTotal = 0;
let bytes = 0;
let shown = 0;

for (const file of files) {
  const source = readFileSync(file, 'utf8');

  for (const mode of modes) {
    // ---- the writer alone: one tree, two writers -------------------------
    const cTree = JSON.parse(run(mode.args, file));
    const cHtml = run(['--html', ...mode.args], file);
    const jsFromCTree = toHtml(cTree, WRITE);

    writerTotal += 1;
    if (cHtml === jsFromCTree) writerSame += 1;
    else if (has('first') && shown < 1) {
      shown += 1;
      const d = firstDifference(cHtml, jsFromCTree);
      console.log(`\nfirst writer difference — ${file} (${mode.name}), at byte ${d.at}\n`);
      console.log(`   C: …${d.c}`);
      console.log(`  JS: …${d.js}\n`);
    }

    // ---- and end to end, which is what an embedder sees ------------------
    const jsHtml = toHtml(fromMdy(source, { highlight: false, ...(mode.options ?? {}) }), WRITE);
    endToEndTotal += 1;
    if (cHtml === jsHtml) endToEndSame += 1;
    bytes += jsHtml.length;
  }
}

const pct = (a, b) => ((a / b) * 100).toFixed(1);
console.log(
  `${writerSame}/${writerTotal} trees written identically (${pct(writerSame, writerTotal)}%) ` +
    `— the same tree through both writers`
);
console.log(
  `${endToEndSame}/${endToEndTotal} documents identical end to end (${pct(endToEndSame, endToEndTotal)}%) ` +
    `over ${(bytes / 1024).toFixed(0)} KB of HTML`
);

process.exit(writerSame === writerTotal && endToEndSame === endToEndTotal ? 0 : 1);
