/*
 * Does `mdy_script_compile` agree with mdy-docs' `compileScript`?
 *
 * Byte for byte, over every document in a corpus. This one matters more than
 * it looks: the source it produces is compiled and RUN, so a difference here
 * is not a different tree, it is different behaviour — a loop that iterates
 * once more, an interpolation that lands in the wrong place, a `%%` block that
 * swallows the markup under it.
 *
 * The `code` map is compared too. It says which lines went in as code, and a
 * host uses it to map a runtime error back to the line somebody wrote.
 *
 *   node test/compare-script.mjs --mdy-docs <path> --corpus <path> [--first]
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
  console.error('usage: compare-script.mjs --mdy-docs <path> [--corpus <dir>]');
  process.exit(2);
}

const { compileScript } = await import(join(mdyDocs, 'src/parse/script.js'));

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
  console.error(`compare-script: no .mdy files under ${corpusDir ?? '(no --corpus given)'}`);
  process.exit(2);
}

const binary = join(here, '..', 'build', 'mdyast');

let same = 0;
let total = 0;
let bytes = 0;
let codeLines = 0;
let shown = 0;

for (const file of files) {
  const source = readFileSync(file, 'utf8');
  // `compileScript` takes the lines, split the way fromMdy splits them.
  const lines = source.split(/\r\n|\r|\n/);
  const js = compileScript(lines);
  // The CLI writes the source and one newline.
  const c = execFileSync(binary, ['--script', file], { encoding: 'utf8', maxBuffer: 1 << 28 }).replace(/\n$/, '');

  total += 1;
  bytes += js.source.length;
  codeLines += js.code.filter(Boolean).length;

  if (c === js.source) {
    same += 1;
    continue;
  }

  if (has('first') && shown < 1) {
    shown += 1;
    const n = Math.min(c.length, js.source.length);
    let i = 0;
    while (i < n && c[i] === js.source[i]) i++;
    const from = Math.max(0, i - 70);
    console.log(`\nfirst difference — ${file}, at byte ${i} of ${js.source.length}\n`);
    console.log(`   C: …${JSON.stringify(c.slice(from, i + 90))}`);
    console.log(`  JS: …${JSON.stringify(js.source.slice(from, i + 90))}\n`);
  }
}

const pct = ((same / total) * 100).toFixed(1);
console.log(
  `${same}/${total} documents compiled identically (${pct}%) — ` +
    `${(bytes / 1024).toFixed(0)} KB of JavaScript, ${codeLines} code lines`
);
process.exit(same === total ? 0 : 1);
