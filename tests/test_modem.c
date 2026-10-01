/* The real modem, with a deterministic clock and partial/nonblocking fake
 * transport. No sockets, threads, browser, or real-time sleeps are needed. */
#include "modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

#define COM 0x3f8
#define BYTE_NS UINT64_C(694445)
#define UART_NS UINT64_C(173612) /* Divisor 2, 8N1: 57600 bps. */
#define GUARD_NS UINT64_C(1000000000)

static uint64_t now;
static enum ModemTransportState state, dial_state;
static uint8_t input[65536], output[65536], screen[65536];
static size_t input_size, input_at, output_size, screen_size;
static size_t read_limit, write_limit;
static unsigned dials, closes;
static char selected_env[128], selected_url[128];

static void fake_dial(const char *env, const char *url) {
    CHECK(strlen(env) < sizeof selected_env && strlen(url) < sizeof selected_url);
    strcpy(selected_env, env);
    strcpy(selected_url, url);
    ++dials;
    state = dial_state;
}
static enum ModemTransportState fake_status(void) { return state; }
static size_t fake_read(uint8_t *bytes, size_t count) {
    if (count > input_size - input_at) count = input_size - input_at;
    if (count > read_limit) count = read_limit;
    memcpy(bytes, input + input_at, count);
    input_at += count;
    return count;
}
static size_t fake_write(const uint8_t *bytes, size_t count) {
    if (count > write_limit) count = write_limit;
    CHECK(count <= sizeof output - output_size);
    memcpy(output + output_size, bytes, count);
    output_size += count;
    return count;
}
static void fake_close(void) { state = MODEM_TRANSPORT_CLOSED; ++closes; }
static const ModemTransport fake = {fake_dial, fake_status, fake_read, fake_write, fake_close};

static void incoming(const void *bytes, size_t count) {
    if (input_at == input_size) input_at = input_size = 0;
    CHECK(count <= sizeof input - input_size);
    memcpy(input + input_size, bytes, count);
    input_size += count;
}

static void capture(void) {
    while (modem_port_in(COM + 5) & 1) {
        CHECK(screen_size + 1 < sizeof screen);
        screen[screen_size++] = modem_port_in(COM);
        screen[screen_size] = 0;
        modem_tick(now);
    }
}

static void advance(uint64_t ns) { now += ns; modem_tick(now); }
static void pump(uint64_t ns) {
    uint64_t end = now + ns;
    while (now < end) {
        advance(end - now > 100000 ? 100000 : end - now);
        capture();
    }
}

static void empty_screen(void) { screen_size = 0; screen[0] = 0; }
static void expect(const char *text) {
    if (screen_size != strlen(text) || memcmp(screen, text, screen_size)) {
        fprintf(stderr, "expected [%s], got [%.*s] (%zu bytes)\n", text, (int)screen_size,
                screen, screen_size);
        exit(1);
    }
}

static void send_bytes(const void *data, size_t count) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < count; ++i) {
        unsigned spins = 0;
        while (!(modem_port_in(COM + 5) & 0x20)) {
            CHECK(++spins < 100);
            pump(50000);
        }
        modem_port_out(COM, bytes[i]);
    }
    unsigned spins = 0;
    while (!(modem_port_in(COM + 5) & 0x40)) {
        CHECK(++spins < 100);
        pump(50000);
    }
}

static void command(const char *text) {
    empty_screen();
    send_bytes(text, strlen(text));
    pump(50000000);
}

static void fresh(void) {
    modem_init(&fake);
    now = UINT64_C(2000000000);
    input_size = input_at = output_size = screen_size = 0;
    read_limit = write_limit = sizeof input;
    state = MODEM_TRANSPORT_CLOSED;
    dial_state = MODEM_TRANSPORT_OPEN;
    dials = closes = 0;
    selected_env[0] = selected_url[0] = 0;
    modem_tick(now);
    modem_port_out(COM + 3, 0x83);
    modem_port_out(COM, 2);
    modem_port_out(COM + 1, 0);
    modem_port_out(COM + 3, 3);
    modem_port_out(COM + 4, 0x0b);
    (void)modem_port_in(COM + 6);
    empty_screen();
}

