// Loaded only when Source opens. Guest addresses are resolved by the runtime;
// source ownership and CALL boundaries come exclusively from build-time maps.
export const SOURCE_SHORTCUT = "Ctrl-Shift-F12";

export function isSourceShortcut(event) {
  return (event.code === "F12" || event.key === "F12")
    && event.ctrlKey && event.shiftKey && !event.altKey && !event.metaKey;
}

function instructionAt(map, offset) {
  const rows = map.lines || [];
  let low = 0, high = rows.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (rows[middle][0] <= offset) low = middle + 1;
    else high = middle;
  }
  const row = rows[low - 1];
  return row && offset < row[0] + row[3] ? row : null;
}

export function sourceLocation(map, offset) {
  if (map.kind === "functions") {
    const functions = map.functions || [];
    let low = 0, high = functions.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (functions[middle][0] <= offset) low = middle + 1;
      else high = middle;
    }
    const fn = functions[low - 1];
    return fn && offset < fn[1] ? { image: map.image, function: fn[2], offset: offset - fn[0],
      address: offset, label: "C function (map)", text: `${fn[2]} + 0x${(offset - fn[0]).toString(16)} (C function, map file)` } : null;
  }
  const row = instructionAt(map, offset);
  if (!row) return null;
  const file = map.files[row[1]];
  return { image: map.image, file: file.name, path: file.path, line: row[2],
    offset, via: file.via || [], source: file };
}

function callBefore(map, address, kind) {
  // Do not accept a merely plausible code pointer, or a return following an
  // INT/JMP. The listing must prove a CALL ending at this exact address.
  const calls = map.calls || [];
  let low = 0, high = calls.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (calls[middle][0] < address) low = middle + 1;
    else high = middle;
  }
  for (; low < calls.length && calls[low][0] === address; low++)
    if (calls[low][2] === kind) return calls[low];
  return null;
}

export function walkCallers(snapshot, mapFor, resolveAddress, missing = new Set()) {
  const result = [];
  const current = snapshot.current;
  let cs = current?.cs ?? snapshot.raw.cs;
  const words = snapshot.stack || [];
  const stack = current?.image ? mapFor(current.image)?.stack : null;
  let stackTop = 0x10000;
  if (stack && Number.isInteger(current.cs) && Number.isInteger(current.ip) && Number.isInteger(current.offset)) {
    // A live MZ address gives its load paragraph. Apply the declared stack
    // only when SS still names that original segment; alternate stacks and
    // moved-code contexts with no matching stack keep the bounded fallback.
    const base = (current.cs * 16 + current.ip - current.offset) & 0xfffff;
    if (!(base & 15) && snapshot.raw.ss === (((base >>> 4) + stack.segment) & 0xffff)
        && snapshot.raw.sp <= stack.top)
      stackTop = stack.top;
  }
  const check = (address, kind) => {
    if (!address?.image) return null;
    const map = mapFor(address.image);
    if (!map) { missing.add(address.image); return null; }
    const call = callBefore(map, address.offset, kind);
    if (!call) return null;
    const location = sourceLocation(map, call[1]);
    return location && { ...location, returnOffset: call[0], callKind: kind };
  };
  for (let index = 0; index < words.length && result.length < 8; index++) {
    const word = words[index];
    if (word.sp >= stackTop) break;
    const next = words[index + 1];
    // FAR CALL pushes CS and IP. Once one is accepted, subsequent NEAR
    // returns belong to that caller's CS, not the currently running one.
    const far = next && next.sp < stackTop && check(word.far !== undefined ? word.far
      : resolveAddress(next.word, word.word, 1), "far");
    if (far) {
      result.push({ ...far, sp: word.sp });
      cs = next.word;
      index++;
      continue;
    }
    const near = check(cs === snapshot.current?.cs && word.near !== undefined
      ? word.near : resolveAddress(cs, word.word, 1), "near");
    if (near) result.push({ ...near, sp: word.sp });
  }
  return result;
}

export function recentLines(recent, mapFor, missing = new Set()) {
  const result = [], seen = new Set();
  for (const entry of recent || []) {
    const map = mapFor(entry.image);
    if (!map) { missing.add(entry.image); continue; }
    const line = sourceLocation(map, entry.offset);
    if (!line) continue;
    const key = line.function ? `${line.image}:${line.function}:${line.offset}`
      : `${line.image}:${line.path}:${line.line}`;
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(line);
    if (result.length === 32) break;
  }
  return result;
}

