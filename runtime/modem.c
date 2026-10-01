/* A 16450, not a FIFO UART: MS-DOS Kermit probes FCR/IIR, enables receive
 * interrupts, drains LSR/RBR, and uses MCR's DTR/RTS/OUT2 and MSR's leads.
 * See unmodified msxibm.asm: chkport, serini, uartsnd, serint and serhng.
 * mssser.asm implements Kermit's file server, not the serial hardware. */
#include "modem.h"

#include <string.h>

enum {
    COM1 = 0x3f8, QUEUE_SIZE = 8192, COMMAND_SIZE = 128,
    DR = 0x01, OE = 0x02, THRE = 0x20, TEMT = 0x40,
    DTR = 0x01, RTS = 0x02, OUT1 = 0x04, OUT2 = 0x08, LOOP = 0x10,
    CTS = 0x10, DSR = 0x20, RI = 0x40, DCD = 0x80,
    IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251,
    SB = 250, SE = 240, BINARY = 0, ECHO = 1, SGA = 3,
    TTYPE = 24, NAWS = 31,
};

/* Ten line bits per byte at 14400 bps. Ceiling division keeps the promised
 * rate a ceiling, with less than one part per million of rounding error. */
#define BYTE_NS UINT64_C(694445)
#define GUARD_NS UINT64_C(1000000000)
#define DIAL_NS UINT64_C(30000000000)
#define POLL_NS UINT64_C(1000000)

typedef struct Queue {
    uint8_t bytes[QUEUE_SIZE];
    size_t head, count;
} Queue;

typedef struct Phone {
    const char *number, *env_key, *web_url;
} Phone;

/* The only phone book. Transport adapters receive the selected data, never
 * their own copy of the dialled number or endpoint. Numbers omit punctuation. */
static const Phone phone_book[] = {
    {"5551992", "VC_MODEM_555_1992", "wss://axis.tail85247.ts.net:8443/"},
};

static struct {
    ModemTransport host;
    uint64_t now, shift_done, rx_due, tx_due, last_input, dial_started, poll_due;
    uint8_t ier, lcr, mcr, dll, dlm, scratch;
    uint8_t rbr, thr, shift, errors, msr, deltas;
    unsigned rbr_full, thr_full, shift_full, thre_irq;
    unsigned echo, verbose, quiet, speaker, command, connected, dialing, remote_eof;
    unsigned pluses, command_overflow;
    char line[COMMAND_SIZE];
    size_t line_length;
    Queue result, remote, outgoing, wire;
    unsigned telnet_state, telnet_verb, sb_option, sb_length, sb_overflow;
    uint8_t sb_data[32], local_option[256], remote_option[256];
} m;

static size_t room(const Queue *q) { return QUEUE_SIZE - q->count; }

static int put(Queue *q, uint8_t byte) {
    if (!room(q)) return 0;
    q->bytes[(q->head + q->count) % QUEUE_SIZE] = byte;
    ++q->count;
    return 1;
}

