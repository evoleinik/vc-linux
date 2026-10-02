import { Terminal } from "./vendor/xterm.mjs?v=__V__";
// __V__ is replaced per build, so a cached page never mixes two builds.
import createVC from "./vc.mjs?v=__V__";
import { createSpeaker } from "./speaker.js?v=__V__";
import { createModemTransport } from "./modem.js?v=__V__";
import { createGraphics } from "./graphics.js?v=__V__";
import { PORTRAIT_QUERY, initialInput, reduceInput, bindKeypad } from "./vc-keypad.js?v=__V__";
import { pageText } from "./vc-language.js?v=__V__";
import { screenLayout } from "./vc-layout.js?v=__V__";

const container = document.getElementById("terminal");
const layout = document.querySelector("main");
const footer = document.querySelector("footer");
const exitMessage = document.getElementById("exit-message");
const sourceButton = document.getElementById("source-button");
const sourceElement = document.getElementById("source-panel");
const touchControls = document.getElementById("keypad");
const portraitMedia = window.matchMedia(PORTRAIT_QUERY);
const viewportMeta = document.querySelector('meta[name="viewport"]');
const defaultViewportContent = viewportMeta.content;
const sourceButtonHome = sourceButton.parentNode;
const sourceButtonNext = sourceButton.nextSibling;
const strings = pageText(navigator.language);
document.documentElement.lang = strings.language;
document.getElementById("page-description").textContent = strings.footer;
document.getElementById("linux-version").textContent = strings.linux;
document.getElementById("font-credit-label").textContent = strings.fontCredit;
document.getElementById("keyboard-button").setAttribute("aria-label", strings.keyboard);
const encoder = new TextEncoder();
const input = [];
let keyState = initialInput();
let keypad;
let inputHead = 0;
let inputOffset = 0;
let inputBytes = 0;
let exited = false;
let terminal;
const speaker = createSpeaker();
const modem = createModemTransport();
let graphics;
let vc, sourcePanel, sourceLoad, sourceSnapshot, sourceResolved;
let sourceWasOpen = false, sourceScroll;
let closedFit, sourceRootStyle;
let portraitWidth = 0, portraitHeight = 0, mobileRenderStyle;

function layoutSource() {
  const opened = !!sourcePanel?.opened;
  // Content refreshes cannot change the geometry or scroll a text selection.
  if (opened === sourceWasOpen) return;
  if (opened) sourceScroll = { left: window.scrollX, top: window.scrollY };
  sourceWasOpen = opened;
  fit(true);
  if (portraitMedia.matches) return;
  if (opened && sourceElement.dataset.position === "below")
    sourceElement.scrollIntoView({ block: "nearest" });
  else if (!opened && sourceScroll) window.scrollTo(sourceScroll);
}

async function toggleSource() {
  if (!vc || exited) return;
  try {
    if (!sourceLoad) sourceLoad = import("./vc-source.js?v=__V__").then(module => {
      if (exited) return null;
      sourcePanel = module.createSourcePanel({
        button: sourceButton, panel: sourceElement,
        indexURL: new URL("./__SOURCE_INDEX__?v=__V__", import.meta.url).href,
        getSnapshot() { vc._vc_source_snapshot(); return sourceSnapshot; },
        resolveAddress(cs, ip, preceding) { vc._vc_source_resolve(cs, ip, preceding); return sourceResolved; },
        getTextarea: () => terminal?.textarea, onLayout: layoutSource,
      });
      sourceButton.title = "Show original source (Ctrl-Shift-F12)";
      sourceButton.removeAttribute("aria-label");
      sourceButton.removeEventListener("click", firstSourceClick);
      sourceButton.removeEventListener("pointerdown", firstSourcePointerDown);
      return sourcePanel;
    }).catch(error => { sourceLoad = null; throw error; });
    const binding = await sourceLoad;
    if (!exited && binding) await binding.toggle();
  } catch (error) {
    if (exited) return;
    // If even the panel module is unavailable, keep the screen unobscured.
    // The same button retries; its tooltip/accessibility label explains why.
    sourceElement.hidden = true;
    sourceButton.setAttribute("aria-expanded", "false");
    sourceButton.title = `Source could not load (${error.message}). Click Source to retry.`;
    sourceButton.setAttribute("aria-label", sourceButton.title);
  }
}
let firstSourceTextarea = null;
function firstSourcePointerDown(event) {
  const textarea = terminal?.textarea;
  // Only a mouse press can move focus here; pointerdown is cancelled. A focus()
  // inside a touch tap opens Android's soft keyboard, so touch never refocuses.
  firstSourceTextarea = event.pointerType === "mouse" && textarea
    && document.activeElement === textarea ? textarea : null;
  event.preventDefault();
}
const firstSourceClick = () => {
  const textarea = firstSourceTextarea;
  firstSourceTextarea = null;
  void toggleSource();
  textarea?.focus({ preventScroll: true });
};
sourceButton.addEventListener("pointerdown", firstSourcePointerDown);
sourceButton.addEventListener("click", firstSourceClick);

