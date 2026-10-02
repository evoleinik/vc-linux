// Pure screen/page sizing. The caller supplies exact xterm-screen rectangles
// by font size; {measure: size} asks it to measure another size and retry with
// the SAME input fontSize. No linear font-metric assumption or DOM is needed.
const MIN_SCREEN_FONT = 9;

function fitScreen(width, height, fontSize, measurements) {
  const original = measurements[fontSize];
  if (!original?.width || !original.height) return { measure: fontSize };
  const estimate = Math.max(1, Math.floor(fontSize
    * Math.min(width / original.width, height / original.height)));
  let size = estimate >= 16 ? Math.floor(estimate / 16) * 16 : estimate;
  const fits = size => measurements[size].width <= width && measurements[size].height <= height;
  if (!measurements[size]) return { measure: size };
  while (!fits(size) && size > 1) {
    size = size > 16 ? size - 16 : size - 1;
    if (!measurements[size]) return { measure: size };
  }
  // Match main's extra probe: device-pixel rounding can make it fit even
  // when the proportional estimate picked a smaller preferred size.
  const next = size >= 16 ? size + 16 : size + 1;
  if (!measurements[next]) return { measure: next };
  if (fits(next)) size = next;
  return { fontSize: size, dimensions: measurements[size] };
}

function box(left, top, width, height) {
  return { left, top, width, height, right: left + width, bottom: top + height };
}

const KEYPAD_BOTTOM_GAP = 12;

function portraitLayout({ width, height, footer, controls, safeArea, measurements, open, gap }) {
  const nativeScreen = measurements[16];
  if (!nativeScreen?.width || !nativeScreen.height) return { measure: 16 };
  const { top = 0, right = 0, bottom = 0, left = 0 } = safeArea;
  const usableWidth = Math.max(0, width - left - right);
  const screen = box(left, top, usableWidth, Math.round(usableWidth * 400 / 640));
  const caption = box(left, screen.bottom, usableWidth, footer.height);
  // Keep the bottom row off the browser's address bar and home indicator.
  const keys = controls ? box(left, height - bottom - KEYPAD_BOTTOM_GAP - controls.height, usableWidth, controls.height) : null;
  const result = { fontSize: 16, nativeScreen, screen, controls: keys,
    footer: caption, panel: null };
  if (open) {
    // Portrait never scrolls the page: Source scrolls inside the unused band.
    // The adapter hides a band too short even for the panel's border/padding.
    const end = Math.max(top, (keys?.top ?? height - bottom) - gap);
    const start = Math.min(caption.bottom + gap, end);
    result.panel = { side: "between", ...box(left, start, usableWidth, end - start) };
  }
  return result;
}

export function screenLayout({ width, height, footer, controls = null,
  safeCenter = false, fontSize = 16, measurements = {}, open = false, padding = 12, gap = 12,
  scrollbarSize = 16, portraitTouch = false, safeArea = {} }) {
  if (portraitTouch) return portraitLayout({ width, height, footer, controls, safeArea,
    measurements, open, gap });
  const availableWidth = width - padding * 2;
  const availableHeight = height - padding * 2;
  const minPanelWidth = 60 * 8 + 20 + scrollbarSize; // text, padding/border, native scrollbar
  const minPanelHeight = 10 * 16 + 20 + scrollbarSize; // rows and horizontal scrollbar
  // This is intentionally main's old budget: phone controls never shrink VC.
  const screenHeight = availableHeight - footer.height - gap;
  let panelWidth = 0;
  if (open && availableWidth > minPanelWidth + gap && availableHeight >= minPanelHeight) {
    const phone = measurements[MIN_SCREEN_FONT];
    if (!phone) return { measure: MIN_SCREEN_FONT };
    if (availableWidth >= phone.width + gap + minPanelWidth && screenHeight >= phone.height) {
      panelWidth = Math.min(640, Math.max(minPanelWidth, Math.floor(availableWidth * 0.4)),
        availableWidth - gap - phone.width);
    }
  }
  const fit = fitScreen(availableWidth - (panelWidth ? panelWidth + gap : 0), screenHeight, fontSize, measurements);
  if (fit.measure) return fit;
  const dimensions = fit.dimensions;
  const occupied = dimensions.height + footer.height + gap + (controls ? controls.height + gap : 0);
  const free = availableHeight - occupied;
  const top = padding + (safeCenter ? Math.max(0, free / 2) : free / 2);
  const centered = (dimensions, y) => box((width - dimensions.width) / 2, y, dimensions.width, dimensions.height);
  const screen = centered(dimensions, top);
  const keys = controls ? centered(controls, screen.bottom + gap) : null;
  const result = { fontSize: fit.fontSize, screen, controls: keys,
    footer: centered(footer, (keys || screen).bottom + gap), panel: null };
  if (!open) return result;

  if (panelWidth) {
    // Center the screen/panel pair; controls and the footer retain their own
    // full-page centering. A tall keypad may overflow without moving Source
    // beyond the visible viewport.
    screen.left = (width - screen.width - gap - panelWidth) / 2;
    screen.right = screen.left + screen.width;
    const panelTop = Math.max(padding, Math.min(screen.top, height - padding - minPanelHeight));
    const panelHeight = Math.max(minPanelHeight, Math.min(screen.height, height - padding - panelTop));
    result.panel = { side: "right", ...box(screen.right + gap, panelTop, panelWidth, panelHeight) };
  } else {
    // Preserve main's screen and keypad, then let a short viewport scroll to
    // a useful ten-line panel. Only the footer follows the extra content.
    const panelTop = (keys || screen).bottom + gap;
    const panelHeight = Math.max(minPanelHeight, height - padding - panelTop);
    result.panel = { side: "below", ...centered({ width: Math.min(640, availableWidth), height: panelHeight }, panelTop) };
    result.footer = centered(footer, result.panel.bottom + gap);
  }
  return result;
}
