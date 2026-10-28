// Deliberate static defects, always made in a private copy. The real gate is
// never weakened: restoring a defect may return to a still-failing baseline
// (for example, supplied untranslated text), which is reported explicitly.
import assert from 'node:assert/strict';
import { cp, mkdtemp, readFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { auditSite } from './test_site.mjs';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const output = resolve(process.argv[2] || join(repository, 'build/web'));
const sourceConfig = resolve(process.argv[3] || join(repository, 'site/pages.toml'));
const temporary = await mkdtemp(join(tmpdir(), 'vc-site-mutations-'));
const publication = join(temporary, 'web'), config = join(temporary, 'pages.toml');

try {
  await cp(output, publication, { recursive: true });
  await cp(sourceConfig, config);
  const baseline = await auditSite(publication, config);
  console.log(`site mutations baseline: ${baseline.errors.length ? `RED (${baseline.errors.length} existing failures)` : 'GREEN'}`);
  for (const error of baseline.errors) console.log(`  ${error.page} [${error.field}]: ${error.message}`);

  async function mutation(name, mutate, restore, matches) {
    try {
      await mutate();
      const red = await auditSite(publication, config);
      const added = red.errors.filter(error => !baseline.errors.some(previous => JSON.stringify(previous) === JSON.stringify(error)));
      assert.ok(added.some(matches), `${name}: deliberate defect was not detected\n${JSON.stringify(red.errors, null, 2)}`);
      const caught = added.find(matches);
      console.log(`RED ${name}: ${caught.page} [${caught.field}]: ${caught.message}`);
    } finally {
      await restore();
    }
    const restored = await auditSite(publication, config);
    assert.deepEqual(restored.errors, baseline.errors, `${name}: restoration did not return to the exact original baseline`);
    console.log(`${baseline.errors.length ? 'RESTORED' : 'GREEN'} ${name}: defect gone${baseline.errors.length ? `; baseline remains RED (${baseline.errors.length} unrelated failures)` : ''}`);
  }

  const text = await readFile(config, 'utf8');
  const withoutRussianTitle = text.replace(/(\[program\.ru\]\r?\n)title\s*=\s*[^\n]*\r?\n/, '$1');
  assert.notEqual(withoutRussianTitle, text, 'fixture must contain a Russian program title');
  await mutation('missing Russian title', () => writeFile(config, withoutRussianTitle), () => writeFile(config, text),
    error => error.field === 'ru.title' && error.message.includes('non-empty'));

  const orphan = join(publication, 'deliberately-unowned.000000000000.wasm');
  await mutation('unowned side-module', () => writeFile(orphan, Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])),
    () => rm(orphan), error => error.field === 'modules' && error.message.includes('deliberately-unowned'));

  const pageFile = join(publication, 'rogue/index.html');
  const page = await readFile(pageFile, 'utf8');
  const broken = page.replace(/(<a\b[^>]*\bhref=")[^"]*(")/, '$1/__site_gate_missing__/$2');
  assert.notEqual(broken, page, 'fixture must contain an internal link');
  await mutation('broken internal link', () => writeFile(pageFile, broken), () => writeFile(pageFile, page),
    error => error.page === '/rogue/' && error.field === 'a.href' && error.message.includes('__site_gate_missing__'));

  const sitemapFile = join(publication, 'sitemap.xml');
  const sitemap = await readFile(sitemapFile, 'utf8');
  const missingPage = sitemap.replace(/<url\b[^>]*>[\s\S]*?<\/url>/g,
    item => /<loc>https:\/\/[^<]+\/rogue\/<\/loc>/.test(item) && !/<loc>[^<]+\/ru\/rogue\/<\/loc>/.test(item) ? '' : item);
  assert.notEqual(missingPage, sitemap, 'fixture must contain the English Rogue sitemap loc');
  await mutation('page missing from sitemap', () => writeFile(sitemapFile, missingPage), () => writeFile(sitemapFile, sitemap),
    error => error.page === '/rogue/' && error.field === 'sitemap');

  const handler = page.replace('<body>', '<body onload="void 0">');
  assert.notEqual(handler, page, 'fixture must have a body for the inline-handler regression');
  await mutation('inline JavaScript handler', () => writeFile(pageFile, handler), () => writeFile(pageFile, page),
    error => error.page === '/rogue/' && error.field === 'body.onload');

  const noContext = page.replace('"@context":"https://schema.org",', '');
  assert.notEqual(noContext, page, 'fixture must have a schema.org JSON-LD context');
  await mutation('missing JSON-LD context', () => writeFile(pageFile, noContext), () => writeFile(pageFile, page),
    error => error.page === '/rogue/' && error.field.includes('JSON-LD') && error.field.endsWith('@context'));

  console.log('PASS static mutation sensitivity: four required defects and two metadata/script regressions caught and restored; original sources/publication unchanged');
  if (baseline.errors.length) console.log('The production site gate is still RED; mutation-test success does not waive the baseline failures.');
} finally {
  await rm(temporary, { recursive: true, force: true });
}