function createStore(indexURL, fetchFile) {
  let index;
  const pending = new Map(), maps = new Map(), texts = new Map();
  const urlFor = name => new URL(name, indexURL).href;
  const request = (url, decode) => {
    if (!pending.has(url)) pending.set(url, (async () => {
      const controller = new AbortController();
      let deadline;
      try {
        return await Promise.race([(async () => {
          const response = await fetchFile(url, { signal: controller.signal });
          if (!response.ok) throw new Error(`Source download failed (${response.status || "no response"})`);
          return decode(response);
        })(), new Promise((_, reject) => {
          deadline = setTimeout(() => {
            controller.abort();
            reject(new Error("Source download timed out"));
          }, 15000);
        })]);
      } finally { clearTimeout(deadline); }
    })().catch(error => { pending.delete(url); throw error; }));
    return pending.get(url);
  };
  const json = url => request(url, async response => JSON.parse(
    new TextDecoder("utf-8", { fatal: true }).decode(await response.arrayBuffer())));
  return {
    async index() {
      if (!index) {
        const data = await json(indexURL);
        if (data.version !== 1 || !data.images) throw new Error("Unsupported source index");
        index = data;
      }
      return index;
    },
    has: image => !!index?.images[image],
    map: image => maps.get(image),
    async loadMap(image) {
      if (!index.images[image] || maps.has(image)) return;
      const map = await json(urlFor(index.images[image].url));
      if (map.version !== 1 || map.image !== image) throw new Error("Source map/image mismatch");
      maps.set(image, map);
    },
    text: file => texts.get(urlFor(file.url)),
    async loadText(file) {
      const url = urlFor(file.url);
      if (texts.has(url)) return;
      const text = await request(url, async response => new TextDecoder("utf-8", { fatal: true })
        .decode(await response.arrayBuffer()));
      texts.set(url, text.split("\n").map(line => line.replace(/\r$/, "")));
    },
  };
}

export function formatSourceLine(line) {
  if (line.function) return line.text;
  // Tab stops belong to the original source, not to the varying file label.
  let column = 0;
  const text = line.text.split("\t").map((part, index) => {
    const spaces = index ? 8 - column % 8 : 0;
    column += spaces + part.length;
    return " ".repeat(spaces) + part;
  }).join("");
  return `${line.file}:${line.line}  ${text}`;
}