// Ctrl-Alt-S is VC's Alt-S speed search. Ctrl-Shift-F12 (8A00h) is unbound
// and avoids VC's Alt-release menu latch. Neither toggle sends guest bytes.
let sourceShortcutHeld = false;
function sourceKey(event) {
  if (exited || (event.code !== "F12" && event.key !== "F12")) return;
  const shortcut = event.ctrlKey && event.shiftKey && !event.altKey && !event.metaKey;
  if (!shortcut && !sourceShortcutHeld) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  if (event.type === "keyup") sourceShortcutHeld = false;
  else if (!event.repeat) { sourceShortcutHeld = true; void toggleSource(); }
}
window.addEventListener("keydown", sourceKey, { capture: true });
window.addEventListener("keyup", sourceKey, { capture: true });
window.addEventListener("blur", () => { sourceShortcutHeld = false; });
document.addEventListener("visibilitychange", () => { sourceShortcutHeld = false; });
// A long press on a touch screen opens the copy bubble or a context menu,
// which misfires while tapping VC's panels. Touch only; desktop keeps both.
const coarsePointer = window.matchMedia("(pointer: coarse)");
document.addEventListener("contextmenu", (event) => { if (coarsePointer.matches) event.preventDefault(); }, { capture: true });

function enqueue(data, raw = false) {
  if (exited || !data) return;
  if (!raw) {
    const result = reduceInput(keyState, { type: "text", data });
    keyState = result.state;
    keypad?.setSticky(keyState.sticky);
    data = result.bytes;
  }
  const bytes = encoder.encode(data);
  input.push(bytes);
  inputBytes += bytes.length;
}

// C pulls bytes only from term_idle, where Asyncify may safely yield.
function readInput(maxBytes) {
  if (!inputBytes || maxBytes <= 0) return null;
  const bytes = new Uint8Array(Math.min(maxBytes, inputBytes));
  let written = 0;
  while (written < bytes.length) {
    const chunk = input[inputHead];
    const count = Math.min(bytes.length - written, chunk.length - inputOffset);
    bytes.set(chunk.subarray(inputOffset, inputOffset + count), written);
    written += count;
    inputOffset += count;
    if (inputOffset === chunk.length) {
      inputHead++;
      inputOffset = 0;
    }
  }
  inputBytes -= written;
  if (inputHead) input.splice(0, inputHead);
  inputHead = 0;
  return bytes;
}

function dispatchInput(action) {
  const result = reduceInput(keyState, action);
  keyState = result.state;
  keypad?.setSticky(keyState.sticky);
  enqueue(result.bytes, true);
  return result;
}

function handleKey(event) {
  if (exited) return false;
  const result = dispatchInput({ type: "key", event });
  if (result.preventDefault) event.preventDefault();
  return !result.handled;
}

function releaseModifiers() {
  dispatchInput({ type: "reset" });
}

function screenText() {
  if (!terminal) return "";
  const buffer = terminal.buffer.active;
  return Array.from({ length: terminal.rows }, (_, row) =>
    buffer.getLine(buffer.viewportY + row)?.translateToString(true) ?? "").join("\n");
}

// A read-only screen snapshot supports state-based browser checks without
// exposing the wasm heap or adding a second input path.
Object.defineProperty(window, "vcScreen", { value: screenText });