static void silent(void) {
    fresh();
    command("ATE0\r");
    expect("ATE0\r\r\nOK\r\n");
    empty_screen();
}

static void online(void) {
    silent();
    command("ATDT 555-1992\r");
    expect("\r\nCONNECT 14400\r\n");
    CHECK(dials == 1 && state == MODEM_TRANSPORT_OPEN);
    CHECK(!strcmp(selected_env, "VC_MODEM_555_1992"));
    CHECK(!strcmp(selected_url, "wss://axis.tail85247.ts.net:8443/"));
    CHECK(modem_port_in(COM + 6) == 0xb8);
    CHECK(modem_port_in(COM + 6) == 0xb0);
    empty_screen();
}

static void one(uint8_t byte) {
    CHECK(modem_port_in(COM + 5) & 0x40);
    modem_port_out(COM, byte);
    advance(UART_NS);
    capture();
}

static void escaped(void) {
    pump(GUARD_NS + 1);
    one('+'); one('+'); one('+');
    pump(GUARD_NS + 10000000);
    expect("\r\nOK\r\n");
    empty_screen();
}

static void test_uart(void) {
    fresh();
    CHECK(modem_port_in(COM + 5) == 0x60);
    CHECK(modem_port_in(COM + 2) == 1);
    CHECK(modem_port_in(COM + 6) == 0x30);
    modem_port_out(COM + 1, 0xfd);
    CHECK(modem_port_in(COM + 1) == 13);
    modem_port_out(COM + 3, 0x83);
    modem_port_out(COM, 0x34);
    modem_port_out(COM + 1, 0x12);
    CHECK(modem_port_in(COM) == 0x34 && modem_port_in(COM + 1) == 0x12);
    modem_port_out(COM + 3, 3);
    CHECK(modem_port_in(COM + 1) == 13);
    modem_port_out(COM + 7, 0xa5);
    CHECK(modem_port_in(COM + 7) == 0xa5);
    modem_port_out(COM + 2, 0x87); /* Kermit's 16550A FIFO probe. */
    CHECK((modem_port_in(COM + 2) & 0xf0) == 0);
    modem_port_out(COM + 5, 0xff);
    modem_port_out(COM + 6, 0xff);
    CHECK(modem_port_in(COM + 5) == 0x60 && modem_port_in(COM + 6) == 0x30);
    CHECK(modem_port_in(0x2f8) == 0xff);

    fresh();
    modem_port_out(COM + 4, 3); /* OUT2 is a physical interrupt gate. */
    modem_port_out(COM + 1, 2);
    CHECK(!modem_irq_pending());
    modem_port_out(COM + 4, 0x0b);
    CHECK(modem_irq_pending());
    CHECK(modem_port_in(COM + 2) == 2); /* IIR read acknowledges THRE. */
    CHECK(!modem_irq_pending() && modem_port_in(COM + 2) == 1);
    modem_port_out(COM + 1, 2);
    CHECK(modem_port_in(COM + 2) == 1); /* No new enable edge. */
    modem_port_out(COM + 1, 0);
    modem_port_out(COM + 1, 2);
    CHECK(modem_port_in(COM + 2) == 2);

    fresh();
    modem_port_out(COM + 4, 0x18); /* Internal loopback, OUT2 -> DCD. */
    (void)modem_port_in(COM + 6);
    modem_port_out(COM + 1, 15);
    CHECK(modem_port_in(COM + 2) == 2);
    modem_port_out(COM, 'A');
    CHECK(modem_port_in(COM + 5) == 0x20); /* THR free; shifter occupied. */
    modem_port_out(COM, 'B');
    CHECK(modem_port_in(COM + 5) == 0); /* Exactly one holding byte. */
    advance(UART_NS);
    CHECK(modem_port_in(COM + 2) == 4);
    advance(UART_NS);
    modem_port_out(COM + 4, 0x1b); /* CTS/DSR deltas below all other causes. */
    CHECK(modem_port_in(COM + 2) == 6 && modem_port_in(COM + 2) == 6);
    CHECK(modem_port_in(COM + 5) == 0x63); /* OE, DR, both TX empties. */
    CHECK(modem_port_in(COM + 2) == 4 && modem_port_in(COM + 2) == 4);
    CHECK(modem_port_in(COM) == 'A'); /* Overrun did not overwrite RBR. */
    CHECK(modem_port_in(COM + 2) == 2);
    CHECK(modem_port_in(COM + 2) == 0);
    CHECK(modem_port_in(COM + 6) == 0xb3);
    CHECK(modem_port_in(COM + 6) == 0xb0);
    CHECK(modem_port_in(COM + 2) == 1 && !modem_irq_pending());
    CHECK(!(modem_port_in(COM + 5) & 1));

    fresh();
    modem_port_out(COM + 4, 0x14); /* RI's rising edge has no delta. */
    CHECK(modem_port_in(COM + 6) == 0x43);
    CHECK(modem_port_in(COM + 6) == 0x40);
    modem_port_out(COM + 4, 0x10);
    CHECK(modem_port_in(COM + 6) == 0x04);
    modem_port_out(COM + 4, 0x18);
    CHECK(modem_port_in(COM + 6) == 0x88);
    modem_port_out(COM + 4, 0x10);
    CHECK(modem_port_in(COM + 6) == 0x08);
    modem_port_out(COM, 'A');
    modem_port_out(COM, 'B');
    modem_port_out(COM, 'C'); /* A non-FIFO THR write replaces its old byte. */
    advance(UART_NS);
    CHECK(modem_port_in(COM) == 'A');
    advance(UART_NS);
    CHECK(modem_port_in(COM) == 'C');
    modem_port_out(COM + 3, 0x43); /* A looped-back break sets BI and FE. */
    modem_port_out(COM, 'x');
    advance(UART_NS);
    CHECK(modem_port_in(COM + 5) == 0x79);
    CHECK(modem_port_in(COM + 5) == 0x61);
    CHECK(modem_port_in(COM) == 0);
}

