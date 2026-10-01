import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

// Exercise the page's actual adapter; never construct a real WebSocket.
const source = await readFile(new URL('../web/modem.js', import.meta.url), 'utf8');
const { createModemTransport, modemTransportStates: state } = await import(
  `data:text/javascript,${encodeURIComponent(source)}`);
const sockets = [];
class FakeWebSocket {
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.bufferedAmount = 0;
    this.sent = [];
    this.closeCalls = 0;
    sockets.push(this);
  }
  open() { this.readyState = 1; this.onopen?.({}); }
  receive(data) { this.onmessage?.({ data }); }
  fail() { this.onerror?.({}); }
  peerClose(code = 1000) { this.readyState = 3; this.onclose?.({ code }); }
  send(bytes) {
    if (this.readyState === 0) throw Error('InvalidStateError');
    if (this.readyState >= 2) return; // WebSocket silently discards after closing.
    if (this.sendError) throw Error('network failed');
    this.sent.push(bytes);
    this.bufferedAmount += bytes.length;
  }
  close() { this.closeCalls++; this.readyState = 3; }
}

let clock = 0;
const modem = createModemTransport({ WebSocketClass: FakeWebSocket, now: () => clock, bufferBytes: 8 });
assert.equal(sockets.length, 0, 'page startup must not open a WebSocket');
assert.equal(modem.status(), state.CLOSED);
assert.equal(modem.read(8), null);
assert.equal(modem.write(new Uint8Array([1])), 0);
assert.equal(sockets.length, 0, 'status/read/write must never dial');

modem.dial('wss://fixture.invalid/raw-telnet');
assert.equal(sockets.length, 1);
let socket = sockets.at(-1);
assert.equal(socket.url, 'wss://fixture.invalid/raw-telnet', 'endpoint comes from the C phonebook');
assert.equal(socket.binaryType, 'arraybuffer', 'all incoming frames are binary, never Blob promises');
assert.equal(modem.status(), state.CONNECTING);
assert.equal(modem.write(new Uint8Array([1])), 0, 'CONNECTING must not accept guest bytes');
socket.open();
assert.equal(modem.status(), state.OPEN);
socket.receive(new Uint8Array([0xff, 0xfb, 0, 0xff, 0xff]).buffer);
assert.deepEqual([...modem.read(2)], [0xff, 0xfb]);
assert.deepEqual([...modem.read(9)], [0, 0xff, 0xff], 'transport must leave telnet processing to C');
assert.equal(modem.read(9), null);
socket.receive(new Uint8Array([0, 1, 2, 3, 4, 5]));
assert.deepEqual([...modem.read(4)], [0, 1, 2, 3]);
socket.receive(new Uint8Array([6, 7, 8, 9, 10, 11]));
assert.deepEqual([...modem.read(8)], [4, 5, 6, 7, 8, 9, 10, 11], 'bounded ring preserves wraparound order');
assert.equal(modem.read(0), null);
assert.equal(modem.read(-1), null);

const outgoing = new Uint8Array([0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8]);
assert.equal(modem.write(outgoing), 8, 'send returns a partial count at the high-water mark');
assert.deepEqual([...socket.sent[0]], [...outgoing.subarray(0, 8)]);
outgoing[0] = 99;
assert.equal(socket.sent[0][0], 0xff, 'send must not retain a view into the mutable wasm heap');
assert.equal(modem.write(new Uint8Array([7, 8])), 0, 'backpressure never adds an unbounded JS send queue');
socket.bufferedAmount = 7;
assert.equal(modem.write(new Uint8Array([7, 8])), 1, 'available capacity is consumed exactly');
socket.bufferedAmount = 0;
assert.equal(modem.write(new Uint8Array([8])), 1);
assert.equal(modem.write(new Uint8Array()), 0);

socket.receive(new Uint8Array([70, 73, 78]).buffer);
socket.peerClose();
assert.equal(modem.status(), state.OPEN, 'final payload is delivered before remote NO CARRIER');
assert.equal(modem.write(new Uint8Array([1])), 0);
assert.deepEqual([...modem.read(8)], [70, 73, 78]);
assert.equal(modem.status(), state.CLOSED);
modem.close();
assert.equal(modem.status(), state.CLOSED);

