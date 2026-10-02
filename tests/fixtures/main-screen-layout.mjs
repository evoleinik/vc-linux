// Independent baseline copied from main (31b4c3342432f87bcb4a0d546b6ae51044a1804d),
// web/vc-web.js:112. Keep this arithmetic verbatim; do not import vc-layout.js.
export const mainFitSource = String.raw`function fit() {
  if (!terminal?.element) return;
  const style = getComputedStyle(layout);
  const width = layout.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
  const height = layout.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom)
    - footer.getBoundingClientRect().height - parseFloat(style.rowGap);
  const screen = terminal.element.querySelector(".xterm-screen");
  const original = screen.getBoundingClientRect();
  if (!original.width || !original.height) return;
  const estimate = Math.max(1, Math.floor(terminal.options.fontSize
    * Math.min(width / original.width, height / original.height)));
  let size = estimate >= 16 ? Math.floor(estimate / 16) * 16 : estimate;
  const fits = () => {
    const rect = screen.getBoundingClientRect();
    return rect.width <= width && rect.height <= height;
  };
  terminal.options.fontSize = size;
  while (!fits() && size > 1) {
    size = size > 16 ? size - 16 : size - 1;
    terminal.options.fontSize = size;
  }
  // Test the next preferred size too: device-pixel rounding can make the
  // proportional estimate conservative, especially below the native 16px.
  const next = size >= 16 ? size + 16 : size + 1;
  terminal.options.fontSize = next;
  if (!fits()) terminal.options.fontSize = size;
}`;

const runMainFit = new Function('terminal', 'layout', 'footer', 'getComputedStyle', `${mainFitSource}\nfit();`);

export function mainScreenLayout(input, measure) {
  const { width, height, footer, controls = null, safeCenter = false, fontSize = 16,
    padding = 12, gap = 12 } = input;
  const terminal = { options: { fontSize }, element: { querySelector() {
    return { getBoundingClientRect: () => measure(terminal.options.fontSize) };
  } } };
  runMainFit(terminal, { clientWidth: width, clientHeight: height },
    { getBoundingClientRect: () => footer }, () => ({
      paddingLeft: String(padding), paddingRight: String(padding),
      paddingTop: String(padding), paddingBottom: String(padding), rowGap: String(gap),
    }));

  // main's column flexbox centers all visible children. The phone's safe
  // center switches to start alignment if the screen + controls overflow.
  const dimensions = measure(terminal.options.fontSize);
  const occupied = dimensions.height + footer.height + gap + (controls ? controls.height + gap : 0);
  const free = height - padding * 2 - occupied;
  const top = padding + (safeCenter ? Math.max(0, free / 2) : free / 2);
  const rect = (dimensions, y) => {
    const left = (width - dimensions.width) / 2;
    return { left, top: y, width: dimensions.width, height: dimensions.height,
      right: left + dimensions.width, bottom: y + dimensions.height };
  };
  const screen = rect(dimensions, top);
  const keys = controls ? rect(controls, screen.bottom + gap) : null;
  return { fontSize: terminal.options.fontSize, screen, controls: keys,
    footer: rect(footer, (keys || screen).bottom + gap), panel: null };
}

// Brief 35's closed portrait-touch fixture is separate from the verbatim
// main arithmetic above. Native 80x25 rendering is scaled into a full-width
// 640:400 box; footer flow and the bottom-pinned keys do not affect its size.
export function portraitTouchScreenLayout(input, measure) {
  const { width, height, footer, controls = null } = input;
  const { top = 0, right = 0, bottom = 0, left = 0 } = input.safeArea ?? {};
  const usableWidth = width - left - right;
  const screenHeight = Math.round(usableWidth * 400 / 640);
  const rectangle = (x, y, w, h) => ({ left: x, top: y, width: w, height: h,
    right: x + w, bottom: y + h });
  return {
    fontSize: 16,
    nativeScreen: measure(16),
    screen: rectangle(left, top, usableWidth, screenHeight),
    controls: controls ? rectangle(left, height - bottom - 12 - controls.height, usableWidth, controls.height) : null,  // 12px above the address bar
    footer: rectangle(left, top + screenHeight, usableWidth, footer.height),
    panel: null,
  };
}
