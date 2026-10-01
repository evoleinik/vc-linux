// Raw binary transport only. The shared C modem owns the phonebook, Hayes
// commands, telnet negotiation, carrier signals, and 14400-bps pacing.
// Values are the ModemTransportState ABI in runtime/modem.h.
export const modemTransportStates = Object.freeze({
  CLOSED: 0, CONNECTING: 1, OPEN: 2, NO_ANSWER: 3, BUSY: 4,
});

export function createModemTransport({
  WebSocketClass = globalThis.WebSocket,
  now = () => globalThis.performance?.now() ?? Date.now(),
  bufferBytes = 64 * 1024,
} = {}) {
  if (!Number.isInteger(bufferBytes) || bufferBytes < 1 || bufferBytes > 256 * 1024)
    throw new RangeError('modem buffer must contain 1..262144 bytes');
  const states = modemTransportStates;
  const received = new Uint8Array(bufferBytes);
  let socket, generation = 0, state = states.CLOSED, opened = false;
  let head = 0, queued = 0, dialStarted = 0;

  function detach(candidate) {
    candidate.onopen = candidate.onmessage = candidate.onerror = candidate.onclose = null;
  }

  function finish(nextState) {
    const previous = socket;
    socket = undefined;
    state = nextState;
    if (!previous) return;
    detach(previous);
    // A blocked constructor, failed TLS handshake or already closed socket
    // is an ordinary modem result, never a page-breaking exception.
    try { previous.close(); } catch { /* the call is already finished */ }
  }

  function close() {
    ++generation;
    finish(states.CLOSED);
    head = queued = 0;
    opened = false;
  }

  return {
    dial(url) {
      close();
      state = states.CONNECTING;
      dialStarted = now();
      const attempt = generation;
      let candidate;
      try {
        if (!WebSocketClass || typeof url !== 'string' || !url) throw Error('no endpoint');
        candidate = new WebSocketClass(url);
        candidate.binaryType = 'arraybuffer';
        socket = candidate;
        const current = () => socket === candidate && generation === attempt;
        candidate.onopen = () => {
          if (!current()) return;
          state = states.OPEN;
          opened = true;
        };
        candidate.onmessage = event => {
          if (!current() || state !== states.OPEN) return;
          let bytes;
          if (event.data instanceof ArrayBuffer) bytes = new Uint8Array(event.data);
          else if (ArrayBuffer.isView(event.data))
            bytes = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
          // binaryType guarantees ArrayBuffer in a browser. Reject text and
          // Blob rather than asynchronously reordering raw telnet frames.
          if (!bytes || bytes.length > received.length - queued) {
            finish(states.CLOSED);
            return;
          }
          const tail = (head + queued) % received.length;
          const first = Math.min(bytes.length, received.length - tail);
          received.set(bytes.subarray(0, first), tail);
          received.set(bytes.subarray(first), 0);
          queued += bytes.length;
        };
        candidate.onerror = () => {
          if (current()) finish(opened ? states.CLOSED : states.NO_ANSWER);
        };
        candidate.onclose = event => {
          if (!current()) return;
          // RFC 6455 code 1013 explicitly says "Try Again Later".
          finish(opened ? states.CLOSED : event.code === 1013 ? states.BUSY : states.NO_ANSWER);
        };
      } catch {
        if (candidate) {
          try { candidate.close(); } catch { /* failed construction */ }
        }
        finish(states.NO_ANSWER);
      }
    },

    status() {
      if (state === states.CONNECTING && now() - dialStarted >= 30000) finish(states.NO_ANSWER);
      // Preserve the last payload before reporting remote carrier loss.
      return opened && queued ? states.OPEN : state;
    },

    read(capacity) {
      if (!Number.isInteger(capacity) || capacity <= 0 || !queued) return null;
      const count = Math.min(capacity, queued);
      const bytes = new Uint8Array(count);
      const first = Math.min(count, received.length - head);
      bytes.set(received.subarray(head, head + first));
      bytes.set(received.subarray(0, count - first), first);
      head = (head + count) % received.length;
      queued -= count;
      return bytes;
    },

    write(bytes) {
      if (state !== states.OPEN || !socket || !bytes?.length) return 0;
      if (socket.readyState !== 1) {
        finish(states.CLOSED);
        return 0;
      }
      const count = Math.min(bytes.length, Math.max(0, bufferBytes - socket.bufferedAmount));
      if (!count) return 0;
      try {
        // WebSocket copies send()'s input; also copy here so injected
        // transports cannot retain a view into the changing wasm heap.
        socket.send(bytes.slice(0, count));
        return count;
      } catch {
        finish(states.CLOSED);
        return 0;
      }
    },

    close,
  };
}
