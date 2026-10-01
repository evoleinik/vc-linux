import { Terminal } from "./vendor/xterm.mjs";
// __V__ is replaced per build, so a cached page never mixes two builds.
import createVC from "./vc.mjs?v=__V__";
import { createSpeaker } from "./speaker.js?v=__V__";

const container = document.getElementById("terminal");
const layout = document.querySelector("main");
const footer = document.querySelector("footer");
const exitMessage = document.getElementById("exit-message");
const encoder = new TextEncoder();
const input = [];
const heldModifiers = new Set();
let inputHead = 0;
let inputOffset = 0;
let inputBytes = 0;
let exited = false;
let terminal;
const speaker = createSpeaker();

function enqueue(data) {
  if (exited || !data) return;
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

const modifierCodes = {
  ShiftLeft: 57441, ControlLeft: 57442, AltLeft: 57443,
  ShiftRight: 57447, ControlRight: 57448, AltRight: 57449,
};
const controlCodes = { "[": 91, i: 105, m: 109, h: 104 };

function modifiers(event) {
  return 1 + (event.shiftKey ? 1 : 0) + (event.altKey ? 2 : 0)
    + (event.ctrlKey ? 4 : 0) + (event.metaKey ? 8 : 0)
    + (event.getModifierState("CapsLock") ? 64 : 0)
    + (event.getModifierState("NumLock") ? 128 : 0);
}

function handleKey(event) {
  if (exited) return false;
  if (event.type !== "keydown" && event.type !== "keyup") return true;
  const modifier = modifierCodes[event.code];
  if (modifier) {
    const release = event.type === "keyup";
    if (release) heldModifiers.delete(modifier);
    else heldModifiers.add(modifier);
    enqueue(`\x1b[${modifier};${modifiers(event)}:${release ? 3 : event.repeat ? 2 : 1}u`);
    event.preventDefault();
    return false;
  }

  // The ordinary terminal bytes confuse these Ctrl keys with Esc, Tab,
  // Enter and Backspace. Kitty reports preserve their actual scan codes.
  if (event.type === "keydown" && event.ctrlKey && !event.altKey && !event.metaKey) {
    if (event.code === "Pause" || (event.shiftKey && event.code === "KeyB")) {
      // Kitty's Pause key (57362), with Ctrl, reaches BIOS INT 1Bh. Using
      // the same report for both shortcuts keeps it distinct from Ctrl+B.
      enqueue("\x1b[57362;5u");
      event.preventDefault();
      return false;
    }
    const code = controlCodes[event.key.toLowerCase()];
    if (code !== undefined) {
      enqueue(`\x1b[${code};${modifiers(event)}u`);
      event.preventDefault();
      return false;
    }
  }
  if (/^F(?:[1-9]|1[0-2])$/.test(event.key) || event.ctrlKey) event.preventDefault();
  // xterm still translates F keys, ordinary Ctrl keys and Alt-letter search.
  return true;
}

function releaseModifiers() {
  for (const code of heldModifiers) enqueue(`\x1b[${code};1:3u`);
  heldModifiers.clear();
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

function fit() {
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
}

function onExit() {
  speaker.silence();
  exited = true;
  input.length = 0;
  inputBytes = inputOffset = inputHead = 0;
  heldModifiers.clear();
  terminal.options.disableStdin = true;
  exitMessage.textContent = "VC has quit. Press any key to start it again.";
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
container.addEventListener("focusout", releaseModifiers);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) releaseModifiers();
});
let resizeFrame = 0;
window.addEventListener("resize", () => {
  cancelAnimationFrame(resizeFrame);
  resizeFrame = requestAnimationFrame(fit);
});

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
    locateFile: (path, prefix) => `${prefix}${path}?v=__V__`,
    vcOutput: (bytes) => terminal.write(bytes),
    vcReadInput: readInput,
    vcExit: onExit,
    vcSpeaker: (frequency) => speaker.setFrequency(frequency),
  });
}

start().catch((error) => {
  console.error(error);
  exitMessage.textContent = `VC could not start (${error?.message || error}). Reload to try again.`;
  exitMessage.hidden = false;
  document.documentElement.dataset.vcState = "failed";
  fit();
});
