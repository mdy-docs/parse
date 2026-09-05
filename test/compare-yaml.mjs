/*
 * Does `mdy_yaml_parse` read YAML correctly?
 *
 * The reference is the `yaml` package, which is a careful YAML 1.2
 * implementation — but it is a REFERENCE, not the specification. Where the two
 * disagree the question is which is right, and the answer goes in the report
 * rather than into a silent adjustment. What must never happen is this parser
 * reading data differently and nobody noticing.
 *
 * The corpus is every scrap of YAML the project actually parses: the front
 * matter of every document, every ```data fence, and every .yaml file.
 *
 *   node test/compare-yaml.mjs --mdy-docs <path> --corpus <dir> [--first]
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
const roots = [];
for (let i = 0; i < argv.length; i++) if (argv[i] === '--corpus') roots.push(argv[i + 1]);
if (!mdyDocs || roots.length === 0) {
  console.error('usage: compare-yaml.mjs --mdy-docs <path> --corpus <dir> [--corpus <dir>]');
  process.exit(2);
}

const { parse } = await import(join(mdyDocs, 'node_modules/yaml/dist/index.js'));

const files = [];
const walk = (d) => {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, e.name);
    if (e.isDirectory()) { if (e.name !== 'node_modules' && e.name !== '.git') walk(p); }
    else if (/\.(mdy|ya?ml)$/.test(e.name)) files.push(p);
  }
};
for (const r of roots) if (statSync(r, { throwIfNoEntry: false })?.isDirectory()) walk(r);

/* Every YAML block the engine would hand to a parser. */
const blocks = [];
for (const f of files.sort()) {
  const text = readFileSync(f, 'utf8');
  if (/\.ya?ml$/.test(f)) { blocks.push({ f, what: 'file', text }); continue; }
  for (const doc of text.split(/\n---[ \t]*\n/)) {
    const lines = doc.split('\n');
    let open = 0;
    while (open < lines.length && lines[open].trim() === '') open++;
    if (lines[open]?.trimEnd() !== '+++') continue;
    let close = open + 1;
    while (close < lines.length && lines[close].trimEnd() !== '+++') close++;
    if (close < lines.length) blocks.push({ f, what: 'front matter', text: lines.slice(open + 1, close).join('\n') });
  }
  const re = /^```data[ \t]*$\n([\s\S]*?)^```[ \t]*$/gm;
  let m;
  while ((m = re.exec(text))) blocks.push({ f, what: 'data fence', text: m[1] });
}

const binary = join(here, '..', 'build', 'yamlcat');

let same = 0;
let total = 0;
let bytes = 0;
let shown = 0;
const kinds = {};

for (const b of blocks) {
  total += 1;
  bytes += b.text.length;

  let expected;
  try {
    expected = JSON.stringify(parse(b.text) ?? null);
  } catch (error) {
    kinds['the reference refused it'] = (kinds['the reference refused it'] ?? 0) + 1;
    continue;
  }

  let got;
  try {
    got = execFileSync(binary, [], { input: b.text, encoding: 'utf8', maxBuffer: 1 << 28 }).trim();
  } catch (error) {
    got = `ERROR: ${String(error.stderr ?? error.message).trim()}`;
  }

  if (got === expected) { same += 1; continue; }

  const kind = got.startsWith('ERROR:') ? got.slice(0, 60) : 'a different value';
  kinds[kind] = (kinds[kind] ?? 0) + 1;

  if (has('first') && shown < 3) {
    shown += 1;
    const n = Math.min(got.length, expected.length);
    let i = 0;
    while (i < n && got[i] === expected[i]) i++;
    console.log(`\n${b.f} (${b.what}), first difference at byte ${i}:`);
    console.log(`   C: …${got.slice(Math.max(0, i - 60), i + 100)}`);
    console.log(`  JS: …${expected.slice(Math.max(0, i - 60), i + 100)}`);
  }
}

const pct = ((same / total) * 100).toFixed(1);
console.log(
  `\n${same}/${total} YAML blocks read identically (${pct}%) — ${(bytes / 1024).toFixed(0)} KB`
);
for (const [k, v] of Object.entries(kinds).sort((a, b) => b[1] - a[1])) {
  console.log(`  ${String(v).padStart(5)}  ${k}`);
}
process.exit(same === total ? 0 : 1);