function updateViewport() {
  const portraitTouch = portraitMedia.matches;
  const content = portraitTouch ? `${defaultViewportContent}, viewport-fit=cover` : defaultViewportContent;
  // Cover is phone-portrait only: a notched landscape keeps main's viewport.
  if (viewportMeta.content !== content) viewportMeta.content = content;
  if (!portraitTouch) {
    if (sourceButton.parentNode !== sourceButtonHome)
      sourceButtonHome.insertBefore(sourceButton, sourceButtonNext);
    layout.style.removeProperty("--vc-viewport-height");
    layout.style.removeProperty("--vc-viewport-top");
    delete touchControls.dataset.keyboardOpen;
    portraitWidth = portraitHeight = 0;
    return { portraitTouch };
  }
  // Source is part of the footer on a phone, leaving the rest of the gap empty.
  if (sourceButton.parentNode !== footer) footer.appendChild(sourceButton);
  const viewport = window.visualViewport;
  const zoomed = viewport && viewport.scale !== 1;
  const height = !zoomed && viewport ? viewport.height : window.innerHeight;
  const top = !zoomed && viewport ? viewport.offsetTop : 0;
  if (portraitWidth !== window.innerWidth) portraitHeight = 0;
  portraitWidth = window.innerWidth;
  portraitHeight = Math.max(portraitHeight, window.innerHeight, height);
  // Both iOS (visual-only resize) and Android (layout resize too) retain the
  // last unobstructed height. A toolbar change is smaller than a keyboard;
  // pinch zoom is not a keyboard. Hiding the pad never clears sticky input.
  const keyboardOpen = !zoomed && document.activeElement === terminal?.textarea
    && portraitHeight - height > 120;
  touchControls.dataset.keyboardOpen = String(keyboardOpen);
  layout.style.setProperty("--vc-viewport-height", `${height}px`);
  layout.style.setProperty("--vc-viewport-top", `${top}px`);
  return { portraitTouch, height };
}

