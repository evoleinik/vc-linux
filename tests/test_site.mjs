// Static publication gate. No browser, loopback socket or npm parser is needed.
// Usage: node tests/test_site.mjs [build/web [site/pages.toml]]
import assert from 'node:assert/strict';
import { readFile, stat, realpath, mkdtemp, open, rm } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const homeTitle = 'Volkov Commander online: DOS programs, not an emulator';
const homeDescription = 'Run Volkov Commander, GW-BASIC, Rogue, Hack, MS-DOS 2.0, VZ Editor, Kermit and bootLogo in your browser, translated from 8086 code to WebAssembly. No emulator.';
const operatingSystem = 'MS-DOS, run in a web browser as WebAssembly';
const contentFields = ['title', 'description', 'h1', 'body'];
const normalize = value => String(value ?? '').replace(/\s+/gu, ' ').trim();
const words = value => String(value ?? '').toLowerCase().split(/\s+/).filter(Boolean);
const same = (left, right) => JSON.stringify(left) === JSON.stringify(right);
const asList = value => Array.isArray(value) ? value : value ? [value] : [];
const pagePath = file => file === 'index.html' ? '/' : `/${file.replace(/\/index\.html$/, '/')}`;
const itemURL = item => typeof item === 'string' ? item : item?.['@id'] ?? item?.url;
const schemaTypes = item => asList(item?.['@type']);
const resolveURL = (value, base) => {
  try { return new URL(value, base); }
  catch { return null; }
};

async function inspect(output, config) {
  // A few managed environments refuse Node's pipe capture. Real temporary
  // descriptors keep Python diagnostics observable there, without sockets.
  const temporary = await mkdtemp(join(tmpdir(), 'vc-site-parser-'));
  let stdout, stderr;
  try {
    stdout = await open(join(temporary, 'stdout'), 'wx');
    stderr = await open(join(temporary, 'stderr'), 'wx');
    const result = spawnSync(join(repository, '.venv/bin/python'),
      [join(repository, 'tests/site_inspect.py'), output, config],
      { cwd: repository, timeout: 30000, stdio: ['ignore', stdout.fd, stderr.fd] });
    assert.ifError(result.error);
    const diagnostics = await readFile(join(temporary, 'stderr'), 'utf8');
    assert.equal(result.status, 0, `site syntax reader: ${diagnostics}`);
    return JSON.parse(await readFile(join(temporary, 'stdout'), 'utf8'));
  } finally {
    await stdout?.close();
    await stderr?.close();
    await rm(temporary, { recursive: true, force: true });
  }
}

function cssReferences(text) {
  return [...String(text).matchAll(/url\(\s*(?:"([^"]*)"|'([^']*)'|([^)'"\s]*))\s*\)/gi)]
    .map(match => match[1] ?? match[2] ?? match[3]);
}

function graphItems(value) {
  if (Array.isArray(value)) return value.flatMap(graphItems);
  if (!value || typeof value !== 'object') return [];
  return [...(value['@type'] ? [value] : []), ...asList(value['@graph']).flatMap(graphItems)];
}

