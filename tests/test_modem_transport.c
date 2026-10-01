/* Loopback-only native transport gate. The resolver wrapper below maps its
 * artificial name to localhost, so the test never uses external DNS/BBSes. */
#define _POSIX_C_SOURCE 200809L
#include "modem_transport.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const ModemTransport *transport;
static atomic_int resolver_started, resolver_release, resolver_finished;

static void pause_briefly(void) {
    struct timespec delay = { .tv_nsec = 1000000 };
    nanosleep(&delay, NULL);
}

int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *service,
                       const struct addrinfo *hints, struct addrinfo **result) {
    if (strcmp(host, "slow-fixture.invalid") == 0) {
        atomic_store(&resolver_started, 1);
        /* The dispatcher has to return from dial() before it can release
         * this lookup. A synchronous resolver therefore hits alarm(). */
        while (!atomic_load(&resolver_release)) pause_briefly();
        int error = __real_getaddrinfo("localhost", service, hints, result);
        atomic_store(&resolver_finished, 1);
        return error;
    }
    assert(strcmp(host, "localhost") == 0);
    return __real_getaddrinfo(host, service, hints, result);
}

static int listener(unsigned *port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) perror("required loopback TCP socket");
    assert(fd >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    socklen_t length = sizeof address;
    assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    *port = ntohs(address.sin_port);
    assert(listen(fd, 4) == 0);
    return fd;
}

static void endpoint(const char *host, unsigned port) {
    char value[300];
    snprintf(value, sizeof value, "%s:%u", host, port);
    assert(setenv("VC_TEST_MODEM_ENDPOINT", value, 1) == 0);
}

static void dial(void) {
    transport->dial("VC_TEST_MODEM_ENDPOINT", "wss://unused.invalid/");
}

static void wait_state(enum ModemTransportState expected) {
    for (unsigned tries = 0; tries < 3000; ++tries) {
        if (transport->status() == expected) return;
        pause_briefly();
    }
    fprintf(stderr, "transport state %d, expected %d\n", transport->status(), expected);
    assert(0 && "transport never reached expected state");
}

static int accept_call(int server) {
    wait_state(MODEM_TRANSPORT_OPEN);
    int peer = -1;
    for (unsigned tries = 0; tries < 3000; ++tries) {
        peer = accept(server, NULL, NULL);
        if (peer >= 0) break;
        assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        pause_briefly();
    }
    assert(peer >= 0);
    assert(fcntl(peer, F_SETFL, fcntl(peer, F_GETFL) | O_NONBLOCK) == 0);
    return peer;
}

static void receive_exact(int peer, const uint8_t *expected, size_t length) {
    uint8_t bytes[64];
    assert(length <= sizeof bytes);
    size_t used = 0;
    for (unsigned tries = 0; used < length && tries < 3000; ++tries) {
        ssize_t count = recv(peer, bytes + used, length - used, MSG_DONTWAIT);
        if (count > 0) used += (size_t)count;
        else {
            assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
            pause_briefly();
        }
    }
    assert(used == length);
    assert(memcmp(bytes, expected, length) == 0);
}

static void read_exact(const uint8_t *expected, size_t length) {
    uint8_t bytes[64];
    assert(length <= sizeof bytes);
    size_t used = 0;
    for (unsigned tries = 0; used < length && tries < 3000; ++tries) {
        assert(transport->status() == MODEM_TRANSPORT_OPEN);
        size_t count = transport->read(bytes + used, length - used);
        if (count) used += count;
        else pause_briefly();
    }
    assert(used == length);
    assert(memcmp(bytes, expected, length) == 0);
}

static void test_missing_invalid_refused(void) {
    assert(unsetenv("VC_TEST_MODEM_ENDPOINT") == 0);
    dial();
    assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    const char *invalid[] = { "", "localhost", ":1", "localhost:0", "localhost:-1",
        "localhost:65536", "localhost:http", "localhost:12junk", "[::1]:", "::1:23", "[::1:23" };
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(setenv("VC_TEST_MODEM_ENDPOINT", invalid[i], 1) == 0);
        dial();
        assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    }
    unsigned port;
    int server = listener(&port);
    close(server);
    endpoint("127.0.0.1", port);
    dial();
    wait_state(MODEM_TRANSPORT_BUSY);
    transport->close();
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
}