static void test_bios(void) {
    static const unsigned divisors[] = {1047, 768, 384, 192, 96, 48, 24, 12};
    fresh();
    for (unsigned baud = 0; baud < 8; ++baud) {
        uint8_t settings = (uint8_t)((baud << 5) | 0x1e);
        CHECK(modem_bios(0, settings, 0) == 0x6030);
        CHECK(modem_port_in(COM + 3) == 0x1e);
        CHECK(modem_port_in(COM + 1) == 0);
        CHECK(modem_port_in(COM + 4) == 3);
        modem_port_out(COM + 3, 0x80);
        unsigned divisor = modem_port_in(COM) | (unsigned)modem_port_in(COM + 1) << 8;
        CHECK(divisor == divisors[baud]);
    }
    CHECK(modem_bios(0, 0xe3, 0) == 0x6030);
    modem_port_out(COM + 4, 0x13);
    (void)modem_port_in(COM + 6);
    CHECK((modem_bios(1, 'A', 0) & 0x80ff) == 'A');
    CHECK((modem_bios(1, 'B', 0) & 0x80ff) == 'B');
    CHECK(modem_bios(1, 'C', 0) & 0x8000);
    CHECK(modem_bios(2, 0, 0) & 0x8000);
    advance(1041667);
    CHECK((modem_bios(3, 0, 0) & 0x0130) == 0x0130);
    CHECK(modem_bios(2, 0, 0) == 0x2141);
    CHECK(!(modem_port_in(COM + 5) & 1));
    advance(1041667);
    CHECK(modem_bios(2, 0, 0) == 0x6142);
    CHECK(modem_bios(2, 0, 0) == 0xe000);
    for (unsigned fn = 0; fn < 5; ++fn) CHECK(modem_bios(fn, 0x55, 1) == 0x8055);
    CHECK(modem_bios(4, 0x55, 0) == 0x8055);
}