modem.dial('wss://fixture.invalid/old-call');
const old = sockets.at(-1);
const stale = { open: old.onopen, message: old.onmessage, close: old.onclose, error: old.onerror };
modem.dial('wss://fixture.invalid/new-call');
socket = sockets.at(-1);
assert.equal(old.closeCalls, 1, 'redial closes the previous attempt');
assert.equal(old.onmessage, null, 'old handlers release their references');
stale.open({});
stale.message({ data: new Uint8Array([99]).buffer });
stale.close({ code: 1013 });
stale.error({});
assert.equal(modem.status(), state.CONNECTING, 'late callbacks cannot change the new attempt');
assert.equal(modem.read(1), null, 'old-call bytes cannot reach the new call');
socket.open();
socket.receive(new Uint8Array([1, 2]));
modem.close();
assert.equal(modem.read(8), null, 'local hangup discards undelivered bytes');
assert.equal(modem.status(), state.CLOSED);

modem.dial('wss://fixture.invalid/overflow');
socket = sockets.at(-1);
socket.open();
socket.receive(new Uint8Array([1, 2, 3]));
socket.receive(new Uint8Array(6));
assert.equal(socket.closeCalls, 1, 'an overfull receive ring terminates the call');
assert.deepEqual([...modem.read(8)], [1, 2, 3], 'overflow cannot overwrite older bytes');
assert.equal(modem.status(), state.CLOSED);
modem.dial('wss://fixture.invalid/binary-only');
socket = sockets.at(-1);
socket.open();
socket.receive('text is not a telnet frame');
assert.equal(modem.status(), state.CLOSED, 'text frames must not be silently recoded');

modem.dial('wss://fixture.invalid/rejected');
sockets.at(-1).fail();
assert.equal(modem.status(), state.NO_ANSWER, 'connect failure is NO ANSWER');
modem.dial('wss://fixture.invalid/busy');
sockets.at(-1).peerClose(1013);
assert.equal(modem.status(), state.BUSY, 'a server asking to try later is BUSY');
modem.dial('wss://fixture.invalid/closed-before-open');
sockets.at(-1).peerClose();
assert.equal(modem.status(), state.NO_ANSWER);
modem.dial('wss://fixture.invalid/send-failure');
socket = sockets.at(-1);
socket.open();
socket.sendError = true;
assert.equal(modem.write(new Uint8Array([1])), 0);
assert.equal(modem.status(), state.CLOSED, 'send failure becomes carrier loss');
modem.dial('wss://fixture.invalid/live-error');
sockets.at(-1).open();
sockets.at(-1).fail();
assert.equal(modem.status(), state.CLOSED, 'an error after CONNECT is NO CARRIER');
modem.dial('wss://fixture.invalid/closing-before-event');
socket = sockets.at(-1);
socket.open();
socket.readyState = 2;
assert.equal(modem.write(new Uint8Array([1])), 0, 'closing sockets must not acknowledge discarded bytes');
assert.equal(modem.status(), state.CLOSED);

modem.dial('wss://fixture.invalid/timeout');
socket = sockets.at(-1);
clock = 29999;
assert.equal(modem.status(), state.CONNECTING);
clock = 30000;
assert.equal(modem.status(), state.NO_ANSWER, 'a stalled WebSocket cannot leave dialing forever');
assert.equal(socket.closeCalls, 1);
clock += 30000;
assert.equal(modem.status(), state.NO_ANSWER);
assert.equal(socket.closeCalls, 1, 'timeout cleanup is idempotent');

const unavailable = createModemTransport({ WebSocketClass: null });
assert.doesNotThrow(() => unavailable.dial('wss://fixture.invalid/'));
assert.equal(unavailable.status(), state.NO_ANSWER);
const blocked = createModemTransport({ WebSocketClass: class { constructor() { throw Error('blocked'); } } });
assert.doesNotThrow(() => blocked.dial('wss://fixture.invalid/'));
assert.equal(blocked.status(), state.NO_ANSWER);
assert.throws(() => createModemTransport({ bufferBytes: 0 }), RangeError);
assert.throws(() => createModemTransport({ bufferBytes: 1024 * 1024 }), RangeError);
console.log('web modem: dial-only binary sockets, raw bytes, bounded queues, failures and stale reconnects passed');
