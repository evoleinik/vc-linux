// Exercise the actual vendored mouse and selection helpers, without a browser.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { runInNewContext } from 'node:vm';

const output = process.argv[2] || 'build/web';
const vendor = readFileSync(join(output, 'vendor/xterm.mjs'), 'utf8');
const start = vendor.indexOf('function Ci(');
const end = vendor.indexOf('Xt=M(', start);
const textareaStart = vendor.indexOf('function Mn(');
const textareaEnd = vendor.indexOf('function Pn(', textareaStart);
assert.ok(start >= 0 && end > start, 'locate the vendored coordinate helpers and mouse service');
assert.ok(textareaStart >= 0 && textareaEnd > textareaStart, 'locate the contextmenu/auxclick textarea helper');
const { localCoordinates, MouseService, positionTextarea } = runInNewContext(
  `${vendor.slice(start, end)}; ${vendor.slice(textareaStart, textareaEnd)};
    ({ localCoordinates: Ci, MouseService: Xt, positionTextarea: Mn })`, {
    window: { getComputedStyle: element => ({ getPropertyValue: name => element.css[name] ?? '' }) },
  });

function fixture(width, height, nativeWidth = 640, nativeHeight = 400, padding = [0, 0]) {
  const scaleX = width / nativeWidth, scaleY = height / nativeHeight;
  const rectangle = { left: 17.25, top: 53.5, width, height };
  const element = {
    css: { 'padding-left': `${padding[0]}px`, 'padding-top': `${padding[1]}px`,
      '--vc-screen-scale-x': String(scaleX), '--vc-screen-scale-y': String(scaleY) },
    getBoundingClientRect: () => rectangle,
  };
  const service = new MouseService({ dimensions: { css: {
    canvas: { width: nativeWidth, height: nativeHeight },
    cell: { width: nativeWidth / 80, height: nativeHeight / 25 },
  } } }, { hasValidSize: true });
  const event = (x, y) => ({
    clientX: rectangle.left + (x + padding[0]) * scaleX,
    clientY: rectangle.top + (y + padding[1]) * scaleY,
  });
  return { service, element, event, rectangle };
}

let cells = 0;
for (const [width, height, nativeWidth, nativeHeight] of [
  [390, 244, 640, 400], [360, 225, 640, 400], [430, 269, 640, 400],
  [338, 211, 640, 400], [390, 244, 680, 416],
]) {
  for (const padding of [[0, 0], [10, 6]]) {
    const { service, element, event } = fixture(width, height, nativeWidth, nativeHeight, padding);
    // Every cell center includes all four corners and catches either axis
    // retaining native pixels after its visual rectangle has been scaled.
    for (let row = 0; row < 25; row++) for (let col = 0; col < 80; col++) {
      const pointer = event((col + 0.5) * nativeWidth / 80, (row + 0.5) * nativeHeight / 25);
      const report = service.getMouseReportCoords(pointer, element);
      assert.deepEqual({ col: report.col, row: report.row }, { col, row },
        `${width}x${height} mouse cell ${col},${row}, padding ${padding}`);
      assert.deepEqual(Array.from(service.getCoords(pointer, element, 80, 25)), [col + 1, row + 1],
        `${width}x${height} selection cell ${col},${row}, padding ${padding}`);
      cells++;
    }
    for (const [x, y, col, row] of [
      [-20, -20, 0, 0], [nativeWidth + 20, -20, 79, 0],
      [-20, nativeHeight + 20, 0, 24], [nativeWidth + 20, nativeHeight + 20, 79, 24],
    ]) {
      const report = service.getMouseReportCoords(event(x, y), element);
      assert.deepEqual({ col: report.col, row: report.row }, { col, row },
        'mouse drags outside a transformed screen still clamp to its edge');
    }
    for (const col of [0, 39, 79]) for (const fraction of [0.25, 0.75]) {
      const pointer = event((col + fraction) * nativeWidth / 80, 12.5 * nativeHeight / 25);
      assert.deepEqual(Array.from(service.getCoords(pointer, element, 80, 25, true)),
        [col + (fraction < 0.5 ? 1 : 2), 13], 'selection retains its native half-cell rounding');
    }
    for (const [x, y] of [[0, 0], [nativeWidth - 1, 0], [0, nativeHeight - 1],
      [nativeWidth - 1, nativeHeight - 1], [nativeWidth / 2, nativeHeight / 2]]) {
      let focused = 0;
      const textarea = { style: {}, focus() { focused++; } };
      positionTextarea(event(x, y), textarea, element);
      // This helper historically does not subtract screen padding, unlike
      // cell hit-testing. Retain its native 20px box and 10px pointer offset.
      assert.ok(Math.abs(parseFloat(textarea.style.left) - (x + padding[0] - 10)) < 1e-9,
        `${width}x${height} contextmenu textarea left, padding ${padding}`);
      assert.ok(Math.abs(parseFloat(textarea.style.top) - (y + padding[1] - 10)) < 1e-9,
        `${width}x${height} contextmenu textarea top, padding ${padding}`);
      assert.equal(textarea.style.width, '20px');
      assert.equal(textarea.style.height, '20px');
      assert.equal(textarea.style.zIndex, '1000');
      assert.equal(focused, 1, 'contextmenu/auxclick retains exactly its existing focus call');
    }
  }
}

const cssWindow = { getComputedStyle: element => ({ getPropertyValue: name => element.css[name] ?? '' }) };
for (const properties of [{}, { '--vc-screen-scale-x': '1', '--vc-screen-scale-y': '1' },
  { '--vc-screen-scale-x': '', '--vc-screen-scale-y': 'invalid' }]) {
  const { element, rectangle } = fixture(640, 400);
  element.css = { 'padding-left': '5.5px', 'padding-top': '7px', ...properties };
  for (const [clientX, clientY] of [[0, 0], [17.25, 53.5], [192.768, 210.035], [800, 600]]) {
    assert.deepEqual(Array.from(localCoordinates(cssWindow, { clientX, clientY }, element)),
      [clientX - rectangle.left - 5, clientY - rectangle.top - 7],
      'without a portrait transform the original native arithmetic is exact, including padding');
    const textarea = { style: {}, focus() {} };
    positionTextarea({ clientX, clientY }, textarea, element);
    assert.equal(textarea.style.left, `${clientX - rectangle.left - 10}px`);
    assert.equal(textarea.style.top, `${clientY - rectangle.top - 10}px`);
  }
}
console.log(`web mouse: ${cells} transformed cell centers, selection/contextmenu/corners/padding, and exact unscaled coordinates passed`);