static void test_hayes(void) {
    fresh();
    command("AT\r"); expect("AT\r\r\nOK\r\n");
    command("at e0 v1 q0 m0\r"); expect("at e0 v1 q0 m0\r\r\nOK\r\n");
    command("AT\r"); expect("\r\nOK\r\n");
    command("ATE1\r"); expect("\r\nOK\r\n");
    command("AT\r"); expect("AT\r\r\nOK\r\n");
    command("ATE0V0\r"); expect("ATE0V0\r0\r");
    command("AT\r"); expect("0\r");
    command("ATI\r"); expect("\r\nnotanemulator 14400 modem\r\n0\r");
    command("ATX\r"); expect("4\r");
    command("ATE2\r"); expect("4\r");
    command("ATV2\r"); expect("4\r");
    command("ATQ2\r"); expect("4\r");
    command("ATM2\r"); expect("4\r");
    command("ATM1\r"); expect("0\r");
    command("ATH\r"); expect("0\r");
    command("ATO\r"); expect("3\r");
    command("ATA\r"); expect("3\r");
    command("ATD\r"); expect("8\r");
    command("ATDP1234\r"); expect("8\r");
    command("ATQ1\r"); expect("");
    command("AT\r"); expect("");
    command("ATI\r"); expect("\r\nnotanemulator 14400 modem\r\n");
    command("ATQ0\r"); expect("0\r");
    command("ATV1\r"); expect("\r\nOK\r\n");
    command("ATI0\r"); expect("\r\nnotanemulator 14400 modem\r\n\r\nOK\r\n");
    command("ATA\r"); expect("\r\nNO CARRIER\r\n");
    command("ATO\r"); expect("\r\nNO CARRIER\r\n");
    command("ATH0\r"); expect("\r\nOK\r\n");
    command("ATDT000\r"); expect("\r\nNO ANSWER\r\n");
    CHECK(dials == 0);
    command("garbage\r"); expect("\r\nERROR\r\n");
    command("ATH2\r"); expect("\r\nERROR\r\n");
    command("ATO2\r"); expect("\r\nERROR\r\n");
    command("ATI2\r"); expect("\r\nERROR\r\n");
    command("ATZ2\r"); expect("\r\nERROR\r\n");
    command("ATAX\r"); expect("\r\nERROR\r\n");
    command("AT\bT\r\n"); expect("\r\nOK\r\n");
    command("ATZ\r"); expect("\r\nOK\r\n");
    command("AT\r"); expect("AT\r\r\nOK\r\n");

    const char *const forms[] = {"ATD555-1992\r", "ATDT5551992\r", "ATDP(555)-1992\r"};
    for (size_t i = 0; i < sizeof forms / sizeof *forms; ++i) {
        silent(); command(forms[i]); expect("\r\nCONNECT 14400\r\n");
        CHECK(dials == 1);
    }
    for (unsigned numeric = 0; numeric < 2; ++numeric) {
        silent();
        if (numeric) command("ATV0\r");
        dial_state = MODEM_TRANSPORT_BUSY;
        command("ATDT555-1992\r"); expect(numeric ? "7\r" : "\r\nBUSY\r\n");
        dial_state = MODEM_TRANSPORT_NO_ANSWER;
        command("ATDT555-1992\r"); expect(numeric ? "8\r" : "\r\nNO ANSWER\r\n");
        dial_state = MODEM_TRANSPORT_CLOSED;
        command("ATDT555-1992\r"); expect(numeric ? "8\r" : "\r\nNO ANSWER\r\n");
        dial_state = MODEM_TRANSPORT_CONNECTING;
        command("ATDT555-1992\r"); expect("");
        advance(UINT64_C(29000000000)); capture(); expect("");
        advance(UINT64_C(1000000000)); pump(20000000);
        expect(numeric ? "8\r" : "\r\nNO ANSWER\r\n");
        dial_state = MODEM_TRANSPORT_OPEN;
        command("ATDT555-1992\r"); expect(numeric ? "13\r" : "\r\nCONNECT 14400\r\n");
        empty_screen();
        modem_port_out(COM + 4, 8); /* DTR drops the carrier and closes once. */
        pump(20000000); expect(numeric ? "3\r" : "\r\nNO CARRIER\r\n");
        CHECK((modem_port_in(COM + 6) & 0x88) == 8);
        CHECK(!(modem_port_in(COM + 6) & 15));
    }
    online(); escaped();
    command("ATD555-1992\r"); expect("\r\nBUSY\r\n"); CHECK(dials == 1);
    command("ATO0\r"); expect("\r\nCONNECT 14400\r\n");
    empty_screen(); escaped();
    command("ATZ0\r"); expect("\r\nOK\r\n");
    CHECK(state == MODEM_TRANSPORT_CLOSED && !(modem_port_in(COM + 6) & 0x80));
    online();
    state = MODEM_TRANSPORT_CLOSED;
    pump(20000000); expect("\r\nNO CARRIER\r\n");
    unsigned closed = closes;
    pump(20000000); CHECK(closes == closed); /* Only one loss-of-carrier result. */
    online();
    incoming("GOODBYE", 7);
    advance(1000000); /* Transport buffered its last bytes before seeing EOF. */
    state = MODEM_TRANSPORT_CLOSED;
    pump(1000000);
    CHECK(!(modem_port_in(COM + 6) & 0x80)); /* Carrier drops without delay. */
    pump(20000000);
    expect("GOODBYE\r\nNO CARRIER\r\n"); /* EOF must not truncate the final chunk. */
    online();
    uint8_t final_chunk[800]; memset(final_chunk, 'x', sizeof final_chunk);
    incoming(final_chunk, sizeof final_chunk);
    advance(2000000);
    state = MODEM_TRANSPORT_CLOSED;
    advance(1000000); capture();
    command("ATDT555-1992\r");
    CHECK(strstr((char *)screen, "CONNECT 14400") != NULL);
    CHECK(strstr((char *)screen, "NO CARRIER") == NULL); /* New dial supersedes the EOF drain. */
    silent();
    dial_state = MODEM_TRANSPORT_CONNECTING;
    command("ATDT555-1992\r"); expect("");
    command("x"); expect("\r\nNO CARRIER\r\n"); /* Abort a pending dial. */
}