static uint8_t get(Queue *q) {
    uint8_t byte = q->bytes[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    --q->count;
    return byte;
}

static void clear(Queue *q) { q->head = q->count = 0; }

static void leads(void) {
    uint8_t next;
    if (m.mcr & LOOP) {
        next = ((m.mcr & RTS) ? CTS : 0) | ((m.mcr & DTR) ? DSR : 0)
            | ((m.mcr & OUT1) ? RI : 0) | ((m.mcr & OUT2) ? DCD : 0);
    } else {
        /* A powered modem is ready to accept AT commands without a carrier.
         * CTS reflects its finite transmit/command buffers; DCD is the call. */
        next = DSR | (m.connected ? DCD : 0);
        if (room(&m.outgoing) >= 16 && room(&m.wire) >= 32
                && room(&m.result) >= COMMAND_SIZE * 2) next |= CTS;
    }
    uint8_t changed = next ^ m.msr;
    m.deltas |= (changed & CTS) >> 4;
    m.deltas |= (changed & DSR) >> 4;
    m.deltas |= (changed & DCD) >> 4;
    if ((m.msr & RI) && !(next & RI)) m.deltas |= 4; /* Trailing edge only. */
    m.msr = next;
}

static uint8_t lsr(void) {
    return m.errors | (m.rbr_full ? DR : 0) | (!m.thr_full ? THRE : 0)
        | ((!m.thr_full && !m.shift_full) ? TEMT : 0);
}

static uint8_t iir(void) {
    if ((m.ier & 4) && m.errors) return 6; /* Receiver line status. */
    if ((m.ier & 1) && m.rbr_full) return 4;
    if ((m.ier & 2) && m.thre_irq) return 2;
    if ((m.ier & 8) && m.deltas) return 0;
    return 1;
}

static void receive_byte(uint8_t byte) {
    if (m.rbr_full) m.errors |= OE; /* Unread RBR survives an overrun. */
    else { m.rbr = byte; m.rbr_full = 1; }
}

static void answer_text(const char *text) {
    if (!m.result.count && !m.remote.count && !m.rbr_full)
        m.rx_due = m.now + BYTE_NS;
    while (*text) (void)put(&m.result, (uint8_t)*text++);
}

enum Result { OK, CONNECT, NO_CARRIER, ERROR, BUSY, NO_ANSWER };

static void answer(enum Result result) {
    static const char *const words[] = {
        "OK", "CONNECT 14400", "NO CARRIER", "ERROR", "BUSY", "NO ANSWER",
    };
    static const char *const codes[] = {"0\r", "13\r", "3\r", "4\r", "7\r", "8\r"};
    if (m.quiet) return;
    if (!m.verbose) { answer_text(codes[result]); return; }
    answer_text("\r\n");
    answer_text(words[result]);
    answer_text("\r\n");
}

static void telnet_reset(void) {
    m.telnet_state = m.telnet_verb = m.sb_option = m.sb_length = 0;
    m.sb_overflow = 0;
    memset(m.local_option, 0, sizeof m.local_option);
    memset(m.remote_option, 0, sizeof m.remote_option);
}

static void disconnect(int report) {
    int active = m.connected || m.dialing;
    if (m.host.close) m.host.close();
    m.connected = m.dialing = m.pluses = m.remote_eof = 0;
    m.command = 1;
    clear(&m.remote);
    clear(&m.outgoing);
    clear(&m.wire);
    telnet_reset();
    leads();
    if (report && active) answer(NO_CARRIER);
}

static void peer_closed(void) {
    /* TCP/WebSocket EOF follows the last transport bytes, not the last byte
     * the much slower modem has delivered to the UART. Lower DCD now, then
     * drain that last chunk before reporting NO CARRIER. An escaped modem
     * was already withholding remote data from command mode, so discard it. */
    if (m.command) clear(&m.remote);
    if (m.host.close) m.host.close();
    m.connected = m.dialing = m.pluses = 0;
    m.command = m.remote_eof = 1;
    clear(&m.outgoing);
    clear(&m.wire);
    telnet_reset();
    leads();
}

static void profile(void) {
    m.echo = m.verbose = m.speaker = 1;
    m.quiet = 0;
}

void modem_reset(void) {
    ModemTransport host = m.host;
    uint64_t now = m.now;
    if (m.host.close) m.host.close();
    memset(&m, 0, sizeof m);
    m.host = host;
    m.now = m.last_input = now;
    m.dll = 12; /* 9600 baud until BIOS or the program programs the latch. */
    m.lcr = 3; /* Eight data bits, one stop bit, no parity. */
    m.command = 1;
    profile();
    leads();
    m.deltas = 0;
}

void modem_init(const ModemTransport *transport) {
    if (m.host.close) m.host.close();
    memset(&m, 0, sizeof m);
    if (transport) m.host = *transport;
    modem_reset();
}

static void dial(const char *number) {
    if (m.connected || m.dialing) { answer(BUSY); return; }
    char digits[COMMAND_SIZE];
    size_t count = 0;
    for (; *number; ++number) {
        if (*number >= '0' && *number <= '9') digits[count++] = *number;
        else if (*number != '-' && *number != '(' && *number != ')' && *number != ' ')
            { answer(NO_ANSWER); return; }
    }
    digits[count] = 0;
    const Phone *phone = NULL;
    for (size_t i = 0; i < sizeof phone_book / sizeof *phone_book; ++i)
        if (!strcmp(phone_book[i].number, digits)) phone = phone_book + i;
    if (!phone || !m.host.dial || !m.host.status) { answer(NO_ANSWER); return; }
    m.remote_eof = 0;
    clear(&m.remote);
    clear(&m.outgoing);
    clear(&m.wire);
    telnet_reset();
    m.dialing = 1;
    m.dial_started = m.now;
    m.poll_due = m.now;
    m.host.dial(phone->env_key, phone->web_url);
}

static int parameter(char **p) {
    if (**p >= '0' && **p <= '9') return *(*p)++ - '0';
    return 0;
}

static void execute(void) {
    char compact[COMMAND_SIZE];
    size_t length = 0;
    for (size_t i = 0; i < m.line_length; ++i) {
        unsigned char byte = (unsigned char)m.line[i];
        if (byte == ' ' || byte == '\t') continue;
        compact[length++] = byte >= 'a' && byte <= 'z' ? (char)(byte - 32) : (char)byte;
    }
    compact[length] = 0;
    if (m.command_overflow) { answer(ERROR); return; }
    if (!length) return;
    if (length < 2 || compact[0] != 'A' || compact[1] != 'T') { answer(ERROR); return; }
    char *p = compact + 2;
    while (*p) {
        int value;
        switch (*p++) {
        case 'E':
        case 'V':
        case 'Q':
        case 'M': {
            char option = p[-1];
            value = parameter(&p);
            if (value > 1) { answer(ERROR); return; }
            if (option == 'E') m.echo = (unsigned)value;
            if (option == 'V') m.verbose = (unsigned)value;
            if (option == 'Q') m.quiet = (unsigned)value;
            if (option == 'M') m.speaker = (unsigned)value;
            break;
        }
        case 'Z':
            if (parameter(&p)) { answer(ERROR); return; }
            disconnect(0);
            profile();
            break;
        case 'I':
            if (parameter(&p)) { answer(ERROR); return; }
            answer_text("\r\nnotanemulator 14400 modem\r\n");
            break;
        case 'H': {
            if (parameter(&p)) { answer(ERROR); return; }
            int active = m.connected || m.dialing;
            disconnect(0);
            answer(active ? NO_CARRIER : OK);
            return;
        }
        case 'O':
            if (parameter(&p) || *p) { answer(ERROR); return; }
            if (m.connected) {
                m.command = 0;
                m.last_input = m.now;
                m.rx_due = m.now + BYTE_NS;
                answer(CONNECT);
            } else answer(NO_CARRIER);
            return;
        case 'A':
            if (*p) answer(ERROR);
            else answer(NO_CARRIER);
            return;
        case 'D':
            if (*p == 'T' || *p == 'P') ++p;
            dial(p);
            return;
        default: answer(ERROR); return;
        }
    }
    answer(OK);
}

static void data_byte(uint8_t byte, uint64_t at) {
    if (!m.outgoing.count) m.tx_due = at + BYTE_NS;
    if (!put(&m.outgoing, byte)) disconnect(1); /* DTE ignored CTS. */
}

static void flush_pluses(uint64_t at) {
    unsigned count = m.pluses;
    m.pluses = 0;
    while (count--) data_byte('+', at);
}

static void escape_timeout(uint64_t at) {
    if (!m.pluses || at - m.last_input < GUARD_NS) return;
    if (m.pluses == 3) {
        m.pluses = 0;
        m.command = 1;
        m.line_length = m.command_overflow = 0;
        answer(OK);
    } else flush_pluses(at);
}

static void dte_byte(uint8_t byte, uint64_t at) {
    if (m.connected && !m.command) {
        escape_timeout(at);
        if (!m.command) {
            if (m.pluses) {
                if (byte == '+' && m.pluses < 3) ++m.pluses;
                else { flush_pluses(at); data_byte(byte, at); }
            } else if (byte == '+' && at - m.last_input >= GUARD_NS) m.pluses = 1;
            else data_byte(byte, at);
            m.last_input = at;
            return;
        }
    }
    if (m.dialing) {
        /* A key aborts an in-progress Hayes dial. */
        disconnect(1);
        return;
    }
    if (byte == '\n') return;
    if (m.echo) {
        char text[2] = {(char)byte, 0};
        answer_text(text);
    }
    if (byte == '\r') {
        execute();
        m.line_length = m.command_overflow = 0;
    } else if (byte == '\b' || byte == 127) {
        if (m.line_length) --m.line_length;
    } else if (byte >= 32) {
        if (m.line_length + 1 < sizeof m.line) m.line[m.line_length++] = (char)byte;
        else m.command_overflow = 1;
    }
}

static uint64_t uart_byte_ns(void) {
    unsigned divisor = m.dll | (unsigned)m.dlm << 8;
    if (!divisor) divisor = 65536;
    unsigned bits = 1 + 5 + (m.lcr & 3) + ((m.lcr & 8) ? 1 : 0)
        + ((m.lcr & 4) ? 2 : 1);
    return ((uint64_t)bits * divisor * UINT64_C(1000000000) + 115199) / 115200;
}

static void uart_progress(void) {
    while (m.shift_full && m.shift_done <= m.now) {
        uint64_t at = m.shift_done;
        uint8_t byte = m.shift;
        m.shift_full = 0;
        if (m.mcr & LOOP) {
            receive_byte((m.lcr & 0x40) ? 0 : byte);
            if (m.lcr & 0x40) m.errors |= 0x18; /* Break + framing error. */
        } else dte_byte(byte, at);
        if (m.thr_full) {
            m.shift = m.thr;
            m.shift_full = 1;
            m.thr_full = 0;
            m.thre_irq = 1;
            m.shift_done = at + uart_byte_ns();
        }
    }
}

static void wire_bytes(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) (void)put(&m.wire, bytes[i]);
}

