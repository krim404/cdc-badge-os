// Post-build internal link checker.
//
// Scans the built site for internal links and fails if any points at a page that
// was not generated. Sibling directories that the CI assembles next to the docs
// (the web flasher at /flasher/ and the Doxygen reference at /api/) are allow-listed
// because they are not part of the Astro build. Run after `astro build`:
//
//   SITE_BASE=/cdc-badge-os/ node scripts/check-links.mjs dist
//
// Exits non-zero if broken internal links are found.

import { readdir, readFile, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { join, relative } from 'node:path';

const distDir = process.argv[2] ?? 'dist';
const SITE_BASE = process.env.SITE_BASE ?? '/cdc-badge-os/';
const basePrefix = SITE_BASE.replace(/\/$/, ''); // '' when SITE_BASE is '/'

// Path prefixes that are deployed beside the docs but not built by Astro.
const EXTERNAL_SIBLINGS = ['/flasher/', '/api/'];

async function walk(dir) {
  const out = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...(await walk(full)));
    else out.push(full);
  }
  return out;
}

function extractHrefs(html) {
  const hrefs = [];
  const re = /(?:href|src)="([^"]*)"/g;
  let m;
  while ((m = re.exec(html)) !== null) hrefs.push(m[1]);
  return hrefs;
}

// Map an internal href to the dist file it should resolve to, or null if it is
// external / an anchor / an allow-listed sibling.
function resolveTarget(href) {
  if (!href) return null;
  if (/^[a-z]+:/i.test(href)) return null; // http:, https:, mailto:, etc.
  if (href.startsWith('//')) return null; // protocol-relative
  if (href.startsWith('#')) return null; // same-page anchor
  let path = href.split('#')[0].split('?')[0];
  if (!path.startsWith('/')) return null; // relative asset, skip
  // strip deployment base
  let rel = path;
  if (basePrefix && (rel === basePrefix || rel.startsWith(basePrefix + '/'))) {
    rel = rel.slice(basePrefix.length);
  }
  if (rel === '') rel = '/';
  for (const sib of EXTERNAL_SIBLINGS) {
    if (rel === sib || rel.startsWith(sib)) return { allow: true };
  }
  return { rel };
}

function targetExists(rel) {
  // Directory route (trailingSlash: always) -> index.html
  if (rel.endsWith('/')) return existsSync(join(distDir, rel, 'index.html'));
  // Explicit file (asset) or a route without slash
  if (existsSync(join(distDir, rel))) return true;
  return existsSync(join(distDir, rel, 'index.html'));
}

const files = (await walk(distDir)).filter((f) => f.endsWith('.html'));
const broken = [];
let checked = 0;

for (const file of files) {
  const html = await readFile(file, 'utf8');
  for (const href of extractHrefs(html)) {
    const t = resolveTarget(href);
    if (!t || t.allow) continue;
    checked++;
    if (!targetExists(t.rel)) {
      broken.push({ file: relative(distDir, file), href });
    }
  }
}

if (broken.length) {
  console.error(`\nBroken internal links: ${broken.length}`);
  for (const b of broken) console.error(`  ${b.file}  ->  ${b.href}`);
  process.exit(1);
}
console.log(`Link check OK: ${checked} internal links across ${files.length} pages, no broken targets.`);