export async function auditSite(outputPath = join(repository, 'build/web'), configPath = join(repository, 'site/pages.toml')) {
  const output = resolve(outputPath), configFile = resolve(configPath);
  let inspected;
  try {
    inspected = await inspect(output, configFile);
  } catch (error) {
    return { errors: [{ page: configFile, field: 'parser', message: error.message }], pages: 0, programs: 0, modules: 0 };
  }
  const errors = [...inspected.errors];
  const fail = (page, field, message) => errors.push({ page, field, message });
  const check = (condition, page, field, message) => { if (!condition) fail(page, field, message); };
  const config = inspected.config;
  const programs = Array.isArray(config.program) ? config.program : [];
  check(programs.length > 0, configFile, 'program', 'at least one [[program]] is required');
  let site;
  try {
    site = new URL(config.site);
    check(site.protocol === 'https:' && site.pathname === '/' && !site.search && !site.hash,
      configFile, 'site', 'must be an HTTPS origin without a path, query or fragment');
  } catch {
    fail(configFile, 'site', 'must be an absolute HTTPS site URL');
    site = new URL('https://notanemulator.com');
  }
  const siteURL = path => new URL(path, site).href;
  const absoluteSite = siteURL('/');
  const imageURL = siteURL('/social-preview.png');
  const licenseURL = program => `${config.repo}/blob/main/${program.license_path}`;
  const descriptors = new Map([['/', { kind: 'home', language: 'en', title: homeTitle, description: homeDescription }]]);

  function verifyText(item, paths) {
    for (const language of ['en', 'ru']) {
      const table = item?.[language];
      check(table && typeof table === 'object' && !Array.isArray(table), paths[language], language, 'language table is required');
      for (const field of contentFields) {
        const value = table?.[field];
        check(typeof value === 'string' && value.trim().length > 0, paths[language], `${language}.${field}`, 'a non-empty string is required');
        if (typeof value !== 'string' || !value.trim()) continue;
        const length = [...value].length;
        if (field === 'title') check(length <= 60, paths[language], `${language}.${field}`, `${length} characters exceeds 60`);
        if (field === 'description') check(length >= 70 && length <= 160, paths[language], `${language}.${field}`, `${length} characters is outside 70–160`);
        // A program's h1 is its name, which stays in Latin script in Russian too.
        if (language === 'ru' && field !== 'h1') check(value !== item?.en?.[field], paths.ru, `ru.${field}`, `must differ from en.${field}; supplied prose needs its author/translator`);
      }
    }
  }

  verifyText(config.hub, { en: '/programs/', ru: '/ru/' });
  for (const language of ['en', 'ru']) {
    const path = language === 'en' ? '/programs/' : '/ru/';
    check(config.hub?.[language]?.path === path, path, `${language}.path`, `must be ${path}`);
    descriptors.set(path, { kind: 'hub', language, ...config.hub?.[language] });
  }
  const seenSlugs = new Set();
  for (const [index, program] of programs.entries()) {
    const slug = program.slug;
    const path = typeof slug === 'string' ? `/${slug}/` : `program[${index}]`;
    check(typeof slug === 'string' && /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(slug), path, 'slug', 'must be a lower-case URL slug');
    check(!seenSlugs.has(slug), path, 'slug', 'duplicate program slug');
    seenSlugs.add(slug);
    const paths = { en: `/${slug}/`, ru: `/ru/${slug}/` };
    verifyText(program, paths);
    for (const language of ['en', 'ru'])
      descriptors.set(paths[language], { kind: 'program', language, program, ...program[language] });
    check(['SoftwareApplication', 'VideoGame'].includes(program.schema_type), path, 'schema_type', 'must be SoftwareApplication or VideoGame');
    const relativeLicense = typeof program.license_path === 'string' ? program.license_path : '';
    const licenseFile = resolve(repository, relativeLicense);
    const relativeFile = relative(repository, licenseFile);
    check(relativeLicense && relativeFile && relativeFile !== '..' && !relativeFile.startsWith(`..${sep}`) &&
      await stat(licenseFile).then(info => info.isFile(), () => false), path, 'license_path', `repository file is missing or outside the repository: ${relativeLicense}`);
    for (const field of ['source_url']) {
      try { check(new URL(program[field]).protocol === 'https:', path, field, 'external links must use HTTPS'); }
      catch { fail(path, field, 'must be an absolute HTTPS URL'); }
    }
  }
  for (const field of ['repo', 'doi']) {
    try { check(new URL(config[field]).protocol === 'https:', configFile, field, 'external links must use HTTPS'); }
    catch { fail(configFile, field, 'must be an absolute HTTPS URL'); }
  }

  // Every deployed wasm other than the main is a lazy-module artifact, even
  // when its basename is new and unknown to the generator or this test.
  const moduleFiles = inspected.files.filter(file => file.endsWith('.wasm') && file !== 'vc.wasm');
  const moduleNames = new Set(moduleFiles.map(file => basename(file).replace(/(?:\.[0-9a-f]{12})?\.wasm$/, '')));
  const owners = new Map();
  for (const program of programs) {
    const path = `/${program.slug}/`;
    check(Array.isArray(program.modules) && program.modules.length > 0, path, 'modules', 'a non-empty list of owned lazy-module basenames is required');
    for (const module of Array.isArray(program.modules) ? program.modules : []) {
      check(typeof module === 'string' && /^[a-z0-9_-]+$/.test(module), path, 'modules', `invalid lazy-module basename: ${JSON.stringify(module)}`);
      owners.set(module, [...(owners.get(module) || []), path]);
      check(moduleNames.has(module), path, 'modules', `owns ${module}, but the web build did not publish it`);
    }
  }
  for (const file of moduleFiles) {
    const module = basename(file).replace(/(?:\.[0-9a-f]{12})?\.wasm$/, '');
    const pages = owners.get(module) || [];
    check(pages.length === 1, `/${file}`, 'modules', `${module} must be owned by exactly one program page; found ${pages.length}${pages.length ? ` (${pages.join(', ')})` : ''}`);
  }

  const expectedRuns = Object.create(null);
  for (const program of programs) {
    expectedRuns[program.slug] = program.run;
    if (program.also_run) expectedRuns[program.also_run.slug || `${program.slug}-405`] = program.also_run.run;
  }
  let runs = {};
  try {
    const source = await readFile(join(output, 'vc-programs.js'), 'utf8');
    const exported = await import(`data:text/javascript,${encodeURIComponent(source)}`);
    runs = exported.programs;
    check(runs && typeof runs === 'object' && !Array.isArray(runs), '/vc-programs.js', 'programs', 'named export must be a program map');
    if (!runs || typeof runs !== 'object' || Array.isArray(runs)) runs = {};
    check(Object.isFrozen(runs), '/vc-programs.js', 'programs', 'the map must be frozen');
    check(same(Object.keys(runs).sort(), Object.keys(expectedRuns).sort()), '/vc-programs.js', 'programs', 'exported slugs must equal TOML program and also_run slugs');
    for (const [slug, lines] of Object.entries(expectedRuns)) {
      check(Array.isArray(lines) && lines.every(line => typeof line === 'string' && line.length > 0 && !/[\r\n\0]/.test(line)), `/${slug}/`, 'run', 'run must be an array of complete non-empty command lines (or [] for VC)');
      check(same(runs[slug], lines), '/vc-programs.js', `run.${slug}`, 'run lines differ from pages.toml');
      check(Array.isArray(runs[slug]) && Object.isFrozen(runs[slug]), '/vc-programs.js', `run.${slug}`, 'run lines must be a frozen array');
    }
  } catch (error) {
    fail('/vc-programs.js', 'programs', `cannot load the generated map: ${error.message}`);
  }

  const htmlPages = new Map(Object.entries(inspected.pages).map(([file, elements]) => [pagePath(file), { file, elements }]));
  const fileSet = new Set(inspected.files);
  const rootReal = await realpath(output).catch(() => output);
  const checkedFiles = new Map();
  async function reference(page, field, value, base, { embedded = false } = {}) {
    if (typeof value !== 'string' || !value.trim()) {
      fail(page, field, 'URL is missing or empty');
      return null;
    }
    if (embedded && /^(?:data|blob):/i.test(value)) return null;
    let url;
    try { url = new URL(value, base); }
    catch { fail(page, field, `invalid URL: ${value}`); return null; }
    if (url.origin !== site.origin) {
      check(url.protocol === 'https:', page, field, `external link must use HTTPS: ${value}`);
      return url;
    }
    check(url.protocol === 'https:', page, field, `same-site link must use HTTPS: ${value}`);
    let path;
    try { path = decodeURIComponent(url.pathname); }
    catch { fail(page, field, `invalid URL encoding: ${value}`); return url; }
    const file = `${path.replace(/^\//, '')}${path.endsWith('/') ? 'index.html' : ''}`;
    let exists = checkedFiles.get(file);
    if (exists === undefined) {
      const resolved = resolve(output, file), rel = relative(output, resolved);
      const actual = await realpath(resolved).catch(() => null);
      const actualRelative = actual ? relative(rootReal, actual) : '..';
      exists = rel !== '..' && !rel.startsWith(`..${sep}`) && actualRelative !== '..' && !actualRelative.startsWith(`..${sep}`) &&
        fileSet.has(file) && await stat(resolved).then(info => info.isFile(), () => false);
      checkedFiles.set(file, exists);
    }
    check(exists, page, field, `same-site target is not a published file: ${value} (${file})`);
    return url;
  }

  const sitemap = new Map();
  for (const row of inspected.sitemap) {
    check(row.locations.length === 1, '/sitemap.xml', 'loc', `each url needs one loc, found ${row.locations.length}`);
    const location = row.locations[0];
    if (!location) continue;
    check(!sitemap.has(location), '/sitemap.xml', 'loc', `duplicate URL: ${location}`);
    const parsed = await reference('/sitemap.xml', 'loc', location, absoluteSite);
    check(parsed?.origin === site.origin && parsed?.href === location, '/sitemap.xml', 'loc', `must be an absolute same-site URL: ${location}`);
    if (parsed) check(htmlPages.has(parsed.pathname), parsed.pathname, 'sitemap', 'loc does not identify a published page');
    sitemap.set(location, row);
  }
  for (const path of descriptors.keys()) {
    check(htmlPages.has(path), path, 'page', 'expected page is missing from the publication');
    check(sitemap.has(siteURL(path)), path, 'sitemap', 'page is missing from sitemap.xml');
  }
  for (const path of htmlPages.keys()) {
    check(sitemap.has(siteURL(path)), path, 'sitemap', 'published page is missing from sitemap.xml');
    check(descriptors.has(path), path, 'page', 'published HTML page is not defined by the home/hub/program contract');
  }

  const homeElements = htmlPages.get('/')?.elements || [];
  const homeCounter = homeElements.filter(element => element.tag === 'script' && element.attrs['data-goatcounter']);
  check(homeCounter.length === 1, '/', 'GoatCounter', `expected exactly one analytics tag, found ${homeCounter.length}`);
  const allTitles = new Map();
  const imageChecks = new Map();

  for (const [path, { elements }] of htmlPages) {
    const descriptor = descriptors.get(path);
    const canonical = siteURL(path);
    const all = tag => elements.filter(element => element.tag === tag);
    const one = (candidates, field) => {
      check(candidates.length === 1, path, field, `expected exactly one, found ${candidates.length}`);
      return candidates[0];
    };
    const meta = (attribute, name) => one(all('meta').filter(element => element.attrs[attribute]?.toLowerCase() === name), name)?.attrs.content;
    const links = relation => all('link').filter(element => words(element.attrs.rel).includes(relation));
    for (const element of elements) for (const attribute of element.duplicateAttributes)
      fail(path, `${element.tag}.${attribute}`, 'duplicate HTML attribute');
    const html = one(all('html'), 'html');
    if (descriptor) check(html?.attrs.lang === descriptor.language, path, 'html.lang', `must be ${descriptor.language}`);
    const title = normalize(one(all('title'), 'title')?.text);
    check(title.length > 0, path, 'title', 'title must not be empty');
    check(!allTitles.has(title), path, 'title', `duplicate title also used by ${allTitles.get(title)}`);
    allTitles.set(title, path);
    const description = meta('name', 'description');
    check(typeof description === 'string' && description.trim().length > 0, path, 'description', 'description must not be empty');
    if (descriptor?.title) check(title === descriptor.title, path, 'title', 'does not match the approved page text');
    if (descriptor?.description) check(description === descriptor.description, path, 'description', 'does not match the approved page text');
    check(one(links('canonical'), 'canonical')?.attrs.href === canonical, path, 'canonical', `must be ${canonical}`);
    const bases = all('base');
    check(bases.length <= 1, path, 'base', 'at most one base URL is allowed');
    let base = canonical;
    if (bases.length) {
      const value = await reference(path, 'base.href', bases[0].attrs.href, canonical);
      if (value) base = value.href;
    }
    const ogTitle = meta('property', 'og:title'), ogDescription = meta('property', 'og:description');
    check(ogTitle === title, path, 'og:title', 'must equal this page title');
    check(ogDescription === description, path, 'og:description', 'must equal this page description');
    check(meta('property', 'og:url') === canonical, path, 'og:url', `must be ${canonical}`);
    check(Boolean(meta('property', 'og:type')), path, 'og:type', 'type is required');
    const ogImage = meta('property', 'og:image');
    check(ogImage === imageURL, path, 'og:image', `must be ${imageURL}`);
    await reference(path, 'og:image', ogImage, base);
    if (ogImage === imageURL && !imageChecks.has(ogImage)) {
      let dimensions = null;
      try {
        const bytes = await readFile(join(output, 'social-preview.png'));
        if (bytes.length >= 24 && bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])) &&
            bytes.toString('ascii', 12, 16) === 'IHDR') dimensions = [bytes.readUInt32BE(16), bytes.readUInt32BE(20)];
      } catch { /* The page/field diagnostic below covers missing and corrupt PNGs. */ }
      imageChecks.set(ogImage, dimensions);
    }
    check(same(imageChecks.get(ogImage), [1200, 630]), path, 'og:image', 'image must be a readable 1200x630 PNG');
    check(meta('name', 'twitter:card') === 'summary_large_image', path, 'twitter:card', 'must be summary_large_image');
    check(meta('name', 'twitter:title') === title, path, 'twitter:title', 'must equal this page title');
    check(meta('name', 'twitter:description') === description, path, 'twitter:description', 'must equal this page description');
    check(meta('name', 'twitter:image') === imageURL, path, 'twitter:image', `must be ${imageURL}`);

    if (descriptor && descriptor.kind !== 'home') {
      const enPath = descriptor.kind === 'hub' ? '/programs/' : `/${descriptor.program.slug}/`;
      const ruPath = descriptor.kind === 'hub' ? '/ru/' : `/ru/${descriptor.program.slug}/`;
      const expectedAlternates = { en: siteURL(enPath), ru: siteURL(ruPath), 'x-default': siteURL(enPath) };
      const alternates = links('alternate').filter(element => element.attrs.hreflang);
      check(alternates.length === 3, path, 'hreflang', `expected en, ru, x-default, found ${alternates.length} links`);
      for (const [language, href] of Object.entries(expectedAlternates)) {
        const link = one(alternates.filter(element => element.attrs.hreflang === language), `hreflang.${language}`);
        check(link?.attrs.href === href, path, `hreflang.${language}`, `must be ${href}`);
        const target = htmlPages.get(new URL(href).pathname);
        const reciprocal = target?.elements.filter(element => element.tag === 'link' && words(element.attrs.rel).includes('alternate') && element.attrs.hreflang === descriptor.language);
        check(reciprocal?.length === 1 && reciprocal[0].attrs.href === canonical, path, `hreflang.${language}`, 'target must link back to this page in its language');
      }
      const sitemapAlternates = sitemap.get(canonical)?.alternates || [];
      for (const [language, href] of Object.entries(expectedAlternates).filter(([language]) => language !== 'x-default')) {
        const alternate = sitemapAlternates.filter(link => link.hreflang === language);
        check(alternate.length === 1 && alternate[0].rel === 'alternate' && alternate[0].href === href,
          path, `sitemap.hreflang.${language}`, `must include one xhtml:link alternate to ${href}`);
      }
      const h1 = one(all('h1'), 'h1');
      check(h1 && !h1.hidden && h1.ancestors.includes('body'), path, 'h1', 'heading must be visible in the body');
      if (descriptor.h1) check(normalize(h1?.text) === descriptor.h1, path, 'h1', 'does not match the approved page text');
      for (const paragraph of String(descriptor.body || '').trim().split(/\n\s*\n/)) {
        const expected = normalize(paragraph.replace(/\[([^\]]+)\]\(([^)]+)\)/g, '$1'));
        check(all('p').some(element => !element.hidden && normalize(element.text) === expected), path, 'body', `approved paragraph is missing: ${expected.slice(0, 70)}`);
      }
      const otherLanguage = descriptor.language === 'en' ? ruPath : enPath;
      check(all('a').some(element => !element.hidden && resolveURL(element.attrs.href || '', base)?.href === siteURL(otherLanguage)), path, 'language link', `visible link to ${otherLanguage} is required`);
      const scripts = all('script').filter(element => element.attrs.type !== 'application/ld+json');
      check(scripts.length === 1 && scripts[0].startTag === homeCounter[0]?.startTag && scripts[0].text === homeCounter[0]?.text,
        path, 'GoatCounter', 'only the exact home-page GoatCounter script may run');
      for (const element of elements) {
        for (const attribute of Object.keys(element.attrs))
          check(!attribute.startsWith('on'), path, `${element.tag}.${attribute}`, 'inline JavaScript handlers are forbidden on static pages');
        check(!['iframe', 'object', 'embed'].includes(element.tag), path, element.tag, 'active embedded documents are forbidden on static pages');
      }
      check(links('stylesheet').length === 0, path, 'CSS', 'static pages must use inline CSS only');
      check(all('style').length > 0 && all('style').every(element => !/@import\b|@font-face\b/i.test(element.text)),
        path, 'CSS', 'inline styles and system fonts are required; no imported CSS or fonts');
    }

    const schemas = [];
    for (const [index, script] of all('script').filter(element => element.attrs.type === 'application/ld+json').entries()) {
      try {
        const value = JSON.parse(script.text);
        check(asList(value).length > 0 && asList(value).every(item => item?.['@context'] === 'https://schema.org'),
          path, `JSON-LD[${index}].@context`, 'must identify https://schema.org');
        schemas.push(...graphItems(value));
      }
      catch (error) { fail(path, `JSON-LD[${index}]`, `invalid JSON: ${error.message}`); }
    }
    const schema = type => {
      const candidates = schemas.filter(item => schemaTypes(item).includes(type));
      check(candidates.length === 1, path, `JSON-LD.${type}`, `expected exactly one object, found ${candidates.length}`);
      return candidates[0] || {};
    };
    if (descriptor?.kind === 'program') {
      const program = descriptor.program;
      const application = schema(program.schema_type);
      // A decade such as "1990s" names no creation year, so the page must claim none.
      const expected = { name: program.name, dateCreated: /^\d{4}(-\d{4})?$/.test(String(program.years)) ? String(program.years).slice(0, 4) : undefined,
        license: licenseURL(program), isBasedOn: program.source_url, applicationCategory: program.category,
        operatingSystem, isAccessibleForFree: true, inLanguage: descriptor.language, url: canonical };
      for (const [field, value] of Object.entries(expected))
        check(application[field] === value, path, `JSON-LD.${field}`, `expected ${JSON.stringify(value)}, got ${JSON.stringify(application[field])}`);
      const authors = asList(application.author).map(author => ({ type: author?.['@type'], name: author?.name }));
      const expectedAuthors = asList(program.authors).map(name => ({ type: ['Microsoft', 'Columbia University'].includes(name) ? 'Organization' : 'Person', name }));
      check(same(authors, expectedAuthors), path, 'JSON-LD.author', 'authors and Person/Organization types must match the program');
      const breadcrumb = schema('BreadcrumbList');
      const items = asList(breadcrumb.itemListElement);
      const expectedPaths = ['/', descriptor.language === 'en' ? '/programs/' : '/ru/', path];
      check(items.length === 3, path, 'JSON-LD.BreadcrumbList', 'expected home, language hub, program');
      for (const [index, expectedPath] of expectedPaths.entries()) {
        const item = items[index];
        check(item?.['@type'] === 'ListItem' && item.position === index + 1 && itemURL(item.item) === siteURL(expectedPath) && typeof item.name === 'string' && item.name.length > 0,
          path, `JSON-LD.BreadcrumbList[${index}]`, `expected named ListItem ${index + 1} for ${siteURL(expectedPath)}`);
      }
      const commands = all('code').filter(element => !element.hidden).map(element =>
        element.text.split(/\r?\n/).map(line => line.trim().replace(/^(?:\$|>)\s*/, '')).filter(Boolean));
      const expectedCommands = program.linux === 'vc' ? ['vc'] : ['vc', program.linux];
      check(commands.some(lines => same(lines, expectedCommands)) || same(commands.flat(), expectedCommands),
        path, 'linux', `must show ${expectedCommands.join(', then ')} commands`);
      for (const [field, url] of [['source_url', program.source_url], ['license_path', licenseURL(program)]])
        check(all('a').some(element => element.attrs.href === url && !element.hidden), path, field, `visible source/licence link is missing: ${url}`);
      for (const other of programs.filter(other => other.slug !== program.slug)) {
        const otherPath = `${descriptor.language === 'ru' ? '/ru' : ''}/${other.slug}/`;
        check(all('a').some(element => element.attrs.href && resolveURL(element.attrs.href, base)?.href === siteURL(otherPath) && !element.hidden), path, 'other programs', `visible link to ${otherPath} is missing`);
      }
    } else if (descriptor?.kind === 'hub') {
      const website = schema('WebSite');
      check(website.url === canonical && website.inLanguage === descriptor.language, path, 'JSON-LD.WebSite', 'url and inLanguage must describe this hub');
      const itemList = schema('ItemList');
      const items = asList(itemList.itemListElement);
      const expected = programs.map(program => siteURL(`${descriptor.language === 'ru' ? '/ru' : ''}/${program.slug}/`));
      check(items.length === expected.length, path, 'JSON-LD.ItemList', 'one item is required per program');
      for (const [index, url] of expected.entries())
        check(items[index]?.['@type'] === 'ListItem' && items[index].position === index + 1 && (itemURL(items[index].item) ?? items[index].url) === url,
          path, `JSON-LD.ItemList[${index}]`, `expected ListItem ${index + 1} for ${url}`);
    } else if (descriptor?.kind === 'home') {
      const website = schema('WebSite'), application = schema('SoftwareApplication');
      check(website.url === canonical, path, 'JSON-LD.WebSite.url', `must be ${canonical}`);
      check(String(application.name).includes('Volkov Commander') &&
        (application.softwareVersion === '4.99.09' || String(application.name).includes('4.99.09')),
        path, 'JSON-LD.SoftwareApplication', 'must identify Volkov Commander 4.99.09');
    }

    const anchors = all('a');
    const markedPlay = anchors.filter(element => words(element.attrs.class).includes('play') || Object.hasOwn(element.attrs, 'data-play'));
    if (descriptor && descriptor.kind !== 'home') check(markedPlay.some(element => !element.hidden), path, 'Play', 'at least one visible Play link is required');
    for (const element of anchors) {
      let target;
      try { target = new URL(element.attrs.href, base); } catch { continue; }
      if (!markedPlay.includes(element) && !target.searchParams.has('run')) continue;
      const slug = target.searchParams.get('run');
      check(target.origin === site.origin && target.pathname === '/' && !target.hash &&
        (target.search === '' || (Object.hasOwn(runs, slug) && target.search === `?run=${encodeURIComponent(slug)}`)),
        path, 'Play', `must target / or /?run=<published slug>: ${element.attrs.href}`);
    }
    if (descriptor?.kind === 'program') {
      const program = descriptor.program;
      const expected = [program.slug === 'volkov-commander' ? '/' : `/?run=${program.slug}`];
      if (program.also_run) expected.push(`/?run=${program.also_run.slug || `${program.slug}-405`}`);
      for (const target of expected)
        check(markedPlay.some(element => !element.hidden && element.attrs.href === target), path, 'Play', `missing Play target ${target}`);
    }

    for (const element of elements) {
      for (const attribute of ['href', 'src', 'poster', 'action', 'xlink:href', 'data-goatcounter']) {
        if (!Object.hasOwn(element.attrs, attribute)) continue;
        const embedded = attribute !== 'href' || element.tag !== 'a';
        await reference(path, `${element.tag}.${attribute}`, element.attrs[attribute], base, { embedded });
      }
      if (element.tag === 'object' && element.attrs.data) await reference(path, 'object.data', element.attrs.data, base, { embedded: true });
      if (element.attrs.srcset) {
        for (const candidate of element.attrs.srcset.split(',').map(value => value.trim().split(/\s+/)[0]).filter(Boolean))
          await reference(path, `${element.tag}.srcset`, candidate, base, { embedded: true });
      }
      for (const css of [element.attrs.style, element.tag === 'style' ? element.text : '']) {
        for (const url of cssReferences(css || '')) await reference(path, 'CSS.url', url, base, { embedded: true });
      }
    }
  }

  let llms = '';
  try { llms = await readFile(join(output, 'llms.txt'), 'utf8'); }
  catch (error) { fail('/llms.txt', 'file', error.message); }
  check(llms.startsWith('# notanemulator\n'), '/llms.txt', 'title', 'must begin with # notanemulator');
  const summary = normalize(String(config.hub?.en?.body || '').trim().split(/\n\s*\n/)[0]);
  const quotedSummary = llms.split('\n').find(line => line.startsWith('> '));
  check(summary && normalize(quotedSummary?.slice(2)) === summary, '/llms.txt', 'summary', 'summary must be the English hub first paragraph');
  check(/^## Programs$/m.test(llms) && /^## Source$/m.test(llms), '/llms.txt', 'headings', 'Programs and Source sections are required');
  const llmsLinks = [...llms.matchAll(/\[([^\]]+)\]\(([^)]+)\)/g)];
  for (const [, , href] of llmsLinks) await reference('/llms.txt', 'link', href, absoluteSite);
  for (const [path, descriptor] of descriptors) {
    if (descriptor.language !== 'en' || descriptor.kind === 'home') continue;
    check(llmsLinks.some(([, , href]) => href === siteURL(path)), path, 'llms.txt', 'English page is missing from llms.txt');
  }
  const programSection = llms.split(/^## Programs\s*$/m)[1]?.split(/^## /m)[0] || '';
  const programLines = programSection.split('\n').filter(line => line.startsWith('- '));
  check(programLines.length === programs.length, '/llms.txt', 'Programs', 'exactly one list item per English program is required');
  for (const program of programs) {
    const expected = `- [${program.name}](${siteURL(`/${program.slug}/`)}): ${program.en?.description}`;
    check(programLines.includes(expected), `/${program.slug}/`, 'llms.txt', 'program name, URL and English description must match pages.toml');
  }
  for (const field of ['repo', 'doi']) check(llmsLinks.some(([, , href]) => href === config[field]), '/llms.txt', `Source.${field}`, `source link is missing: ${config[field]}`);
  try {
    const robots = await readFile(join(output, 'robots.txt'), 'utf8');
    check(/^User-agent:\s*\*\s*$/mi.test(robots) && (/^Allow:\s*\/\s*$/mi.test(robots) || /^Disallow:\s*$/mi.test(robots)) && !/^Disallow:\s*\S/m.test(robots), '/robots.txt', 'rules', 'must allow every crawler and every path');
    check(robots.split(/\r?\n/).includes(`Sitemap: ${siteURL('/sitemap.xml')}`), '/robots.txt', 'Sitemap', 'absolute sitemap directive is missing');
  } catch (error) { fail('/robots.txt', 'file', error.message); }
  try {
    check((await readFile(join(output, 'social-preview.png'))).equals(await readFile(join(repository, 'docs/social-preview.png'))), '/social-preview.png', 'copy', 'must be an unchanged copy of docs/social-preview.png');
  } catch (error) { fail('/social-preview.png', 'file', error.message); }

  // Stable diagnostics make isolated mutation assertions exact and readable.
  const unique = [...new Map(errors.map(error => [JSON.stringify(error), error])).values()];
  unique.sort((left, right) => `${left.page}\0${left.field}\0${left.message}`.localeCompare(`${right.page}\0${right.field}\0${right.message}`, 'en'));
  return { errors: unique, pages: htmlPages.size, programs: programs.length, modules: moduleNames.size };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const result = await auditSite(process.argv[2], process.argv[3]);
  for (const error of result.errors) console.error(`FAIL ${error.page} [${error.field}]: ${error.message}`);
  if (result.errors.length) {
    console.error(`site gate: ${result.errors.length} failure(s); ${result.pages} pages, ${result.programs} programs, ${result.modules} lazy modules`);
    process.exitCode = 1;
  } else {
    console.log(`PASS site gate: ${result.pages} pages, ${result.programs} programs, ${result.modules} lazy modules; text, coverage, discovery, metadata, links and Play targets`);
  }
}