static void test_escape(void) {
    online();
    one('x'); pump(2000000);
    pump(GUARD_NS - 10000000);
    one('+'); one('+'); one('+');
    pump(GUARD_NS + 10000000);
    CHECK(output_size == 4 && !memcmp(output, "x+++", 4));
    expect(""); /* Missing the leading guard cannot escape. */

    online(); pump(GUARD_NS + 1);
    one('+'); one('+'); one('+');
    uint64_t third = now;
    advance(GUARD_NS - 1); capture();
    CHECK(now == third + GUARD_NS - 1 && output_size == 0); expect("");
    advance(1); pump(10000000); expect("\r\nOK\r\n");
    CHECK(output_size == 0); /* A successful escape never sends pluses. */
    command("ATH\r"); expect("\r\nNO CARRIER\r\n");
    CHECK(state == MODEM_TRANSPORT_CLOSED && closes == 1);

    online(); pump(GUARD_NS + 1);
    one('+'); one('+'); one('+');
    pump(GUARD_NS / 2); one('x'); pump(GUARD_NS);
    CHECK(output_size == 4 && !memcmp(output, "+++x", 4)); expect("");

    online(); pump(GUARD_NS + 1);
    one('+'); one('+');
    pump(GUARD_NS + 10000000);
    CHECK(output_size == 2 && !memcmp(output, "++", 2)); expect("");

    online(); pump(GUARD_NS + 1);
    one('+'); one('+'); one('+'); one('+');
    pump(GUARD_NS + 10000000);
    CHECK(output_size == 4 && !memcmp(output, "++++", 4)); expect("");

    online(); pump(GUARD_NS + 1);
    one('+'); pump(GUARD_NS + 10000000);
    CHECK(output_size == 1 && output[0] == '+'); expect("");
    one('+'); one('+'); one('+');
    incoming("R", 1); pump(GUARD_NS + 10000000);
    expect("R\r\nOK\r\n"); /* Incoming remote data does not break DTE silence. */
}