static void negotiate(unsigned verb, uint8_t option) {
    uint8_t response[3] = {IAC, 0, option};
    uint8_t *state;
    if (verb == WILL || verb == WONT) {
        state = m.remote_option + option;
        if (verb == WONT) {
            if (*state == 1) response[1] = DONT;
            *state = 0;
        } else if (option == BINARY || option == SGA || option == ECHO) {
            if (*state != 1) response[1] = DO;
            *state = 1;
        } else {
            if (*state != 2) response[1] = DONT;
            *state = 2; /* Refused; repeating WILL cannot create a loop. */
        }
    } else {
        state = m.local_option + option;
        if (verb == DONT) {
            if (*state == 1) response[1] = WONT;
            *state = 0;
        } else if (option == BINARY || option == SGA || option == TTYPE || option == NAWS) {
            if (*state != 1) response[1] = WILL;
            *state = 1;
        } else {
            if (*state != 2) response[1] = WONT;
            *state = 2; /* In particular, the modem never echoes for the BBS. */
        }
    }
    if (response[1]) wire_bytes(response, sizeof response);
    if (verb == DO && option == NAWS && response[1] == WILL) {
        static const uint8_t size[] = {IAC, SB, NAWS, 0, 80, 0, 25, IAC, SE};
        wire_bytes(size, sizeof size);
    }
}

