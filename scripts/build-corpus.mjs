/*
 * A markdown corpus, borrowed.
 *
 * The C stages in this repo are trustworthy because each is diffed against
 * mdy-docs' JavaScript over real input — 179 YAML blocks, 145 documents of
 * script, 87 trees, 290 HTML outputs, 10,514 URLs. A markdown front end has no
 * such corpus here: this project contains zero `.md` files.
 *
 * So one is borrowed, from three kinds of source, because they fail in
 * different ways:
 *
 *   SPEC EXAMPLES say what the language IS. The CommonMark spec's 652 and the
 *   GFM spec's 672 are the cases their authors thought worth pinning, which
 *   means they are dense in exactly the corners a hand-written parser gets
 *   wrong — lazy continuation, link reference definitions, HTML blocks, tight
 *   and loose lists.
 *
 *   EXTENSION SPECS say what md4c does beyond CommonMark, which is where it
 *   and remark-gfm may simply disagree. Knowing that is the point.
 *
 *   REAL DOCUMENTS say what people actually write, which is neither. A README
 *   is long, mundane, and full of things no spec example bothers with: badges,
 *   nested lists ten deep, tables with ragged rows, raw HTML, emoji.
 *
 * Nothing is committed. The corpus is assembled into build/corpus/, which is
 * build output; the spec files come from md4c's own vendored copies (CC-BY-SA
 * 4.0, licensed in third_party/md4c/test/LICENSE.md) and the GFM spec is
 * fetched once and cached.
 *
 *   node scripts/build-corpus.mjs [--roots <dir>[,<dir>…]] [--offline]
 */
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync, existsSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..');
const out = join(repo, 'build', 'corpus');
const cache = join(repo, 'build', 'spec-cache');

const argv = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i === -1 ? fallback : argv[i + 1];
};
const offline = argv.includes('--offline');

const GFM_SPEC_URL = 'https://raw.githubusercontent.com/github/cmark-gfm/master/test/spec.txt';

/*
 * A spec file's examples. The fence is 32 backticks and the two halves are
 * separated by a `.` on its own line; `→` stands for a tab, which is the one
 * substitution the spec's own test runner makes too.
 */
function specExamples(text) {
  const examples = [];
  const re = /^`{32} example.*?\n([\s\S]*?)^\.\n([\s\S]*?)^`{32}$/gm;
  let m;
  while ((m = re.exec(text))) {
    examples.push({ markdown: m[1].replace(/→/g, '\t'), html: m[2].replace(/→/g, '\t') });
  }
  return examples;
}

const manifest = [];
const seen = new Set();
let written = 0;

function add(group, name, markdown, provenance) {
  const hash = createHash('sha1').update(markdown).digest('hex');
  if (seen.has(hash)) return false;          /* the specs overlap heavily */
  seen.add(hash);
  const file = `${group}/${name}.md`;
  mkdirSync(join(out, group), { recursive: true });
  writeFileSync(join(out, file), markdown);
  manifest.push({ file, bytes: markdown.length, from: provenance });
  written += 1;
  return true;
}

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
mkdirSync(cache, { recursive: true });

/* ---- 1. the CommonMark spec, from md4c's vendored copy -------------------- */

const md4cTest = join(repo, 'third_party', 'md4c', 'test');
if (!existsSync(join(md4cTest, 'spec.txt'))) {
  console.error('build-corpus: third_party/md4c is not checked out (git submodule update --init)');
  process.exit(2);
}

const commonmark = readFileSync(join(md4cTest, 'spec.txt'), 'utf8');
const version = /^version: '([^']+)'/m.exec(commonmark)?.[1] ?? '?';
let n = 0;
for (const [i, ex] of specExamples(commonmark).entries()) {
  if (add('commonmark', String(i).padStart(4, '0'), ex.markdown, `CommonMark spec ${version}, example ${i + 1}`)) n += 1;
}
console.log(`  commonmark   ${String(n).padStart(4)}  spec ${version} (CC-BY-SA 4.0, via third_party/md4c)`);

/* ---- 2. md4c's own extension specs ---------------------------------------- */

const extensions = readdirSync(md4cTest).filter((f) => /^spec-.+\.txt$/.test(f)).sort();
for (const file of extensions) {
  const group = file.replace(/^spec-|\.txt$/g, '');
  const text = readFileSync(join(md4cTest, file), 'utf8');
  let count = 0;
  for (const [i, ex] of specExamples(text).entries()) {
    if (add(`ext-${group}`, String(i).padStart(4, '0'), ex.markdown, `md4c ${file}, example ${i + 1}`)) count += 1;
  }
  if (count) console.log(`  ext-${group.padEnd(9)} ${String(count).padStart(4)}  ${file}`);
}

/* ---- 3. the GFM spec, fetched once ---------------------------------------- */

const gfmPath = join(cache, 'gfm-spec.txt');
if (!existsSync(gfmPath) && !offline) {
  try {
    execFileSync('curl', ['-sSL', '--max-time', '60', '-o', gfmPath, GFM_SPEC_URL], { stdio: 'pipe' });
  } catch {
    console.log('  gfm          ---  could not fetch; run again with a network, or --offline to skip');
  }
}
if (existsSync(gfmPath)) {
  const gfm = readFileSync(gfmPath, 'utf8');
  const gfmVersion = /^version: ([\d.]+)/m.exec(gfm)?.[1] ?? '?';
  let count = 0;
  for (const [i, ex] of specExamples(gfm).entries()) {
    if (add('gfm', String(i).padStart(4, '0'), ex.markdown, `GFM spec ${gfmVersion}, example ${i + 1}`)) count += 1;
  }
  console.log(`  gfm          ${String(count).padStart(4)}  spec ${gfmVersion} (CC-BY-SA 4.0, ${GFM_SPEC_URL})`);
}

/* ---- 4. real documents, from whatever is on this machine ------------------ */

const roots = (flag('roots') ?? join(repo, '..')).split(',').filter(Boolean);
const SKIP = new Set(['.git', 'build', 'dist', 'target', 'corpus']);
const real = [];
const walk = (d, depth = 0) => {
  if (depth > 12) return;
  let entries;
  try { entries = readdirSync(d, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = join(d, e.name);
    if (e.isDirectory()) { if (!SKIP.has(e.name) && !e.name.endsWith('.dSYM')) walk(p, depth + 1); }
    else if (e.name.endsWith('.md')) real.push(p);
  }
};
for (const r of roots) walk(r);

let realCount = 0;
let realBytes = 0;
for (const p of real.sort()) {
  let text;
  try { text = readFileSync(p, 'utf8'); } catch { continue; }
  if (!text.trim()) continue;
  /* Long enough to be prose rather than a stub, and not so long that one file
   * dominates the report. */
  if (text.length < 200 || text.length > 400_000) continue;
  const name = createHash('sha1').update(p).digest('hex').slice(0, 12);
  if (add('real', name, text, relative(join(repo, '..'), p))) { realCount += 1; realBytes += text.length; }
}
console.log(`  real         ${String(realCount).padStart(4)}  documents found on this machine, ${(realBytes / 1024).toFixed(0)} KB`);

writeFileSync(join(out, 'manifest.json'), `${JSON.stringify(manifest, null, 1)}\n`);
const bytes = manifest.reduce((a, m) => a + m.bytes, 0);
console.log(`\n${written} documents, ${(bytes / 1024).toFixed(0)} KB → build/corpus/`);
console.log('provenance in build/corpus/manifest.json');