static size_t fixture(uint8_t *bytes, size_t capacity) {
    FILE *file = fopen("tests/fixtures/enigma-connect-2026-10-02.bin", "rb");
    CHECK(file);
    size_t count = fread(bytes, 1, capacity, file);
    CHECK(!ferror(file) && feof(file));
    CHECK(fclose(file) == 0);
    CHECK(count == 5433);
    return count;
}

static void test_telnet(void) {
    static const uint8_t replies[] = {
        255, 253, 1, /* WILL ECHO -> DO; initial DONT ECHO needs no reply. */
        255, 253, 3, 255, 251, 3,
        255, 251, 0, 255, 253, 0,
        255, 251, 24,
        255, 251, 31, 255, 250, 31, 0, 80, 0, 25, 255, 240,
        255, 252, 39,
    };
    uint8_t bytes[8192];
    size_t count = fixture(bytes, sizeof bytes);
    const size_t fragments[] = {1, 2, 3, 17, 256};
    for (size_t f = 0; f < sizeof fragments / sizeof *fragments; ++f) {
        online();
        read_limit = fragments[f];
        write_limit = 2; /* The exact reply stream survives partial writes. */
        incoming(bytes, count);
        pump(UINT64_C(5000000000));
        CHECK(input_at == count);
        CHECK(screen_size == count - 27 && !memcmp(screen, bytes + 27, count - 27));
        CHECK(output_size == sizeof replies && !memcmp(output, replies, sizeof replies));
        CHECK(strstr((char *)screen, "BBS version") != NULL);
    }
    output_size = 0; empty_screen();
    static const uint8_t request[] = {255, 250, 24, 1, 255, 240};
    static const uint8_t name[] = {255, 250, 24, 0, 'A', 'N', 'S', 'I', 255, 240};
    read_limit = write_limit = 1;
    incoming(request, sizeof request); pump(10000000);
    CHECK(output_size == sizeof name && !memcmp(output, name, sizeof name));
    expect("");

    output_size = 0;
    static const uint8_t repeated[] = {255, 253, 31, 255, 251, 1, 255, 253, 39};
    incoming(repeated, sizeof repeated); pump(10000000);
    CHECK(output_size == 0); /* Repeated negotiations cannot create a loop. */
    static const uint8_t other[] = {
        255, 251, 42, 255, 253, 42, 255, 253, 1,
        255, 254, 31, 255, 252, 1,
    };
    static const uint8_t refused[] = {
        255, 254, 42, 255, 252, 42, 255, 252, 1,
        255, 252, 31, 255, 254, 1,
    };
    incoming(other, sizeof other); pump(20000000);
    CHECK(output_size == sizeof refused && !memcmp(output, refused, sizeof refused));
    output_size = 0;
    static const uint8_t escaped_iac[] = {'A', 255, 255, 'B'};
    incoming(escaped_iac, sizeof escaped_iac); pump(10000000);
    CHECK(screen_size == 3 && screen[0] == 'A' && screen[1] == 255 && screen[2] == 'B');
    uint8_t ff = 255;
    send_bytes(&ff, 1); pump(10000000);
    CHECK(output_size == 2 && output[0] == 255 && output[1] == 255);

    empty_screen(); output_size = 0;
    uint8_t oversized[80] = {255, 250, 24};
    memset(oversized + 3, 1, 73);
    oversized[76] = 255; oversized[77] = 240; oversized[78] = 'O'; oversized[79] = 'K';
    incoming(oversized, sizeof oversized); incoming(request, sizeof request);
    pump(50000000);
    expect("OK");
    CHECK(output_size == sizeof name && !memcmp(output, name, sizeof name));
}