static void subnegotiation(void) {
    if (m.sb_option == TTYPE && m.local_option[TTYPE] == 1
            && !m.sb_overflow && m.sb_length == 1 && m.sb_data[0] == 1) {
        static const uint8_t name[] = {IAC, SB, TTYPE, 0, 'A', 'N', 'S', 'I', IAC, SE};
        wire_bytes(name, sizeof name);
    }
}

static void remote_byte(uint8_t byte) {
    enum { DATA, COMMAND, OPTION, SUB_OPTION, SUB_DATA, SUB_IAC };
    switch (m.telnet_state) {
    case DATA:
        if (byte == IAC) m.telnet_state = COMMAND;
        else (void)put(&m.remote, byte);
        break;
    case COMMAND:
        if (byte == IAC) { (void)put(&m.remote, IAC); m.telnet_state = DATA; }
        else if (byte == WILL || byte == WONT || byte == DO || byte == DONT) {
            m.telnet_verb = byte;
            m.telnet_state = OPTION;
        } else if (byte == SB) m.telnet_state = SUB_OPTION;
        else m.telnet_state = DATA;
        break;
    case OPTION:
        negotiate(m.telnet_verb, byte);
        m.telnet_state = DATA;
        break;
    case SUB_OPTION:
        m.sb_option = byte;
        m.sb_length = m.sb_overflow = 0;
        m.telnet_state = SUB_DATA;
        break;
    case SUB_DATA:
        if (byte == IAC) m.telnet_state = SUB_IAC;
        else if (m.sb_length < sizeof m.sb_data) m.sb_data[m.sb_length++] = byte;
        else m.sb_overflow = 1;
        break;
    case SUB_IAC:
        if (byte == SE) { subnegotiation(); m.telnet_state = DATA; }
        else {
            if (byte == IAC) {
                if (m.sb_length < sizeof m.sb_data) m.sb_data[m.sb_length++] = byte;
                else m.sb_overflow = 1;
            } else m.sb_overflow = 1;
            m.telnet_state = SUB_DATA;
        }
        break;
    }
}

