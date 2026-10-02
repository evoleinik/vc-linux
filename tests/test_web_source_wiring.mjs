// Brief 25: exercise the real page handlers with DOM doubles, never a browser.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import { runInNewContext } from 'node:vm';

const output = process.argv[2] || 'build/web';
const page = readFileSync(join(output, 'index.html'), 'utf8');
const app = readFileSync(join(output, 'vc-web.js'), 'utf8');
const layoutModule = readFileSync(join(output, 'vc-layout.js'), 'utf8');
const { screenLayout } = await import(`data:text/javascript,${encodeURIComponent(layoutModule)}`);

test('Source adds no footer height and stays fixed during layout and scrolling', () => {
  const footer = page.match(/<footer>[\s\S]*?<\/footer>/)?.[0];
  assert.ok(footer);
  assert.doesNotMatch(footer, /source-button/, 'Source must not participate in footer line layout');
  assert.match(footer, /<p class="font-credit"><span id="font-credit-label">/,
    'main\'s font-credit markup, including its leading whitespace, is unchanged');
  const css = page.match(/#source-button\s*\{([^}]+)\}/)?.[1] || '';
  assert.match(css, /position:\s*fixed\s*;/);
  assert.match(css, /(?:top|bottom):\s*\d+px\s*;/);
  assert.match(css, /(?:left|right):\s*\d+px\s*;/);
});

test('Source stays anchored when opening introduces a classic right scrollbar', () => {
  const css = page.match(/#source-button\s*\{([^}]+)\}/)?.[1] || '';
  assert.match(css, /\bleft:\s*12px\s*;/,
    'the left edge is stable even when the root scrollbar changes the right edge');
});

test('closed page has no reserved scrollbar gutter or source overflow wrapper', () => {
  assert.doesNotMatch(page, /scrollbar-gutter\s*:/);
  assert.doesNotMatch(page, /source-overflow|--source-shift/);
  assert.doesNotMatch(app, /sourceOverflow|--source-shift/);
});

test('source text scrolls horizontally inside the panel without wrapping', () => {
  const pre = page.match(/#source-panel pre\s*\{([^}]+)\}/)?.[1] || '';
  assert.match(pre, /white-space:\s*pre\s*;/);
  assert.match(pre, /(?:min-width|width):\s*max-content\s*;/,
    'the highlighted row must cover a whole long line, not clip at the panel edge');
  assert.match(page, /#source-panel\s*\{[^}]*overflow:\s*auto\s*;/s);
  assert.match(page, /#source-panel\s*\{[^}]*tab-size:\s*8\s*;/s);
});

test('source panel cannot take keyboard focus from VC', () => {
  const aside = page.match(/<aside\b[^>]*id="source-panel"[^>]*>/)?.[0] || '';
  assert.ok(aside);
  assert.doesNotMatch(aside, /\btabindex\s*=/);
});

function eventTarget() {
  const listeners = new Map();
  return {
    addEventListener(type, handler) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(handler);
    },
    removeEventListener(type, handler) { listeners.get(type)?.delete(handler); },
    dispatch(type, extra = {}) {
      const event = { type, prevented: false, stopped: false,
        preventDefault() { this.prevented = true; },
        stopImmediatePropagation() { this.stopped = true; }, ...extra };
      for (const handler of listeners.get(type) || []) {
        handler(event);
        if (event.stopped) break;
      }
      return event;
    },
  };
}

function harness(layoutFunction, screenMetric = size => ({ width: 40 * size, height: 25 * size }),
  { portraitTouch = false, safeArea = {}, visualViewport = portraitTouch } = {}) {
  const style = () => ({
    removeProperty(name) { delete this[name]; },
    setProperty(name, value) { this[name] = value; },
    getPropertyValue(name) { return this[name] || ''; },
  });
  const scrolls = [];
  let clampOnMeasure = false;
  const nodes = new Map([...page.matchAll(/\bid="([^"]+)"/g)].map(([, id]) =>
    [id, { ...eventTarget(), textContent: '', hidden: true, attributes: {},
      style: style(), dataset: {},
      getBoundingClientRect: () => ({ width: 366, height: 244 }),
      scrollIntoView: options => scrolls.push(options),
      setAttribute(name, value) { this.attributes[name] = value; },
      removeAttribute(name) { delete this.attributes[name]; } }]));
  const media = { ...eventTarget(), matches: portraitTouch };
  const main = { style: style(), clientWidth: 390, clientHeight: 844 };
  const body = { insertBefore(node) { node.parentNode = this; } };
  const footer = { style: style(), appendChild(node) { node.parentNode = this; },
    getBoundingClientRect: () => ({ width: media.matches ? 390 : 366,
      height: 88 + (nodes.get('source-button').parentNode === footer ? 50 : 0) }) };
  const viewportMeta = { content: 'width=device-width, initial-scale=1' };
  nodes.get('source-button').parentNode = body;
  nodes.get('source-button').nextSibling = {};
  nodes.get('keypad').hidden = !portraitTouch;
  const document = { ...eventTarget(), activeElement: null, hidden: false,
    documentElement: { dataset: {}, style: style() },
    getElementById: id => nodes.get(id), querySelector: selector => selector === 'main' ? main
      : selector === 'footer' ? footer : viewportMeta,
    fonts: { load: () => new Promise(() => {}) } };
  Object.defineProperty(document.documentElement, 'clientWidth', {
    configurable: true, get: () => main.clientWidth,
  });
  const focusCalls = [];
  const textarea = { ownerDocument: document,
    focus(options) { focusCalls.push(options); document.activeElement = this; } };
  let toggles = 0;
  const window = { ...eventTarget(), scrollX: 0, scrollY: 0,
    matchMedia(query) {
      // The long-press guard asks only whether the pointer is coarse.
      if (query === '(pointer: coarse)') return { matches: media.matches };
      assert.equal(query, '(pointer: coarse) and (orientation: portrait)');
      return media;
    },
    innerWidth: 390, innerHeight: 844, scrollTo(options) {
      scrolls.push(options);
      this.scrollX = options.left ?? this.scrollX;
      this.scrollY = options.top ?? this.scrollY;
    } };
  if (visualViewport) window.visualViewport = { ...eventTarget(),
    width: 390, height: 844, offsetTop: 0, scale: 1 };
  const terminal = { textarea, options: { fontSize: 16 },
    element: { style: style(), querySelector: () => ({ getBoundingClientRect() {
      const size = screenMetric(terminal.options.fontSize);
      const scale = terminal.element.style.transform?.match(/scale\(([^,]+),\s*([^)]+)\)/);
      return scale ? { width: size.width * Number(scale[1]), height: size.height * Number(scale[2]) } : size;
    } }) },
    focus: options => textarea.focus(options) };
  const frames = new Map();
  let nextFrame = 0;
  const context = {
    window, document, navigator: { language: 'en' }, TextEncoder, console,
    PORTRAIT_QUERY: '(pointer: coarse) and (orientation: portrait)',
    requestAnimationFrame(callback) { frames.set(++nextFrame, callback); return nextFrame; },
    cancelAnimationFrame(id) { frames.delete(id); },
    pageText: () => ({}), initialInput: () => ({}),
    reduceInput: state => ({ state, bytes: '' }),
    createSpeaker: () => ({ unlock() {} }),
    createModemTransport: () => ({ close() {} }),
    terminalDouble: terminal,
    toggleDouble: () => { toggles++; return Promise.resolve(); },
    screenLayout: layoutFunction,
    getComputedStyle: node => {
      if (clampOnMeasure && nodes.get('source-panel').hidden) window.scrollY = 0;
      if (node === main && media.matches) return {
        paddingLeft: String(safeArea.left || 0), paddingRight: String(safeArea.right || 0),
        paddingTop: String(safeArea.top || 0), paddingBottom: String(safeArea.bottom || 0),
        rowGap: '0', justifyContent: 'flex-start',
      };
      return { paddingLeft: '12', paddingRight: '12',
        paddingTop: '12', paddingBottom: '12', rowGap: '12', justifyContent: 'safe center',
        display: node.hidden || node.dataset?.keyboardOpen === 'true' ? 'none' : 'grid' };
    },
  };
  runInNewContext(app.replace(/^import .*;\r?\n/gm, '')
    .replaceAll('import.meta.url', '"https://local.invalid/vc-web.js"')
    + '\nterminal = terminalDouble; vc = {}; toggleSource = toggleDouble;'
    + '\nglobalThis.geometry = { fit, layoutSource, setOpen(value) { sourcePanel = { opened: value }; } };', context);
  return { window, document, body, media, viewportMeta, button: nodes.get('source-button'), textarea,
    focusCalls, main, footer, nodes, terminal, scrolls, geometry: context.geometry,
    flushFrames() {
      const pending = [...frames.values()];
      frames.clear();
      for (const callback of pending) callback();
    },
    clampScrollOnMeasure() { clampOnMeasure = true; },
    get toggles() { return toggles; } };
}

test('first Source tap with the textarea unfocused never opens the soft keyboard', () => {
  const h = harness();
  h.button.dispatch('pointerdown');
  h.button.dispatch('click');
  assert.equal(h.toggles, 1);
  assert.equal(h.focusCalls.length, 0);
});

test('first Source tap restores only pointerdown focus, using preventScroll', () => {
  const h = harness();
  h.document.activeElement = h.textarea;
  h.button.dispatch('pointerdown', { pointerType: 'mouse' });
  h.document.activeElement = null;
  h.button.dispatch('click');
  assert.equal(JSON.stringify(h.focusCalls), '[{"preventScroll":true}]');
  h.button.dispatch('click', { detail: 0 });
  assert.equal(h.focusCalls.length, 1, 'keyboard activation must not reuse an earlier pointer capture');
});

test('first Source touch tap never calls focus, even with the textarea focused', () => {
  const h = harness();
  h.document.activeElement = h.textarea; // Android focuses it at load, keyboard closed
  h.button.dispatch('pointerdown', { pointerType: 'touch' });
  h.button.dispatch('click');
  assert.equal(h.toggles, 1);
  assert.equal(h.focusCalls.length, 0);
});

test('focus gained after pointerdown does not make a Source tap refocus VC', () => {
  const h = harness();
  h.button.dispatch('pointerdown');
  h.document.activeElement = h.textarea;
  h.button.dispatch('click');
  assert.equal(h.focusCalls.length, 0);
});

for (const lost of ['blur', 'hidden', 'visible']) {
  test(`Source shortcut latch clears on ${lost}`, () => {
    const h = harness();
    h.window.dispatch('keydown', { key: 'F12', code: 'F12', ctrlKey: true, shiftKey: true });
    assert.equal(h.toggles, 1);
    if (lost === 'blur') h.window.dispatch('blur');
    else {
      h.document.hidden = lost === 'hidden';
      h.document.dispatch('visibilitychange');
    }
    const plain = h.window.dispatch('keydown', { key: 'F12', code: 'F12' });
    assert.equal(h.toggles, 1, 'a lost F12 keyup must not turn the next plain F12 into Source');
    assert.equal(plain.prevented, false, 'plain F12 remains available to the guest');
  });
}

test('page fitting delegates exact measured sizes to the pure layout function', () => {
  const calls = [];
  const h = harness(input => {
    calls.push(structuredClone(input));
    if (!input.measurements[9]) return { measure: 9 };
    return { fontSize: 9, screen: {}, footer: {}, controls: null, panel: null };
  });
  h.geometry.fit();
  assert.equal(calls.length, 2, 'fit must ask the pure function, then supply its requested measurement');
  assert.equal(h.terminal.options.fontSize, 9);
  assert.equal(calls[1].fontSize, 16, 'measurement retries retain the original font-size anchor');
  assert.deepEqual(calls[1].measurements[9], { width: 360, height: 225 });
  assert.deepEqual(calls[1].footer, { width: 366, height: 88 });
});

test('below layout scrolls into view once; closing removes every layout override', () => {
  const h = harness(input => ({ fontSize: input.open ? 9 : 16,
    screen: { left: 15, top: 12, width: 360, height: 225 },
    controls: { left: 12, top: 249, width: 366, height: 244 },
    footer: { left: 12, top: 844, width: 366, height: 88 },
    panel: input.open ? { side: 'below', left: 12, top: 505, width: 366, height: 327 } : null }));
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  assert.equal(h.nodes.get('source-panel').style.top, '505px');
  assert.equal(h.nodes.get('source-panel').style.height, '327px');
  assert.equal(h.scrolls.length, 1);
  assert.equal(h.scrolls[0].block, 'nearest');
  h.geometry.layoutSource();
  assert.equal(h.scrolls.length, 1, 'content polling must not scroll again');
  h.geometry.setOpen(false);
  h.geometry.layoutSource();
  assert.equal(h.terminal.options.fontSize, 16);
  for (const node of [h.nodes.get('terminal'), h.nodes.get('keypad'), h.footer]) {
    for (const property of ['position', 'left', 'top', 'width'])
      assert.ok(!node.style[property], `closing restores main's ${property}`);
  }
  assert.equal(h.scrolls.length, 2, 'closing restores the pre-open page scroll');
});

test('resizing an open below panel preserves scroll through the measurement pass', () => {
  const h = harness(input => ({ fontSize: 9,
    screen: { left: 15, top: 12, width: 360, height: 225 }, controls: null,
    footer: { left: 12, top: 1040, width: 366, height: 88 },
    panel: input.open ? { side: 'below', left: 12, top: 700, width: 366, height: 328 } : null }));
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  h.window.scrollY = 300;
  h.clampScrollOnMeasure();
  h.geometry.fit();
  assert.equal(h.window.scrollY, 300, 'hiding Source for measurement must not strand it below the viewport');
});

test('below layout uses the visible scrollbar width, without reserving it when closed', () => {
  const h = harness(screenLayout);
  Object.defineProperty(h.main, 'clientWidth', { get: () =>
    h.document.documentElement.style.overflowY === 'scroll' || !h.nodes.get('source-panel').hidden ? 375 : 390 });
  h.footer.getBoundingClientRect = () => ({ width: h.main.clientWidth - 24, height: 88 });
  h.geometry.fit();
  assert.equal(h.main.clientWidth, 390);
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  const pane = h.nodes.get('source-panel');
  assert.equal(h.main.clientWidth, 375);
  assert.ok(parseFloat(pane.style.left) + parseFloat(pane.style.width) <= h.main.clientWidth - 12,
    'the panel right edge must fit after a classic scrollbar appears');
  assert.equal(parseFloat(h.footer.style.width), 351, 'footer wraps using the same visible width');
  h.geometry.setOpen(false);
  h.geometry.layoutSource();
  assert.equal(h.main.clientWidth, 390);
  assert.ok(!h.document.documentElement.style.overflowY, 'closing restores the root overflow policy');
});

test('open and close reuse the closed fit anchor even with non-idempotent font rounding', () => {
  const h = harness(screenLayout, size => ({
    width: 80 * Math.max(1, Math.floor(size / 2 * 1.25)) / 1.25,
    height: 25 * Math.max(1, Math.floor(size * 1.25)) / 1.25,
  }));
  h.main.clientWidth = 416;
  h.main.clientHeight = 480;
  h.footer.getBoundingClientRect = () => ({ width: 392, height: 70.7 });
  h.geometry.fit();
  assert.equal(h.terminal.options.fontSize, 10);
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  assert.equal(h.terminal.options.fontSize, 10, 'opening below must retain main\'s first chosen size');
  h.geometry.setOpen(false);
  h.geometry.layoutSource();
  assert.equal(h.terminal.options.fontSize, 10, 'closing returns exactly to the pre-open size');
});

test('open layout budgets the measured native scrollbar instead of assuming 16px', () => {
  const calls = [];
  const h = harness(input => {
    calls.push(structuredClone(input));
    return { fontSize: 16, screen: { left: 12, top: 12, width: 640, height: 400 },
      footer: { left: 12, top: 424, width: 1000, height: 50 }, controls: null,
      panel: input.open ? { side: 'right', left: 664, top: 12, width: 640, height: 400 } : null };
  });
  h.main.clientWidth = h.window.innerWidth = 1600;
  Object.defineProperty(h.document.documentElement, 'clientWidth', {
    get: () => h.document.documentElement.style.overflowY === 'scroll' ? 1583 : 1600,
  });
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  assert.ok(calls.some(input => input.open));
  for (const input of calls.filter(input => input.open)) assert.equal(input.scrollbarSize, 17);
  assert.ok(!h.document.documentElement.style.overflowY, 'right layout restores the original root scroll policy');
});

test('portrait fits full-width native VGA pixels above the footer and bottom keypad', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.geometry.fit();
  const screen = h.nodes.get('terminal').style, keys = h.nodes.get('keypad').style;
  assert.equal(screen.left, '0px');
  assert.equal(screen.top, '0px');
  assert.equal(screen.width, '390px');
  assert.equal(screen.height, '244px');
  assert.equal(h.terminal.options.fontSize, 16, 'render the original VGA font before scaling');
  assert.equal(h.terminal.element.style.width, '640px');
  assert.equal(h.terminal.element.style.transform, 'scale(0.609375, 0.61)');
  assert.equal(screen['--vc-screen-scale-x'], '0.609375', 'mouse and selection share the render scale');
  assert.equal(screen['--vc-screen-scale-y'], '0.61');
  assert.equal(h.footer.style.top, '244px');
  assert.equal(keys.top, '588px'); // 844 - 244 - 12px gap
  assert.equal(keys.width, '390px');
  assert.equal(h.button.parentNode, h.footer, 'Source belongs to the footer, not the empty gap or keypad');
  assert.equal(h.main.style['--vc-viewport-height'], '844px');
  assert.match(h.viewportMeta.content, /viewport-fit=cover/);
  for (let i = 0; i < 3; i++) h.geometry.fit();
  assert.equal(h.terminal.element.style.transform, 'scale(0.609375, 0.61)',
    'remeasure untransformed pixels, never compound the previous scale');
  assert.equal(h.focusCalls.length, 0);
  assert.equal(h.scrolls.length, 0);
});

test('portrait reads all four safe insets without adding arbitrary screen margins', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true,
    safeArea: { top: 47, right: 5, bottom: 34, left: 7 } });
  h.geometry.fit();
  const screen = h.nodes.get('terminal').style, keys = h.nodes.get('keypad').style;
  assert.equal(screen.left, '7px');
  assert.equal(screen.top, '47px');
  assert.equal(screen.width, '378px');
  assert.equal(screen.height, '236px');
  assert.equal(keys.top, '554px'); // 844 - 34 inset - 244 - 12px gap
  assert.equal(keys.left, '7px');
  assert.equal(keys.width, '378px');
});