function fit(reuseClosed = false) {
  if (!terminal?.element) return;
  const viewport = updateViewport();
  const { portraitTouch } = viewport;
  if (mobileRenderStyle) {
    Object.assign(terminal.element.style, mobileRenderStyle);
    mobileRenderStyle = null;
    container.style.removeProperty("--vc-screen-scale-x");
    container.style.removeProperty("--vc-screen-scale-y");
  }
  const screen = terminal.element.querySelector(".xterm-screen");
  const original = screen.getBoundingClientRect();
  if (!original.width || !original.height) return;
  const opened = !!sourcePanel?.opened;
  const scroll = { left: window.scrollX, top: window.scrollY };
  const rootStyle = document.documentElement.style;
  if (opened && !portraitTouch && !sourceRootStyle) sourceRootStyle = { overflowY: rootStyle.overflowY || "" };
  if (sourceRootStyle) rootStyle.overflowY = sourceRootStyle.overflowY;
  if (!opened || portraitTouch) sourceRootStyle = null;
  // Measure main's unchanged flex layout, including naturally wrapped footer
  // text. No open-panel positioning survives a close or a viewport resize.
  sourceElement.hidden = true;
  for (const node of [container, touchControls, footer])
    for (const property of ["position", "left", "top", "width", "height"])
      node.style.removeProperty(property);
  let scrollbarSize;
  if (opened && !portraitTouch) {
    // Native bars differ by platform (including zero-width overlay bars).
    // Measure only while opening/resizing Source, never on the closed page.
    rootStyle.overflowY = "scroll";
    scrollbarSize = Math.max(0, window.innerWidth - document.documentElement.clientWidth);
    rootStyle.overflowY = sourceRootStyle.overflowY;
  }
  const dimensions = node => {
    const rect = node.getBoundingClientRect();
    return { width: rect.width, height: rect.height };
  };
  const currentFont = terminal.options.fontSize;
  // main's estimate is not necessarily idempotent for every fallback font.
  // Toggles reuse its original anchor; a resize starts from its closed size.
  const fontSize = reuseClosed && closedFit ? closedFit.anchor
    : opened && closedFit ? closedFit.fontSize : currentFont;
  const measurements = { [currentFont]: { width: original.width, height: original.height } };
  const input = () => {
    const style = getComputedStyle(layout);
    return {
      width: layout.clientWidth, height: portraitTouch ? viewport.height : layout.clientHeight,
      padding: parseFloat(style.paddingLeft), gap: portraitTouch ? 12 : parseFloat(style.rowGap),
      footer: dimensions(footer),
      controls: getComputedStyle(touchControls).display === "none" ? null : dimensions(touchControls),
      safeCenter: style.justifyContent.includes("safe"), fontSize, measurements, scrollbarSize,
      portraitTouch,
      safeArea: portraitTouch ? {
        top: parseFloat(style.paddingTop), right: parseFloat(style.paddingRight),
        bottom: parseFloat(style.paddingBottom), left: parseFloat(style.paddingLeft),
      } : undefined,
    };
  };
  // Font fallback and device-pixel rounding are measured, not approximated.
  // All size choices and box arithmetic live in the pure function.
  const resolveLayout = input => {
    let result = screenLayout(input);
    while (result.measure !== undefined) {
      terminal.options.fontSize = result.measure;
      measurements[result.measure] = dimensions(screen);
      result = screenLayout(input);
    }
    return result;
  };
  const base = input();
  const closed = resolveLayout({ ...base, open: false });
  closedFit = { anchor: fontSize, fontSize: closed.fontSize };
  let result = opened ? resolveLayout({ ...base, open: true }) : closed;
  if (result.panel?.side === "below") {
    // This layout puts the footer beyond the viewport. Establish its real
    // scrollbar width before measuring wrapping/boxes, even while hidden.
    // The original root policy is restored on close and before each resize.
    rootStyle.overflowY = "scroll";
    result = resolveLayout({ ...input(), open: true });
  }
  terminal.options.fontSize = result.fontSize;
  const place = (node, box) => Object.assign(node.style, {
    position: "absolute", left: `${box.left}px`, top: `${box.top}px`, width: `${box.width}px`,
  });
  if (portraitTouch || result.panel) {
    place(container, result.screen);
    place(footer, result.footer);
    if (result.controls) place(touchControls, result.controls);
  }
  if (portraitTouch) {
    // Keep the 8x16 VGA glyphs at their native size, then scale only this
    // viewport. CGA fills the same box; xterm mouse/selection share the scale.
    const scaleX = result.screen.width / result.nativeScreen.width;
    const scaleY = result.screen.height / result.nativeScreen.height;
    mobileRenderStyle = { width: terminal.element.style.width || "",
      transform: terminal.element.style.transform || "" };
    terminal.element.style.width = `${result.nativeScreen.width}px`;
    terminal.element.style.transform = `scale(${scaleX}, ${scaleY})`;
    container.style.height = `${result.screen.height}px`;
    container.style.setProperty("--vc-screen-scale-x", String(scaleX));
    container.style.setProperty("--vc-screen-scale-y", String(scaleY));
  }
  if (result.panel) {
    place(sourceElement, result.panel);
    sourceElement.style.height = `${result.panel.height}px`;
    sourceElement.dataset.position = result.panel.side;
  }
  sourceElement.hidden = !opened || (portraitTouch && result.panel.height < 20);
  // A layout read with the panel hidden can temporarily clamp root scroll.
  if (!portraitTouch && opened && (window.scrollX !== scroll.left || window.scrollY !== scroll.top))
    window.scrollTo(scroll);
}

function onExit() {
  modem.close();
  sourcePanel?.close();
  sourceButton.disabled = true;
  speaker.silence();
  exited = true;
  input.length = 0;
  inputBytes = inputOffset = inputHead = 0;
  keyState = initialInput();
  keypad?.setSticky(0);
  terminal.options.disableStdin = true;
  exitMessage.textContent = strings.quit;
  exitMessage.hidden = false;
  document.documentElement.dataset.vcState = "quit";
  fit();
}