static void transport_progress(void) {
    if ((!m.dialing && !m.connected) || m.now < m.poll_due) return;
    m.poll_due = m.now + POLL_NS;
    enum ModemTransportState state = m.host.status ? m.host.status() : MODEM_TRANSPORT_CLOSED;
    if (m.dialing) {
        if (state == MODEM_TRANSPORT_OPEN) {
            m.dialing = m.command = 0;
            m.connected = 1;
            m.last_input = m.now;
            m.rx_due = m.now + BYTE_NS;
            answer(CONNECT);
        } else if (state != MODEM_TRANSPORT_CONNECTING || m.now - m.dial_started >= DIAL_NS) {
            disconnect(0);
            answer(state == MODEM_TRANSPORT_BUSY ? BUSY : NO_ANSWER);
            return;
        }
    } else if (state != MODEM_TRANSPORT_OPEN) { peer_closed(); return; }
    if (!m.connected || !m.host.read) return;
    /* Reserve for the largest subnegotiation reply before reading. Fragmented
     * telnet, repeated requests, and a stalled socket all stay bounded. */
    for (unsigned reads = 0; reads < 4; ++reads) {
        uint8_t bytes[256];
        size_t capacity = room(&m.remote);
        if (capacity > room(&m.wire) / 16) capacity = room(&m.wire) / 16;
        if (capacity > sizeof bytes) capacity = sizeof bytes;
        if (!capacity) break;
        size_t count = m.host.read(bytes, capacity);
        if (!count) break;
        if (count > capacity) { disconnect(1); break; }
        if (!m.remote.count && !m.result.count && !m.rbr_full) m.rx_due = m.now + BYTE_NS;
        for (size_t i = 0; i < count; ++i) remote_byte(bytes[i]);
    }
}

static void output_progress(void) {
    if (!m.connected) return;
    while (m.outgoing.count && m.now >= m.tx_due && room(&m.wire) >= 2) {
        uint8_t byte = get(&m.outgoing);
        (void)put(&m.wire, byte);
        if (byte == IAC) (void)put(&m.wire, IAC);
        m.tx_due += BYTE_NS;
    }
    if (m.wire.count && m.host.write) {
        size_t count = QUEUE_SIZE - m.wire.head;
        if (count > m.wire.count) count = m.wire.count;
        size_t written = m.host.write(m.wire.bytes + m.wire.head, count);
        if (written > count) { disconnect(1); return; }
        m.wire.head = (m.wire.head + written) % QUEUE_SIZE;
        m.wire.count -= written;
    }
}

