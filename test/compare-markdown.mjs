/*
 * The markdown front end, held against mdy-docs'.
 *
 * mdy-docs turns `.md` into hast with
 *
 *     remarkParse → remarkGfm → remarkAlert → remarkRehype → rehypeRaw
 *
 * and stops at the tree. So this compares TREES, not HTML: what a `.md`
 * document becomes is a hast tree that everything downstream — composition,
 * `transform`, the TOC — then works on, and two pipelines agreeing on HTML
 * while disagreeing on the tree would be a difference nobody saw until a
 * transform ran.
 *
 * Run it before writing any C. With no C tool present it reports the
 * REFERENCE side alone — how much of the corpus remark reads, and what it
 * costs — which is the baseline any port is measured against. Point `--tool`
 * at a binary that reads markdown on stdin and writes the same canonical JSON
 * on stdout, and it compares.
 *
 *   node test/compare-markdown.mjs --mdy-docs <path> [--tool <binary>] [--first]
 */
import { execFileSync } from 'node:child_process';
import { readdirSync, readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const corpus = join(here, '..', 'build', 'corpus');
const argv = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i === -1 ? fallback : argv[i + 1];
};
const has = (name) => argv.includes(`--${name}`);

const mdyDocs = flag('mdy-docs');
const tool = flag('tool');
if (!mdyDocs) {
  console.error('usage: compare-markdown.mjs --mdy-docs <path> [--tool <binary>]');
  process.exit(2);
}
if (!existsSync(corpus)) {
  console.error('compare-markdown: no corpus — run `make corpus` first');
  process.exit(2);
}

const { markdownToHast } = await import(join(mdyDocs, 'src/markdown.js'));

/*
 * The tree, reduced to what both sides could agree on. Positions are left out
 * deliberately: remark's are mdast's, carried through remark-rehype, and a C
 * front end would have md4c's own offsets. Getting the SHAPE right comes
 * first, and a position comparison is a later and separate question.
 */
function canon(node) {
  if (node.type === 'text') return { type: 'text', value: node.value };
  if (node.type === 'comment') return { type: 'comment', value: node.value };
  if (node.type === 'doctype') return { type: 'doctype' };
  if (node.type === 'raw') return { type: 'raw', value: node.value };
  if (node.type === 'root') return { type: 'root', children: (node.children ?? []).map(canon) };
  /*
   * INSERTION ORDER, not sorted. hast keeps properties in the order they were
   * set and the HTML writer writes them in that order, so sorting one side
   * and not the other invents differences — which is what the first version
   * of this did, reporting every task list as wrong when the two agreed.
   */
  const props = {};
  for (const key of Object.keys(node.properties ?? {})) {
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

const files = [];
const walk = (d) => {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith('.md')) files.push(p);
  }
};
walk(corpus);
files.sort();

const groups = {};
let bytes = 0;
let nodes = 0;
let failed = 0;
let same = 0;
let shown = 0;
const started = Date.now();

for (const file of files) {
  const group = file.slice(corpus.length + 1).split('/')[0];
  groups[group] ??= { total: 0, same: 0, refFailed: 0 };
  groups[group].total += 1;

  const text = readFileSync(file, 'utf8');
  bytes += text.length;

  let expected;
  try {
    const tree = markdownToHast(text);
    let count = 0;
    (function walkTree(n) { count += 1; for (const c of n.children ?? []) walkTree(c); })(tree);
    nodes += count;
    expected = JSON.stringify(canon(tree));
  } catch (error) {
    groups[group].refFailed += 1;
    failed += 1;
    continue;
  }

  if (!tool) continue;

  let got;
  try {
    got = execFileSync(tool, [], { input: text, encoding: 'utf8', maxBuffer: 1 << 28 }).trim();
  } catch (error) {
    got = `ERROR: ${String(error.stderr ?? error.message).trim().split('\n')[0]}`;
  }
  if (got === expected) { same += 1; groups[group].same += 1; continue; }

  if (has('first') && shown < 3) {
    shown += 1;
    const n = Math.min(got.length, expected.length);
    let i = 0;
    while (i < n && got[i] === expected[i]) i++;
    console.log(`\n${file.slice(corpus.length + 1)}, first difference at byte ${i}:`);
    console.log(`   C: …${got.slice(Math.max(0, i - 60), i + 100)}`);
    console.log(`  JS: …${expected.slice(Math.max(0, i - 60), i + 100)}`);
  }
}

const ms = Date.now() - started;
console.log(`\ncorpus: ${files.length} documents, ${(bytes / 1024).toFixed(0)} KB`);
console.log(`reference: ${nodes} hast nodes in ${ms} ms${failed ? `, ${failed} that remark itself refused` : ''}`);

if (!tool) {
  console.log('\nno --tool given: the reference side only. This is the baseline.');
  for (const [g, s] of Object.entries(groups).sort()) {
    console.log(`  ${g.padEnd(26)} ${String(s.total).padStart(5)}`);
  }
  process.exit(0);
}

const pct = ((same / files.length) * 100).toFixed(1);
console.log(`\n${same}/${files.length} trees identical (${pct}%)`);
for (const [g, s] of Object.entries(groups).sort()) {
  console.log(`  ${g.padEnd(26)} ${String(s.same).padStart(5)}/${String(s.total).padEnd(5)}`);
}
process.exit(same === files.length ? 0 : 1);