export function createSourcePanel({ button, panel, getSnapshot, resolveAddress,
  indexURL, fetchFile = globalThis.fetch, onChange = () => {}, getTextarea = () => null, onLayout = () => {} }) {
  const store = createStore(indexURL, fetchFile);
  const document = panel.ownerDocument;
  let opened = false, view = null, timer = null, active = null, again = false, generation = 0;
  let renderedSnapshot = null, focusOnClick = null;
  const element = (tag, text, className) => {
    const node = document.createElement(tag);
    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;
    return node;
  };
  const render = (next, snapshotKey = null) => {
    view = next;
    renderedSnapshot = snapshotKey;
    const children = [];
    if (next.status !== "ready") children.push(element("p", next.message));
    else {
      const address = next.snapshot.current;
      const hex = value => value.toString(16).toUpperCase().padStart(4, "0");
      children.push(element("h2", `Now — ${hex(address.cs)}:${hex(address.ip)}`));
      if (address.kind === "interrupt") children.push(element("p", "Waiting in this instruction's DOS/BIOS call.", "source-note"));
      const lines = (items, highlight) => {
        const block = element("pre");
        for (const line of items) {
          const row = element("div", formatSourceLine(line),
            highlight && line.line === next.current.line ? "source-current" : "source-line");
          if (line.via?.length) row.setAttribute("title", [
            ...line.via.map(file => `${file.name}:${file.line}`), `${line.file}:${line.line}`,
          ].join(" → "));
          block.appendChild(row);
        }
        return block;
      };
      children.push(lines(next.now, true));
      children.push(element("h2", "Called from"));
      children.push(next.callers.length ? lines(next.callers)
        : element("p", "No CALL return addresses on this stack.", "source-note"));
      children.push(element("h2", "Just ran"));
      children.push(next.recent.length ? lines(next.recent)
        : element("p", "No mapped block entries yet.", "source-note"));
    }
    panel.replaceChildren(...children);
    onLayout();
    onChange(view);
  };

  const draw = async ticket => {
    await store.index();
    // Downloads can outlive guest execution. Retake the snapshot after each
    // fetch, then resolve and render synchronously from that fresh snapshot.
    while (opened && ticket === generation) {
      const snapshot = getSnapshot();
      // The runtime gives each poll fresh, JSON-owned values. Compare their
      // contents, not object identity, and keep selected DOM text untouched.
      const snapshotKey = JSON.stringify(snapshot ?? null);
      if (snapshotKey === renderedSnapshot) return;
      const address = snapshot?.current;
      const image = address?.image;
      if (!image || !store.has(image)) {
        render({ status: "no-map", image, message: `No source map for ${image || "the current program"}.` }, snapshotKey);
        return;
      }
      const needed = new Set([image]);
      for (const entry of snapshot.recent || []) if (store.has(entry.image)) needed.add(entry.image);
      const missing = [...needed].filter(name => !store.map(name));
      if (missing.length) { await Promise.all(missing.map(store.loadMap)); continue; }
      const map = store.map(image);
      const current = sourceLocation(map, address.offset);
      if (!current) {
        render({ status: "no-line", image, message: `No listed source line at ${image} + 0x${address.offset.toString(16)}.` }, snapshotKey);
        return;
      }
      const absent = new Set();
      const callers = walkCallers(snapshot, store.map, resolveAddress, absent);
      const recent = recentLines(snapshot.recent, store.map, absent);
      const missingMaps = [...absent].filter(name => store.has(name) && !store.map(name));
      if (missingMaps.length) { await Promise.all(missingMaps.map(store.loadMap)); continue; }
      const locations = [current, ...callers, ...recent];
      const files = [...new Set(locations.filter(line => line.source && !store.text(line.source))
        .map(line => line.source))];
      if (files.length) { await Promise.all(files.map(store.loadText)); continue; }
      const withText = location => location.source
        ? { ...location, text: store.text(location.source)[location.line - 1] } : location;
      const now = current.source ? store.text(current.source)
        .slice(Math.max(0, current.line - 9), current.line + 8)
        .map((text, index) => ({ ...current, line: Math.max(1, current.line - 8) + index, text }))
        : [current];
      render({ status: "ready", image, snapshot, current: withText(current), now,
        callers: callers.map(withText), recent: recent.map(withText) }, snapshotKey);
      return;
    }
  };

  function refresh() {
    if (!opened) return Promise.resolve(view);
    if (active) { again = true; return active; }
    const ticket = generation;
    active = draw(ticket).catch(error => {
      if (opened && ticket === generation) {
        clearInterval(timer);
        again = false;
        render({ status: "error", message: `${error.message}. Close and reopen Source to retry.` });
      }
    }).finally(() => {
      active = null;
      if (again && opened) { again = false; void refresh(); }
    });
    return active;
  }
  function close() {
    opened = false;
    generation++;
    clearInterval(timer);
    timer = null;
    panel.hidden = true;
    button.setAttribute("aria-expanded", "false");
    onLayout();
  }
  async function toggle() {
    if (opened) { close(); return; }
    opened = true;
    generation++;
    panel.hidden = false;
    button.setAttribute("aria-expanded", "true");
    render({ status: "loading", message: "Loading original source…" });
    // Nothing, including snapshots or fetches, is polled while closed.
    timer = setInterval(() => { void refresh(); }, 100);
    await refresh();
  }
  function handleKey(event) {
    if (!isSourceShortcut(event)) return false;
    event.preventDefault();
    event.stopImmediatePropagation?.();
    if (event.type === "keydown" && !event.repeat) void toggle();
    return true;
  }
  const mouseFocus = event => {
    // Touch never refocuses: focus() inside a tap opens Android's keyboard.
    const textarea = getTextarea();
    return event.pointerType === "mouse" && textarea
      && textarea.ownerDocument.activeElement === textarea ? textarea : null;
  };
  const pointerdown = event => {
    focusOnClick = mouseFocus(event);
    event.preventDefault();
  };
  // A mouse press on the panel moves focus to the page, so VC stops getting
  // keys. Give focus back on release, unless the visitor selected text.
  let panelFocus = null;
  const panelPointerdown = event => { panelFocus = mouseFocus(event); };
  const panelPointerup = () => {
    const textarea = panelFocus;
    panelFocus = null;
    if (textarea && !String(document.getSelection?.() ?? "")) textarea.focus({ preventScroll: true });
  };
  panel.addEventListener("pointerdown", panelPointerdown);
  panel.addEventListener("pointerup", panelPointerup);
  const click = () => {
    const textarea = focusOnClick;
    focusOnClick = null;
    void toggle();
    textarea?.focus({ preventScroll: true });
  };
  button.addEventListener("pointerdown", pointerdown);
  button.addEventListener("click", click);
  return { toggle, refresh, close, handleKey, get opened() { return opened; }, get view() { return view; },
    dispose() {
      close();
      button.removeEventListener("pointerdown", pointerdown);
      button.removeEventListener("click", click);
      panel.removeEventListener("pointerdown", panelPointerdown);
      panel.removeEventListener("pointerup", panelPointerup);
    } };
}
