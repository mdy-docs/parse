/*
 * Does the C agree with linkify-it?
 *
 * The same discipline as compare.mjs and for the same reason: the URL rules
 * are not rules anyone guesses, so agreement has to be measured. This feeds
 * both implementations the same strings — every line of the corpus that could
 * hold a link, plus a set of edge cases chosen to hit the conditional
 * alternatives in the path grammar — and reports where they part.
 *
 *   node test/linkify.mjs --mdy-docs <path> [--corpus <dir>] [--show N]
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(`--${n}`); return i === -1 ? d : argv[i + 1]; };

const mdyDocs = flag('mdy-docs');
if (!mdyDocs) { console.error('usage: linkify.mjs --mdy-docs <path> [--corpus <dir>]'); process.exit(2); }
const { createRequire } = await import('node:module');
const { pathToFileURL } = await import('node:url');
const require = createRequire(pathToFileURL(join(mdyDocs, 'package.json')));
const { LinkifyIt } = await import(pathToFileURL(require.resolve('linkify-it')));
const linkify = new LinkifyIt();

/* Edge cases, each one aimed at a conditional alternative in the path grammar
 * or at a schema rule — the places a hand-written boundary goes wrong. */
const CASES = [
  'see http://example.com.', 'a http://x.com/a,b c', 'http://x.com/a, b',
  'http://x.com/a;b', 'http://x.com/a; b', 'http://x.com/p!', 'http://x.com/p!!',
  'http://x.com/p!x', 'http://x.com/q?', 'http://x.com/q??', 'http://x.com/q?a=1',
  'http://x.com/(a)', 'http://x.com/(a', 'http://x.com/[a]b', 'http://x.com/{a}',
  'http://x.com/"q"', "http://x.com/'q'", 'http://x.com/a..b', 'http://x.com/a...',
  'http://x.com/a--b', 'http://x.com/a-', 'x //example.com/p y', '//example.com',
  '//localhost/x', '//foo', '//a.b', '//a.b-c', '//E.J. Brill', '//wꜣs.t//',
  '//UD.UNUG^^KI^^//, y', '//j3ḥ-ḏḥw.ty)//, z', 'http://x.com:8080/p',
  'http://x.com:99999/p', 'HTTP://X.COM/P', '_http://x.com', '<http://x.com>',
  'ftp://files.example.org/pub', 'a@b.com', 'mailto:a@b.com',
  'http://x.com/p#frag', 'http://x.com/p?a=1&b=2', 'text http://x.com more text',
  'http://xn--80ak6aa92e.com/p', 'http://x.com/%20a', 'http://x.com/a_b~c*d',
  '(see http://x.com/p)', 'http://x.com/a.b.c', 'http://.com', 'http://x./p',
];

function candidates() {
  const out = [...CASES];
  const corpus = flag('corpus');
  if (corpus && statSync(corpus, { throwIfNoEntry: false })?.isDirectory()) {
    const walk = (d) => {
      for (const e of readdirSync(d, { withFileTypes: true })) {
        const p = join(d, e.name);
        if (e.isDirectory()) walk(p);
        else if (e.name.endsWith('.mdy')) {
          for (const line of readFileSync(p, 'utf8').split('\n')) {
            if (/https?:|ftp:|mailto:|\/\//.test(line)) out.push(line);
          }
        }
      }
    };
    walk(corpus);
  }
  return out;
}

const inputs = candidates();
const binary = join(here, '..', 'build', 'linkify');

let same = 0;
const shown = [];
const limit = Number(flag('show', 6));

for (const input of inputs) {
  const want = (linkify.match(input) ?? []).map((m) => `${m.index} ${m.lastIndex} ${m.raw}`);
  const got = execFileSync(binary, { input, encoding: 'utf8' })
    .split('\n').filter(Boolean)
    // The C reports BYTE offsets; linkify reports UTF-16 ones. Compare the
    // matched text, which is what actually lands in the tree.
    .map((l) => l.slice(l.indexOf(' ', l.indexOf(' ') + 1) + 1));
  const wantText = want.map((l) => l.slice(l.indexOf(' ', l.indexOf(' ') + 1) + 1));

  if (JSON.stringify(got) === JSON.stringify(wantText)) { same++; continue; }
  if (shown.length < limit) shown.push({ input, got, want: wantText });
}

console.log(`${same}/${inputs.length} inputs agree (${(same / inputs.length * 100).toFixed(1)}%)`);
for (const s of shown) {
  console.log(`\n  input: ${JSON.stringify(s.input.slice(0, 100))}`);
  console.log(`  C   : ${JSON.stringify(s.got)}`);
  console.log(`  JS  : ${JSON.stringify(s.want)}`);
}
