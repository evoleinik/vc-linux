// The page chooses a language without needing a DOM. DOS text remains CP866;
// Ukrainian is deliberately confined to the Unicode page.
//   node tests/web_language.mjs build/web-work/demo
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { runInNewContext } from 'node:vm';

const source = readFileSync(new URL('../web/vc-language.js', import.meta.url), 'utf8');
const { pageText } = await import(`data:text/javascript,${encodeURIComponent(source)}`);

for (const tag of [undefined, null, '', 'en', 'en-US', 'en-GB', 'de-DE', 'fr',
  'russian', 'ukrainian', '__proto__', 'constructor']) {
  const text = pageText(tag);
  assert.equal(text.language, 'en', `${String(tag)} falls back to English`);
  assert.equal(text.quit, 'VC has quit. Press any key to start it again.');
  assert.equal(text.keyboard, 'Keyboard', 'the keyboard label falls back to English');
  assert.equal(text.footer + text.linux, 'Volkov Commander 4.99, machine-translated '
    + 'from 8086 assembly to WebAssembly. Not an emulator. Files live in memory '
    + 'and vanish on reload. Linux version on GitHub.');
}
for (const [language, tags, footer, quit, keyboard] of [
  ['ru', ['ru', 'ru-RU', 'ru-KZ', 'RU-ru'],
    'Volkov Commander 4.99, автоматически переведен с ассемблера 8086 в '
    + 'WebAssembly. Это не эмулятор. Файлы живут в памяти и исчезают при '
    + 'обновлении страницы. Версия для Linux доступна на GitHub.',
    'VC завершил работу. Нажмите любую клавишу, чтобы запустить его снова.',
    'Клавиатура'],
  ['uk', ['uk', 'uk-UA', 'UK-ua'],
    'Volkov Commander 4.99, автоматично перекладений з асемблера 8086 на '
    + "WebAssembly. Це не емулятор. Файли живуть у пам'яті та зникають при "
    + 'оновленні сторінки. Версія для Linux доступна на GitHub.',
    'VC завершив роботу. Натисніть будь-яку клавішу, щоб запустити його знову.',
    'Клавіатура'],
]) {
  for (const tag of tags) {
    const text = pageText(tag);
    assert.equal(text.language, language, `${tag} selects ${language}`);
    assert.equal(text.footer + text.linux, footer, 'the supplied footer is preserved');
    assert.equal(text.quit, quit, 'quit uses the same language as the footer');
    assert.equal(text.keyboard, keyboard, 'the keyboard label uses the same language as the footer');
    assert.equal(text.fontCredit, 'Шрифт IBM VGA: ');
    assert.ok(Object.isFrozen(text), 'callers cannot corrupt another locale lookup');
  }
}

const russian = readFileSync(new URL('../web/README-RU.TXT', import.meta.url), 'utf8');
const decoder = new TextDecoder('ibm866');
const alphabet = decoder.decode(Uint8Array.from({ length: 256 }, (_, i) => i));
assert.ok([...russian].every((char) => alphabet.includes(char)), 'the Russian source fits CP866');
assert.ok(russian.split('\n').every((line) => line.length < 76), 'the README fits the DOS viewer');
assert.equal(russian.split('\n')[0], 'Volkov Commander в вашем браузере.');
assert.ok(pageText('uk-UA').footer.includes('і'), 'correct Ukrainian keeps the Cyrillic і');
assert.ok(!alphabet.includes('і'), 'Ukrainian must not be forced into CP866');

const demo = process.argv[2] || 'build/web-work/demo';
const encoded = readFileSync(join(demo, 'ПРОЧТИ.TXT'));
assert.equal(decoder.decode(encoded), russian.replaceAll('\n', '\r\n'),
  'H:\\ПРОЧТИ.TXT contains the exact Russian README as CP866 with DOS newlines');
assert.notEqual(encoded.toString('utf8'), russian, 'the DOS README must not contain UTF-8 bytes');

// Exercise the real page wiring without a browser. Font loading stays
// pending so startup cannot construct xterm or run wasm in this small DOM.
const app = readFileSync(new URL('../web/vc-web.js', import.meta.url), 'utf8');
const html = readFileSync(new URL('../web/index.html', import.meta.url), 'utf8');
for (const language of ['ru-RU', 'uk-UA', 'en-US', 'de-DE', undefined]) {
  let modemCloses = 0;
  const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(([, id]) =>
    [id, { textContent: '', hidden: true, addEventListener() {} }]));
  const document = {
    documentElement: { lang: 'en', dataset: {} },
    getElementById: (id) => nodes.get(id) || null,
    querySelector: () => ({}),
    addEventListener() {},
    fonts: { load: () => new Promise(() => {}) },
  };
  // This non-module VM supplies the real script URL for lazy Source URLs;
  // font loading remains pending, so no dynamic import or wasm can run.
  runInNewContext(app.replace(/^import .*;\r?\n/gm, '')
    .replaceAll('import.meta.url', JSON.stringify(new URL('../web/vc-web.js', import.meta.url).href))
    + '\nterminal = { options: {} }; onExit();', {
    document,
    window: { addEventListener() {} },
    navigator: { language },
    pageText,
    TextEncoder,
    initialInput: () => ({}),
    createSpeaker: () => ({ silence() {} }),
    createModemTransport: () => ({ close() { modemCloses++; } }),
    console,
  });
  const text = pageText(language);
  assert.equal(document.documentElement.lang, text.language, 'the page updates its language');
  assert.equal(nodes.get('page-description')?.textContent, text.footer, 'the page uses the footer');
  assert.equal(nodes.get('linux-version')?.textContent, text.linux, 'the link text is translated');
  assert.equal(nodes.get('font-credit-label')?.textContent, text.fontCredit, 'the credit is translated');
  assert.equal(nodes.get('keyboard-button')?.textContent, text.keyboard,
    'the real keyboard button uses the translated label');
  assert.equal(nodes.get('exit-message').textContent, text.quit, 'the actual exit hook is translated');
  assert.equal(nodes.get('exit-message').hidden, false);
  assert.equal(document.documentElement.dataset.vcState, 'quit');
  assert.equal(modemCloses, 1, 'the page closes any modem call when VC exits');
}
assert.match(app, /^import \{ pageText \} from ["']\.\/vc-language\.js\?v=/m,
  'the page imports the language module with a build hash');
console.log('web language: ru/uk/en page, keyboard and quit wiring, locale fallback, and the CP866 Russian README passed');
