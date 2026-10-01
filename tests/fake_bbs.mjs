// A local double of a telnet BBS behind a binary WebSocket. The production
// browser adapter, C modem, UART and translated Kermit are not replaced.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const capture = readFileSync(new URL('./fixtures/enigma-connect-2026-10-02.bin', import.meta.url));
export const fixtureReplies = Uint8Array.from([
  255, 253, 1, 255, 253, 3, 255, 251, 3, 255, 251, 0,
  255, 253, 0, 255, 251, 24, 255, 251, 31,
  255, 250, 31, 0, 80, 0, 25, 255, 240, 255, 252, 39,
]);

export function fakeBBS() {
  const calls = [];
  class FakeWebSocket {
    readyState = 0;
    bufferedAmount = 0;
    binaryType = 'blob';
    input = [];
    line = [];
    replies = [];
    raw = [];
    closed = false;
    telnet = 0;
    terminal = 0;

    constructor(url) {
      assert.equal(url, 'wss://axis.tail85247.ts.net:8443/', 'dial uses the phonebook endpoint');
      calls.push(this);
      queueMicrotask(() => {
        if (this.closed) return;
        assert.equal(this.binaryType, 'arraybuffer', 'the page requests binary frames');
        this.readyState = 1;
        this.onopen?.({});
        // Split IAC sequences across frames, just as a real stream can.
        for (const part of [capture.subarray(0, 1), capture.subarray(1, 17), capture.subarray(17)])
          this.deliver(part);
        // The unchanged capture clears its welcome art. Put later echoed
        // input in a separate visible area after the original bytes.
        this.deliver(Buffer.from('\x1b[20;1H\x1b[0m\x1b[J\r\nTEST BBS READY\r\n'));
      });
    }

    deliver(bytes) {
      if (this.closed) return;
      this.onmessage?.({ data: Uint8Array.from(bytes).buffer });
    }

    application(byte, echoed) {
      // A BBS consumes DA/DSR feature reports instead of echoing them back
      // as fresh terminal queries. Human input is echoed without local echo.
      if (this.terminal === 1) {
        this.terminal = byte === 91 ? 2 : 0;
        return;
      }
      if (this.terminal === 2) {
        if (byte >= 0x40 && byte <= 0x7e) this.terminal = 0;
        return;
      }
      if (byte === 27) { this.terminal = 1; return; }
      this.input.push(byte);
      if (byte === 13) {
        echoed.push(...Buffer.from('\r\nBBS ECHO: '), ...this.line, 13, 10);
        this.line = [];
      } else this.line.push(byte);
    }

    send(bytes) {
      assert.ok(ArrayBuffer.isView(bytes), 'WebSocket writes are binary');
      assert.equal(this.readyState, 1);
      const echoed = [];
      for (const byte of bytes) {
        this.raw.push(byte);
        if (this.telnet === 0) {
          if (byte === 255) this.telnet = 1;
          else this.application(byte, echoed);
        } else if (this.telnet === 1) {
          if (byte === 255) { this.application(255, echoed); this.telnet = 0; }
          else {
            this.replies.push(255, byte);
            this.telnet = byte === 250 ? 3 : (byte >= 251 && byte <= 254 ? 2 : 0);
          }
        } else if (this.telnet === 2) {
          this.replies.push(byte);
          this.telnet = 0;
        } else {
          this.replies.push(byte);
          if (this.telnet === 4 && byte === 240) this.telnet = 0;
          else this.telnet = byte === 255 && this.telnet === 3 ? 4 : 3;
        }
      }
      if (echoed.length) queueMicrotask(() => this.deliver(echoed));
    }

    close() {
      this.closed = true;
      this.readyState = 3;
      this.onclose?.({ code: 1000 });
    }

    get text() { return Buffer.from(this.input).toString('latin1'); }
  }
  return { calls, WebSocketClass: FakeWebSocket };
}