static void test_pacing(void) {
    online();
    one('A');
    advance(BYTE_NS - 1);
    CHECK(output_size == 0);
    advance(1);
    CHECK(output_size == 1 && output[0] == 'A');

    online();
    incoming("A", 1);
    advance(1000000); /* Poll obtains a byte, which must still cross the line. */
    CHECK(!(modem_port_in(COM + 5) & 1));
    advance(BYTE_NS - 1);
    CHECK(!(modem_port_in(COM + 5) & 1));
    advance(1);
    CHECK(modem_port_in(COM + 5) & 1);
    CHECK(modem_port_in(COM) == 'A' && !(modem_port_in(COM + 5) & 1));

    online();
    uint8_t bulk[1000]; memset(bulk, 'x', sizeof bulk);
    incoming(bulk, sizeof bulk);
    pump(100000000);
    CHECK(screen_size >= 135 && screen_size <= 145); /* ~1440 bytes/second. */
    pump(600000000);
    CHECK(screen_size == sizeof bulk);

    online();
    uint64_t start = now;
    send_bytes(bulk, sizeof bulk); /* UART is 57600; the modem link is 14400. */
    CHECK(output_size <= (now - start) / BYTE_NS + 1);
    CHECK(output_size > 230 && output_size < 260);
    pump(600000000);
    CHECK(output_size == sizeof bulk && !memcmp(output, bulk, sizeof bulk));
}

static void test_bounds(void) {
    silent();
    char long_command[300];
    memset(long_command, 'A', sizeof long_command);
    long_command[sizeof long_command - 2] = '\r';
    long_command[sizeof long_command - 1] = 0;
    command(long_command); expect("\r\nERROR\r\n");
    command("AT\r"); expect("\r\nOK\r\n"); /* Parser resynchronizes at CR. */

    online();
    uint8_t bulk[20000]; memset(bulk, 'x', sizeof bulk);
    incoming(bulk, sizeof bulk);
    for (unsigned i = 0; i < 100; ++i) advance(1000000); /* Do not read RBR. */
    CHECK(input_at < sizeof bulk); /* Bounded modem buffer backpressures TCP. */
    CHECK((modem_port_in(COM + 5) & 3) == 1); /* No fictitious host-pause overrun. */
    capture();
    pump(UINT64_C(15000000000));
    CHECK(screen_size == sizeof bulk && !memcmp(screen, bulk, sizeof bulk));

    online();
    write_limit = 0;
    uint8_t repeated[18000];
    for (size_t i = 0; i < sizeof repeated; i += 6) {
        repeated[i] = 255; repeated[i + 1] = 253; repeated[i + 2] = 31;
        repeated[i + 3] = 255; repeated[i + 4] = 254; repeated[i + 5] = 31;
    }
    incoming(repeated, sizeof repeated);
    pump(100000000);
    CHECK(input_at < sizeof repeated && output_size == 0);
    write_limit = 7;
    pump(2000000000);
    CHECK(input_at == sizeof repeated && output_size == 45000);
    CHECK(state == MODEM_TRANSPORT_OPEN && screen_size == 0);
    modem_reset();
    CHECK(state == MODEM_TRANSPORT_CLOSED && modem_port_in(COM + 5) == 0x60);
}

static void test_door(void) {
    silent();
    modem_set_door(1);
    command("ATDT555-1992\r"); expect("\r\nNO ANSWER\r\n");
    CHECK(dials == 0);
    command("ATDT000\r"); expect("\r\nNO ANSWER\r\n");
    CHECK(dials == 0);
    /* A guest ATZ must not restore the original phone book. */
    command("ATZ\r");
    command("ATE0\r");
    command("ATDP5551992\r"); expect("\r\nNO ANSWER\r\n");
    CHECK(dials == 0);
    modem_set_door(0);
    command("ATDT555-1992\r");
    CHECK(dials == 1); /* Normal mode retains the existing endpoint. */
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } tests[] = {
        {"uart", test_uart}, {"bios", test_bios}, {"hayes", test_hayes},
        {"escape", test_escape}, {"telnet", test_telnet},
        {"pacing", test_pacing}, {"bounds", test_bounds}, {"door", test_door},
    };
    unsigned ran = 0;
    for (size_t i = 0; i < sizeof tests / sizeof *tests; ++i) {
        if (argc == 1 || !strcmp(argv[1], tests[i].name)) {
            tests[i].run();
            printf("modem %s: passed\n", tests[i].name);
            ++ran;
        }
    }
    CHECK(ran);
    return 0;
}