for (const resizeLayout of [false, true]) test(`phone keyboard hides only the pad (${resizeLayout ? 'layout + visual' : 'visual'} viewport resize)`, () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.geometry.fit();
  h.document.activeElement = h.textarea;
  h.window.visualViewport.height = 500;
  h.window.visualViewport.offsetTop = 36;
  if (resizeLayout) h.window.innerHeight = 500;
  h.window.visualViewport.dispatch('resize');
  h.flushFrames();
  assert.equal(h.nodes.get('keypad').dataset.keyboardOpen, 'true');
  assert.equal(h.nodes.get('terminal').style.width, '390px', 'typing never shrinks the screen');
  assert.equal(h.main.style['--vc-viewport-height'], '500px');
  assert.equal(h.main.style['--vc-viewport-top'], '36px');
  assert.equal(h.focusCalls.length, 0, 'layout does not focus or blur the input');
  h.window.visualViewport.height = h.window.innerHeight = 844;
  h.window.visualViewport.offsetTop = 0;
  h.window.visualViewport.dispatch('resize');
  h.flushFrames();
  assert.equal(h.nodes.get('keypad').dataset.keyboardOpen, 'false');
  assert.equal(h.nodes.get('keypad').style.top, '588px');
});

test('browser chrome and pinch zoom do not masquerade as the phone keyboard', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.geometry.fit();
  h.document.activeElement = h.textarea;
  h.window.visualViewport.height = 784;
  h.window.visualViewport.dispatch('resize');
  h.flushFrames();
  assert.equal(h.nodes.get('keypad').dataset.keyboardOpen, 'false');
  assert.equal(h.nodes.get('keypad').style.top, '528px', 'the pad follows ordinary browser-chrome changes');
  h.window.visualViewport.height = 422;
  h.window.visualViewport.scale = 2;
  h.window.visualViewport.dispatch('resize');
  h.flushFrames();
  assert.equal(h.nodes.get('keypad').dataset.keyboardOpen, 'false');
});

