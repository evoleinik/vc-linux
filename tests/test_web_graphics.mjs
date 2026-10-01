import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../web/graphics.js', import.meta.url), 'utf8');
const { createGraphics } = await import(`data:text/javascript,${encodeURIComponent(source)}`);
let image;
let allocations = 0;
const context = {
  imageSmoothingEnabled: true,
  createImageData(width, height) {
    allocations++;
    return { width, height, data: new Uint8ClampedArray(width * height * 4) };
  },
  putImageData(value, x, y) {
    assert.equal(x, 0);
    assert.equal(y, 0);
    image = value;
  },
};
const canvas = {
  hidden: true,
  getContext(type, options) {
    assert.equal(type, '2d');
    assert.deepEqual(options, { alpha: false });
    return context;
  },
};
const classes = new Set();
const labels = {};
const container = {
  classList: { add: name => classes.add(name), remove: name => classes.delete(name) },
  setAttribute: (name, value) => { labels[name] = value; },
};
const graphics = createGraphics(canvas, container);
const pixels = new Uint8Array(320 * 200);
pixels[0] = 1;
pixels[10 * 320 + 100] = 2;
pixels[pixels.length - 1] = 3;
const palette = new Uint8Array([0, 0, 170, 0, 170, 0, 170, 0, 0, 170, 85, 0]);
const frame = { mode: 4, width: 320, height: 200, pixels, palette };
graphics.draw(frame);
assert.equal(canvas.hidden, false, 'a graphics frame reveals the canvas');
assert.equal(classes.has('graphics-mode'), true, 'graphics cover the xterm screen');
assert.equal(canvas.width, 320, 'canvas retains source width, not CSS-scaled width');
assert.equal(canvas.height, 200);
assert.equal(context.imageSmoothingEnabled, false, 'canvas never interpolates CGA pixels');
assert.deepEqual([...image.data.subarray(0, 8)], [0, 170, 0, 255, 0, 0, 170, 255]);
assert.deepEqual([...image.data.subarray((10 * 320 + 100) * 4, (10 * 320 + 100) * 4 + 4)],
  [170, 0, 0, 255], 'exact source coordinates reach the canvas');
assert.deepEqual([...image.data.subarray(-4)], [170, 85, 0, 255], 'CGA brown is not dark yellow');
assert.match(labels['aria-label'], /320 by 200/);

palette.set([255, 255, 85], 9);
graphics.draw(frame);
assert.deepEqual([...image.data.subarray(-4)], [255, 255, 85, 255],
  'palette-only frames repaint the same pixels');
assert.equal(allocations, 1, 'same-size frames reuse ImageData');

const mono = new Uint8Array(640 * 200);
mono[mono.length - 1] = 1;
graphics.draw({ mode: 6, width: 640, height: 200, pixels: mono,
  palette: new Uint8Array([0, 0, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0]) });
assert.equal(canvas.width, 640, 'mode 6 switches to all 640 source pixels');
assert.equal(allocations, 2);
assert.deepEqual([...image.data.subarray(-4)], [255, 255, 255, 255]);

graphics.draw(null);
assert.equal(canvas.hidden, true, 'returning to text hides the canvas');
assert.equal(classes.has('graphics-mode'), false, 'returning to text shows xterm');
assert.match(labels['aria-label'], /80 columns by 25 rows/);
graphics.draw(frame);
assert.equal(canvas.hidden, false, 'a later program may enter graphics again');
assert.throws(() => graphics.draw({ ...frame, width: 80 }), /Invalid CGA frame/);

// The overlay must not replace/remove xterm's textarea: both the ordinary
// keys and custom kitty reports keep their one existing input path.
const page = await readFile(new URL('../web/index.html', import.meta.url), 'utf8');
const app = await readFile(new URL('../web/vc-web.js', import.meta.url), 'utf8');
assert.match(page, /#terminal\.graphics-mode \.xterm\s*\{\s*opacity:\s*0/);
assert.match(page, /#graphics\s*\{[^}]*width:\s*100%[^}]*height:\s*100%[^}]*image-rendering:\s*pixelated[^}]*pointer-events:\s*none/s);
assert.match(app, /vcGraphics:\s*\(frame\)\s*=>\s*graphics\.draw\(frame\)/);
assert.match(app, /terminal\.onData\(enqueue\)/);
assert.match(app, /terminal\.attachCustomKeyEventHandler\(handleKey\)/);
assert.match(app, /pointerdown.*terminal\?\.focus/);
console.log('web graphics: CGA RGB, exact pixels, mode switches, crisp overlay and input preservation passed');