window.addEventListener("keydown", (event) => {
  if (!exited) speaker.unlock();
  if (!exited) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  window.location.reload();
}, { capture: true });
window.addEventListener("blur", releaseModifiers);
window.addEventListener("pagehide", () => modem.close());
container.addEventListener("focusout", releaseModifiers);
container.addEventListener("pointerdown", () => terminal?.focus());
document.addEventListener("visibilitychange", () => {
  if (document.hidden) releaseModifiers();
});
let resizeFrame = 0;
function scheduleFit() {
  cancelAnimationFrame(resizeFrame);
  resizeFrame = requestAnimationFrame(() => fit());
}
window.addEventListener("resize", scheduleFit);
portraitMedia.addEventListener("change", () => {
  updateViewport();
  scheduleFit();
});
for (const event of ["resize", "scroll"])
  window.visualViewport?.addEventListener(event, () => { if (portraitMedia.matches) scheduleFit(); });
for (const event of ["focusin", "focusout"])
  container.addEventListener(event, () => { if (portraitMedia.matches) scheduleFit(); });
updateViewport();

async function start() {
  try {
    await document.fonts.load('16px "IBM VGA"');
  } catch (error) {
    console.warn("IBM VGA font unavailable; using monospace.", error);
  }
  terminal = new Terminal({
    cols: 80,
    rows: 25,
    fontFamily: '"IBM VGA", monospace',
    fontSize: 16,
    fontWeight: "normal",
    fontWeightBold: "normal",
    lineHeight: 1,
    letterSpacing: 0,
    scrollback: 0,
    cursorBlink: false,
    minimumContrastRatio: 1,
    drawBoldTextInBrightColors: false,
    macOptionIsMeta: true,
    theme: { background: "#000000", foreground: "#aaaaaa" },
  });
  terminal.onData(enqueue);
  terminal.attachCustomKeyEventHandler(handleKey);
  terminal.open(container);
  keypad = bindKeypad(document.getElementById("keypad"), {
    matchMedia: query => window.matchMedia(query),
    onKey(key) {
      if (exited) return window.location.reload();
      speaker.unlock();
      dispatchInput({ type: /^(Control|Alt|Shift)$/.test(key) ? "toggle" : "pad", key });
    },
    onKeyboard() {
      if (exited) return window.location.reload();
      speaker.unlock();
      terminal?.textarea?.focus({ preventScroll: true });
    },
    onHide: () => dispatchInput({ type: "clearSticky" }),
  });
  const canvas = document.createElement("canvas");
  canvas.id = "graphics";
  canvas.hidden = true;
  canvas.setAttribute("aria-label", "CGA graphics screen");
  container.appendChild(canvas);
  graphics = createGraphics(canvas, container);
  fit();
  terminal.focus();
  // Any-motion tracking, so VC's mouse cursor follows the pointer without a click.
  terminal.write("\x1b[?1003h\x1b[?1006h");

  const initialRender = terminal.onRender(() => {
    const text = screenText();
    if (!text.includes("10Quit") || !text.includes("README")) return;
    initialRender.dispose();
    // onRender has updated xterm's visible rows; allow that frame to paint
    // before recording the page-load-to-first-screen measurement.
    requestAnimationFrame(() => {
      performance.mark("vc-first-screen");
      performance.measure("vc-startup", { start: 0, end: "vc-first-screen" });
      document.documentElement.dataset.vcState = "running";
    });
  });
  await createVC({
    preRun: [module => { vc = module; sourceButton.disabled = false; }],
    locateFile: (path, prefix) => `${prefix}${path}?v=__V__`,
    vcOutput: (bytes) => terminal.write(bytes),
    vcReadInput: readInput,
    vcExit: onExit,
    vcSpeaker: (frequency) => speaker.setFrequency(frequency),
    vcModem: modem,
    vcGraphics: (frame) => graphics.draw(frame),
    vcSourceSnapshot: snapshot => { sourceSnapshot = snapshot; },
    vcSourceResolved: address => { sourceResolved = address; },
  });
}

start().catch((error) => {
  console.error(error);
  exitMessage.textContent = `VC could not start (${error?.message || error}). Reload to try again.`;
  exitMessage.hidden = false;
  document.documentElement.dataset.vcState = "failed";
  fit();
});