void modem_tick(uint64_t now_ns) {
    if (now_ns < m.now) now_ns = m.now;
    m.now = now_ns;
    uart_progress();
    escape_timeout(m.now);
    transport_progress();
    output_progress();
    /* The modem's bounded receive buffer supplies the single RBR only when
     * it is free. Host scheduling pauses therefore exert backpressure on the
     * virtual wire, rather than corrupting the BBS with an artificial burst.
     * Real UART overruns remain observable through its internal loopback. */
    if (!(m.mcr & LOOP) && !m.rbr_full && m.now >= m.rx_due) {
        Queue *q = m.result.count ? &m.result :
            ((m.connected && !m.command) || m.remote_eof ? &m.remote : NULL);
        if (q && q->count) {
            receive_byte(get(q));
            m.rx_due += BYTE_NS;
            /* No more than 32 chars of scheduling credit after a host pause. */
            if (m.now > m.rx_due && m.now - m.rx_due > BYTE_NS * 32)
                m.rx_due = m.now - BYTE_NS * 32;
        }
    }
    if (m.remote_eof && !m.remote.count) {
        m.remote_eof = 0;
        answer(NO_CARRIER);
    }
    leads();
}

uint8_t modem_port_in(uint16_t port) {
    switch (port - COM1) {
    case 0:
        if (m.lcr & 0x80) return m.dll;
        m.rbr_full = 0;
        return m.rbr;
    case 1: return (m.lcr & 0x80) ? m.dlm : m.ier;
    case 2: {
        uint8_t cause = iir();
        if (cause == 2) m.thre_irq = 0;
        return cause;
    }
    case 3: return m.lcr;
    case 4: return m.mcr;
    case 5: {
        uint8_t status = lsr();
        m.errors = 0;
        return status;
    }
    case 6: {
        uint8_t status = m.msr | m.deltas;
        m.deltas = 0;
        return status;
    }
    case 7: return m.scratch;
    default: return 0xff;
    }
}

void modem_port_out(uint16_t port, uint8_t value) {
    switch (port - COM1) {
    case 0:
        if (m.lcr & 0x80) { m.dll = value; break; }
        m.thre_irq = 0;
        if (!m.shift_full) {
            m.shift = value;
            m.shift_full = 1;
            m.shift_done = m.now + uart_byte_ns();
            m.thre_irq = 1; /* THR immediately transferred to idle shifter. */
        } else { m.thr = value; m.thr_full = 1; }
        break;
    case 1:
        if (m.lcr & 0x80) m.dlm = value;
        else {
            if ((value & 2) && !(m.ier & 2) && !m.thr_full) m.thre_irq = 1;
            m.ier = value & 15;
        }
        break;
    case 2: break; /* FCR does not exist on a 16450; no fake FIFO flags. */
    case 3: m.lcr = value; break;
    case 4: {
        uint8_t previous = m.mcr;
        m.mcr = value & 31;
        if ((previous & DTR) && !(m.mcr & DTR) && !(previous & LOOP)) disconnect(1);
        break;
    }
    case 7: m.scratch = value; break;
    default: break; /* LSR and MSR are read-only. */
    }
    leads();
}

int modem_irq_pending(void) { return (m.mcr & OUT2) && !(iir() & 1); }

uint16_t modem_bios(unsigned function, uint8_t value, unsigned port) {
    if (port) return 0x8000 | value;
    switch (function) {
    case 0: {
        static const uint16_t divisors[8] = {1047, 768, 384, 192, 96, 48, 24, 12};
        uint16_t divisor = divisors[value >> 5];
        modem_port_out(COM1 + 3, 0x80);
        modem_port_out(COM1, (uint8_t)divisor);
        modem_port_out(COM1 + 1, (uint8_t)(divisor >> 8));
        modem_port_out(COM1 + 3, value & 0x1f);
        modem_port_out(COM1 + 1, 0);
        modem_port_out(COM1 + 4, DTR | RTS);
        return (uint16_t)(modem_port_in(COM1 + 5) << 8) | modem_port_in(COM1 + 6);
    }
    case 1:
        if (!(lsr() & THRE)) return (uint16_t)((lsr() | 0x80) << 8) | value;
        modem_port_out(COM1, value);
        return (uint16_t)(lsr() << 8) | value;
    case 2: {
        uint8_t status = modem_port_in(COM1 + 5);
        if (!(status & DR)) return (uint16_t)((status | 0x80) << 8);
        return (uint16_t)(status << 8) | modem_port_in(COM1);
    }
    case 3: return (uint16_t)(modem_port_in(COM1 + 5) << 8) | modem_port_in(COM1 + 6);
    default: return 0x8000 | value;
    }
}
