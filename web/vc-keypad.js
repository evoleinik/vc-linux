export const PORTRAIT_QUERY = "(pointer: coarse) and (orientation: portrait)";

const stickyKeys = { Shift: [1, 57441], Alt: [2, 57443], Control: [4, 57442] };
const modifierCodes = {
  ShiftLeft: 57441, ControlLeft: 57442, AltLeft: 57443,
  ShiftRight: 57447, ControlRight: 57448, AltRight: 57449,
};
const namedCodes = {
  Escape: 27, Enter: 13, Tab: 9, Backspace: 127,
  Insert: 57348, Delete: 57349,
  ArrowLeft: 57350, ArrowRight: 57351, ArrowUp: 57352, ArrowDown: 57353,
  PageUp: 57354, PageDown: 57355, Home: 57356, End: 57357, Pause: 57362,
};
const controlCodes = { "[": 91, i: 105, m: 109, h: 104 };
const report = (code, flags, type) => `\x1b[${code};${flags + 1}${type ? `:${type}` : ""}u`;

export function keySequence(key, flags = 0) {
  let code = namedCodes[key];
  if (/^F(?:[1-9]|1[0-2])$/.test(key)) code = 57363 + Number(key.slice(1));
  if (code === undefined && [...key].length === 1) code = key.codePointAt(0);
  return code === undefined ? null : report(code, flags);
}

export function initialInput() {
  return { sticky: 0, physical: 0, held: [] };
}

function eventFlags(event) {
  return (event.shiftKey ? 1 : 0) | (event.altKey ? 2 : 0)
    | (event.ctrlKey ? 4 : 0) | (event.metaKey ? 8 : 0)
    | (event.getModifierState?.("CapsLock") ? 64 : 0)
    | (event.getModifierState?.("NumLock") ? 128 : 0);
}

function result(state, bytes = "", handled = false, preventDefault = handled) {
  return { state, bytes, handled, preventDefault };
}

function clearSticky(state, bytes = "") {
  for (const [flag, code] of Object.values(stickyKeys)) {
    // A physical modifier can outlive its one-shot counterpart.
    if ((state.sticky & flag) && !(state.physical & flag))
      bytes += report(code, state.physical, 3);
  }
  return result({ ...state, sticky: 0 }, bytes, true);
}

// All transitions are pure. Real keys, pad keys and committed phone text
// share this encoder, then enter the page's existing terminal input queue.
export function reduceInput(state, action) {
  if (action.type === "toggle") {
    const modifier = stickyKeys[action.key];
    if (!modifier) return result(state);
    const [flag, code] = modifier;
    const next = { ...state, sticky: state.sticky ^ flag };
    const down = !!(next.sticky & flag);
    return result(next, !down && (state.physical & flag) ? ""
      : report(code, next.sticky | next.physical, down ? 1 : 3), true);
  }
  if (action.type === "clearSticky") return clearSticky(state);
  if (action.type === "reset") {
    const codes = new Set(state.held);
    for (const [flag, code] of Object.values(stickyKeys)) {
      if ((state.sticky | state.physical) & flag) codes.add(code);
    }
    return result(initialInput(), [...codes].map(code => report(code, 0, 3)).join(""), true);
  }
  if (action.type === "text") {
    const data = action.data;
    if (state.sticky && /^\x1b\[<\d+;\d+;\d+[Mm]$/.test(data)) {
      // SGR mouse reports include a physical-only modifier snapshot. Keep
      // the click intact, then restore the still-pending sticky key bar.
      const held = Object.values(stickyKeys).filter(([flag]) => state.sticky & flag)
        .map(([, code]) => report(code, state.sticky | state.physical, 1)).join("");
      return result(state, data + held);
    }
    // xterm also emits mouse/focus reports here. They are not the next key.
    if (!state.sticky || !data || data.startsWith("\x1b")) return result(state, data);
    const key = [...data][0];
    const consumed = clearSticky(state, keySequence(key, state.sticky | state.physical));
    return { ...consumed, bytes: consumed.bytes + data.slice(key.length) };
  }
  if (action.type === "pad") {
    const bytes = keySequence(action.key, state.sticky | state.physical);
    return bytes === null ? result(state) : clearSticky(state, bytes);
  }
  if (action.type !== "key") return result(state);

  const event = action.event;
  if (event.type !== "keydown" && event.type !== "keyup") return result(state);
  state = { ...state, physical: eventFlags(event) };
  const flags = state.physical | state.sticky;
  const modifier = modifierCodes[event.code];
  if (modifier) {
    const release = event.type === "keyup";
    const held = new Set(state.held);
    if (release) held.delete(modifier);
    else held.add(modifier);
    return result({ ...state, held: [...held] },
      report(modifier, flags, release ? 3 : event.repeat ? 2 : 1), true);
  }
  if (event.type !== "keydown" || event.isComposing) return result(state);

  let bytes = null;
  if ((flags & (4 | 2 | 8)) === 4) {
    if (event.code === "Pause" || ((flags & 1) && event.code === "KeyB")) {
      // Retain the existing Ctrl-Pause / Ctrl-Shift-B BASIC break shortcut.
      bytes = report(57362, 4);
    } else {
      const code = controlCodes[event.key.toLowerCase()];
      if (code !== undefined) bytes = report(code, flags);
    }
  }
  if (bytes === null && state.sticky) bytes = keySequence(event.key, flags);
  if (bytes !== null) return clearSticky(state, bytes);
  // Unmodified desktop keys keep xterm's established translations.
  return result(state, "", false, /^F(?:[1-9]|1[0-2])$/.test(event.key) || event.ctrlKey);
}

// Only the keypad element is touched: screen taps remain xterm mouse input.
export function bindKeypad(element, { matchMedia, onKey, onKeyboard, onHide }) {
  const media = matchMedia(PORTRAIT_QUERY);
  const updateVisibility = () => {
    element.hidden = !media.matches;
    if (element.hidden) onHide();
  };
  const buttonAt = event => {
    const button = event.target.closest("button[data-key]");
    return button && element.contains(button) ? button : null;
  };
  const pointerdown = event => {
    // Keep focus in xterm; a pad tap must not open the phone keyboard or
    // scroll a newly focused button into view. Keyboard has its own button.
    if (buttonAt(event)) event.preventDefault();
  };
  const click = event => {
    const button = buttonAt(event);
    if (!button) return;
    event.preventDefault();
    if (button.dataset.key === "Keyboard") onKeyboard();
    else onKey(button.dataset.key);
  };
  element.addEventListener("pointerdown", pointerdown);
  element.addEventListener("click", click);
  media.addEventListener("change", updateVisibility);
  updateVisibility();
  return {
    setSticky(flags) {
      for (const button of element.querySelectorAll("button[data-key]")) {
        const modifier = stickyKeys[button.dataset.key];
        if (modifier) button.setAttribute("aria-pressed", String(!!(flags & modifier[0])));
      }
    },
    dispose() {
      element.removeEventListener("pointerdown", pointerdown);
      element.removeEventListener("click", click);
      media.removeEventListener("change", updateVisibility);
    },
  };
}