test('portrait source uses the empty band and never scrolls the page', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.geometry.fit();
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  const panel = h.nodes.get('source-panel');
  assert.equal(panel.dataset.position, 'between');
  assert.equal(panel.style.top, '394px');
  assert.equal(panel.style.height, '182px');
  assert.equal(panel.hidden, false);
  assert.equal(h.nodes.get('keypad').style.top, '588px');
  assert.equal(h.scrolls.length, 0);
  assert.ok(!h.document.documentElement.style.overflowY);
  h.geometry.setOpen(false);
  h.geometry.layoutSource();
  assert.equal(h.scrolls.length, 0);
});

test('an exhausted portrait Source band cannot let its border overlap touch keys', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.window.innerHeight = h.window.visualViewport.height = h.main.clientHeight = 650;
  h.geometry.setOpen(true);
  h.geometry.layoutSource();
  assert.equal(h.nodes.get('source-panel').hidden, true, 'a panel shorter than its 20px chrome stays hidden');
  assert.equal(h.nodes.get('keypad').style.top, '394px');
  assert.equal(h.scrolls.length, 0);
});

test('leaving portrait removes every mobile override and restores original Source placement', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true });
  h.geometry.fit();
  h.media.matches = false;
  h.nodes.get('keypad').hidden = true;
  h.media.dispatch('change');
  h.flushFrames();
  assert.equal(h.button.parentNode, h.body);
  assert.equal(h.viewportMeta.content, 'width=device-width, initial-scale=1');
  for (const node of [h.nodes.get('terminal'), h.nodes.get('keypad'), h.footer])
    for (const property of ['position', 'left', 'top', 'width', 'height'])
      assert.ok(!node.style[property], `leaving portrait restores ${property}`);
  for (const property of ['--vc-screen-scale-x', '--vc-screen-scale-y'])
    assert.ok(!h.nodes.get('terminal').style[property]);
  assert.ok(!h.terminal.element.style.transform);
  assert.ok(!h.terminal.element.style.width);
  assert.ok(!h.main.style['--vc-viewport-height']);
  assert.ok(!h.main.style['--vc-viewport-top']);
  assert.equal(h.terminal.options.fontSize, 9, 'the unchanged desktop fitter chooses its original size');
});

test('portrait without the visual viewport API keeps a usable pinned layout', () => {
  const h = harness(screenLayout, undefined, { portraitTouch: true, visualViewport: false });
  h.geometry.fit();
  assert.equal(h.nodes.get('keypad').style.top, '588px');
  assert.equal(h.nodes.get('terminal').style.width, '390px');
});