static void test_raw_bytes_eof(void) {
    unsigned port;
    int server = listener(&port);
    endpoint("127.0.0.1", port);
    dial();
    int peer = accept_call(server);
    const uint8_t raw[] = { 0xff, 0xfb, 0, 0xff, 0xff, 0x80, 0x0d, 0x0a };
    uint8_t empty;
    assert(transport->read(&empty, 1) == 0);
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    assert(transport->write(raw, sizeof raw) == sizeof raw);
    receive_exact(peer, raw, sizeof raw);
    assert(send(peer, raw, sizeof raw, MSG_NOSIGNAL) == sizeof raw);
    read_exact(raw, sizeof raw);
    assert(transport->read(&empty, 0) == 0);
    assert(transport->write(raw, 0) == 0);
    /* Payload preceding FIN cannot be thrown away by a status poll. */
    assert(send(peer, raw, sizeof raw, MSG_NOSIGNAL) == sizeof raw);
    assert(shutdown(peer, SHUT_WR) == 0);
    read_exact(raw, sizeof raw);
    wait_state(MODEM_TRANSPORT_CLOSED);
    assert(transport->write(raw, sizeof raw) == 0);
    close(peer);
    close(server);
}

static void test_backpressure_reset(void) {
    unsigned port;
    int server = listener(&port);
    endpoint("127.0.0.1", port);
    dial();
    int peer = accept_call(server);
    /* Without O_NONBLOCK / MSG_DONTWAIT, filling the socket hangs this
     * test and the external alarm fails the gate. No heap send queue. */
    static const uint8_t block[65536] = { 0xff, 0x42 };
    int partial = 0, blocked = 0;
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        size_t written = transport->write(block, sizeof block);
        assert(written <= sizeof block);
        if (written < sizeof block) partial = 1;
        if (!written) { blocked = 1; break; }
    }
    assert(partial && blocked);
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    struct linger reset = { .l_onoff = 1, .l_linger = 0 };
    assert(setsockopt(peer, SOL_SOCKET, SO_LINGER, &reset, sizeof reset) == 0);
    close(peer);
    for (unsigned tries = 0; tries < 3000; ++tries) {
        transport->write(block, sizeof block);
        if (transport->status() == MODEM_TRANSPORT_CLOSED) break;
        pause_briefly();
    }
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
    close(server);
}

static void test_async_dns_reconnect(void) {
    unsigned first_port, second_port;
    int old_server = listener(&first_port), new_server = listener(&second_port);
    endpoint("slow-fixture.invalid", first_port);
    dial();
    for (unsigned tries = 0; !atomic_load(&resolver_started) && tries < 3000; ++tries) pause_briefly();
    assert(atomic_load(&resolver_started));
    assert(transport->status() == MODEM_TRANSPORT_CONNECTING);
    endpoint("127.0.0.1", second_port);
    dial();
    int peer = accept_call(new_server);
    atomic_store(&resolver_release, 1);
    for (unsigned tries = 0; !atomic_load(&resolver_finished) && tries < 3000; ++tries) pause_briefly();
    assert(atomic_load(&resolver_finished));
    const uint8_t current[] = { 'n', 'e', 'w' };
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    assert(transport->write(current, sizeof current) == sizeof current);
    receive_exact(peer, current, sizeof current);
    assert(accept(old_server, NULL, NULL) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    close(peer);
    transport->close();
    /* Real local resolution, including falling back from ::1 to IPv4. */
    endpoint("localhost", second_port);
    dial();
    peer = accept_call(new_server);
    assert(transport->write(current, sizeof current) == sizeof current);
    receive_exact(peer, current, sizeof current);
    transport->close();
    close(peer);
    close(old_server);
    close(new_server);
}

int main(void) {
    alarm(20); /* also catches a deliberately planted blocking-socket defect */
    transport = modem_transport_ops();
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
    test_missing_invalid_refused();
    test_raw_bytes_eof();
    test_backpressure_reset();
    test_async_dns_reconnect();
    transport->close();
    assert(unsetenv("VC_TEST_MODEM_ENDPOINT") == 0);
    alarm(0);
    puts("native modem transport: config, nonblocking TCP/DNS, raw bytes, EOF, backpressure and reconnect passed");
    return 0;
}
