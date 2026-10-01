/* Supplemental native transport tests with link-time syscall wrappers.
 * These need no socket permission and prove state/byte handling, not kernel
 * TCP behavior. tests/test_modem_transport.c remains the real loopback gate. */
#define _POSIX_C_SOURCE 200809L
#include "modem_transport.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { FIRST_FD = 2000, MAX_SOCKETS = 32 };
typedef struct {
    int family, connect_error, ready, socket_error;
    unsigned port;
} Attempt;
typedef struct {
    int active, family, eof, read_error, send_error;
    size_t read_chunk, send_capacity;
    uint8_t received[128], sent[128];
    size_t receive_count, receive_head, sent_count;
    Attempt attempt;
} FakeSocket;

static FakeSocket sockets[MAX_SOCKETS];
static Attempt attempts[MAX_SOCKETS];
static unsigned socket_count, attempt_count, attempt_next;
static unsigned close_count, socket_failures;
static atomic_ullong clock_ns;
static atomic_int slow_started, slow_release, slow_finished;
static atomic_int latest_started, latest_release = 1, skipped_lookups;
static pthread_t dispatcher;
static const ModemTransport *transport;

static void pause_briefly(void) {
    struct timespec delay = { .tv_nsec = 1000000 };
    nanosleep(&delay, NULL);
}

static FakeSocket *socket_for(int fd) {
    assert(fd >= FIRST_FD && fd < FIRST_FD + (int)socket_count);
    FakeSocket *socket = &sockets[fd - FIRST_FD];
    assert(socket->active);
    return socket;
}

int __wrap_socket(int family, int type, int protocol) {
    assert(pthread_equal(pthread_self(), dispatcher));
    assert(family == AF_INET || family == AF_INET6);
    assert(type & SOCK_NONBLOCK);
    assert(type & SOCK_CLOEXEC);
    assert((type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_STREAM);
    assert(protocol == IPPROTO_TCP);
    if (socket_failures) { --socket_failures; errno = EMFILE; return -1; }
    assert(socket_count < MAX_SOCKETS);
    FakeSocket *socket = &sockets[socket_count];
    memset(socket, 0, sizeof *socket);
    socket->active = 1;
    socket->family = family;
    socket->read_chunk = 128;
    socket->send_capacity = 128;
    return FIRST_FD + (int)socket_count++;
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length) {
    FakeSocket *socket = socket_for(fd);
    assert(attempt_next < attempt_count);
    socket->attempt = attempts[attempt_next++];
    assert(socket->family == socket->attempt.family);
    assert(address->sa_family == socket->family);
    if (address->sa_family == AF_INET) {
        assert(length == sizeof(struct sockaddr_in));
        assert(ntohs(((const struct sockaddr_in *)address)->sin_port) == socket->attempt.port);
    } else {
        assert(length == sizeof(struct sockaddr_in6));
        assert(ntohs(((const struct sockaddr_in6 *)address)->sin6_port) == socket->attempt.port);
    }
    if (!socket->attempt.connect_error) return 0;
    errno = socket->attempt.connect_error;
    return -1;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 1 && timeout == 0);
    FakeSocket *socket = socket_for(fds[0].fd);
    assert(fds[0].events == POLLOUT);
    fds[0].revents = socket->attempt.ready ? POLLOUT : 0;
    return socket->attempt.ready ? 1 : 0;
}

/* Fortified libc may select these entry points under sanitizers or a
 * different optimization level. Keep those calls inside the same fake. */
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    assert(count <= bytes / sizeof *fds);
    return __wrap_poll(fds, count, timeout);
}

int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    FakeSocket *socket = socket_for(fd);
    assert(level == SOL_SOCKET && option == SO_ERROR && *length >= sizeof(int));
    *(int *)value = socket->attempt.socket_error;
    *length = sizeof(int);
    return 0;
}

ssize_t __wrap_recv(int fd, void *data, size_t capacity, int flags) {
    FakeSocket *socket = socket_for(fd);
    assert(flags & MSG_DONTWAIT);
    assert(capacity > 0);
    if (socket->read_error) {
        errno = socket->read_error;
        socket->read_error = 0;
        return -1;
    }
    size_t available = socket->receive_count - socket->receive_head;
    if (!available) {
        if (socket->eof) return 0;
        errno = EAGAIN;
        return -1;
    }
    size_t count = available < capacity ? available : capacity;
    if (count > socket->read_chunk) count = socket->read_chunk;
    memcpy(data, socket->received + socket->receive_head, count);
    if (!(flags & MSG_PEEK)) socket->receive_head += count;
    return (ssize_t)count;
}

ssize_t __wrap___recv_chk(int fd, void *data, size_t capacity, size_t bytes, int flags) {
    assert(capacity <= bytes);
    return __wrap_recv(fd, data, capacity, flags);
}

ssize_t __wrap_send(int fd, const void *data, size_t length, int flags) {
    FakeSocket *socket = socket_for(fd);
    assert(flags & MSG_DONTWAIT);
    assert(flags & MSG_NOSIGNAL);
    if (socket->send_error) {
        errno = socket->send_error;
        socket->send_error = 0;
        return -1;
    }
    size_t count = length < socket->send_capacity ? length : socket->send_capacity;
    if (!count) { errno = EAGAIN; return -1; }
    assert(socket->sent_count + count <= sizeof socket->sent);
    memcpy(socket->sent + socket->sent_count, data, count);
    socket->sent_count += count;
    socket->send_capacity -= count;
    return (ssize_t)count;
}

int __wrap_close(int fd) {
    socket_for(fd)->active = 0;
    ++close_count;
    return 0;
}

int __wrap_clock_gettime(clockid_t which, struct timespec *out) {
    assert(which == CLOCK_MONOTONIC);
    uint64_t now = atomic_load(&clock_ns);
    out->tv_sec = (time_t)(now / UINT64_C(1000000000));
    out->tv_nsec = (long)(now % UINT64_C(1000000000));
    return 0;
}

static struct addrinfo *address(int family, unsigned port) {
    struct addrinfo *result = calloc(1, sizeof *result);
    assert(result);
    result->ai_family = family;
    result->ai_socktype = SOCK_STREAM;
    result->ai_protocol = IPPROTO_TCP;
    result->ai_addrlen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    result->ai_addr = calloc(1, result->ai_addrlen);
    assert(result->ai_addr);
    result->ai_addr->sa_family = family;
    if (family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)result->ai_addr;
        v4->sin_port = htons((uint16_t)port);
        v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)result->ai_addr;
        v6->sin6_port = htons((uint16_t)port);
        v6->sin6_addr = in6addr_loopback;
    }
    return result;
}

int __wrap_getaddrinfo(const char *host, const char *service,
                       const struct addrinfo *hints, struct addrinfo **result) {
    assert(!pthread_equal(pthread_self(), dispatcher) && "DNS must never block the dispatcher");
    assert(hints->ai_family == AF_UNSPEC && hints->ai_socktype == SOCK_STREAM);
    assert(hints->ai_flags & AI_NUMERICSERV);
    if (!strcmp(host, "missing.fixture")) return EAI_NONAME;
    if (!strcmp(host, "slow.fixture")) {
        atomic_store(&slow_started, 1);
        while (!atomic_load(&slow_release)) pause_briefly();
        atomic_store(&slow_finished, 1);
    } else if (!strcmp(host, "latest.fixture")) {
        atomic_store(&latest_started, 1);
        while (!atomic_load(&latest_release)) pause_briefly();
    } else if (!strcmp(host, "skipped.fixture")) atomic_fetch_add(&skipped_lookups, 1);
    unsigned port = (unsigned)strtoul(service, NULL, 10);
    if (!strcmp(host, "multi.fixture")) {
        *result = address(AF_INET6, port);
        (*result)->ai_next = address(AF_INET, port);
    } else if (!strcmp(host, "many.fixture")) {
        struct addrinfo **next = result;
        for (unsigned i = 0; i < 12; ++i) {
            *next = address(AF_INET, port);
            next = &(*next)->ai_next;
        }
    } else *result = address(AF_INET, port);
    return 0;
}

void __wrap_freeaddrinfo(struct addrinfo *it) {
    while (it) {
        struct addrinfo *next = it->ai_next;
        free(it->ai_addr);
        free(it);
        it = next;
    }
}

static void reset(void) {
    transport->close();
    for (unsigned i = 0; i < socket_count; ++i) assert(!sockets[i].active);
    socket_count = attempt_count = attempt_next = close_count = socket_failures = 0;
    atomic_store(&clock_ns, UINT64_C(7000000000));
}

static void plan(int family, unsigned port, int connect_error, int ready, int socket_error) {
    assert(attempt_count < MAX_SOCKETS);
    attempts[attempt_count++] = (Attempt){ family, connect_error, ready, socket_error, port };
}

static void dial(const char *endpoint) {
    if (endpoint) assert(setenv("VC_TEST_MODEM_ENDPOINT", endpoint, 1) == 0);
    else assert(unsetenv("VC_TEST_MODEM_ENDPOINT") == 0);
    transport->dial("VC_TEST_MODEM_ENDPOINT", "wss://unused.invalid/");
}

static void wait_state(enum ModemTransportState expected) {
    for (unsigned tries = 0; tries < 3000; ++tries) {
        if (transport->status() == expected) return;
        pause_briefly();
    }
    assert(0 && "transport did not reach expected state");
}

static void wait_atomic(atomic_int *value) {
    for (unsigned tries = 0; !atomic_load(value) && tries < 3000; ++tries) pause_briefly();
    assert(atomic_load(value));
}

static void test_config_connect_timeout(void) {
    reset();
    const char *invalid[] = { NULL, "", "host", ":23", "host:0", "host:65536", "host:word",
        "host:-1", "host:23extra", "[::1]", "[::1]:", "[::1:23", "::1:23" };
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        dial(invalid[i]);
        assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    }
    assert(socket_count == 0);
    plan(AF_INET, 23, EINPROGRESS, 0, 0);
    dial("127.0.0.1:23");
    assert(transport->status() == MODEM_TRANSPORT_CONNECTING);
    uint8_t byte = 1;
    assert(transport->read(&byte, 1) == 0 && transport->write(&byte, 1) == 0);
    atomic_store(&clock_ns, UINT64_C(36999999999));
    assert(transport->status() == MODEM_TRANSPORT_CONNECTING);
    atomic_store(&clock_ns, UINT64_C(37000000000));
    assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    assert(close_count == 1);
    reset();
    plan(AF_INET6, 2300, EINPROGRESS, 1, 0);
    dial("[::1]:2300");
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    assert(socket_count == 1);
    reset();
    plan(AF_INET, 42, ECONNREFUSED, 0, 0);
    dial("127.0.0.1:42");
    assert(transport->status() == MODEM_TRANSPORT_BUSY);
    assert(close_count == 1);
    reset();
    plan(AF_INET, 42, EINPROGRESS, 1, ECONNREFUSED);
    dial("127.0.0.1:42");
    assert(transport->status() == MODEM_TRANSPORT_BUSY);
    reset();
    plan(AF_INET, 42, ENETUNREACH, 0, 0);
    dial("127.0.0.1:42");
    assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    reset();
    socket_failures = 1;
    dial("127.0.0.1:42");
    assert(transport->status() == MODEM_TRANSPORT_NO_ANSWER);
    assert(socket_count == 0);
}

static void test_raw_partial_eof_errors(void) {
    reset();
    plan(AF_INET, 2323, 0, 1, 0);
    dial("127.0.0.1:2323");
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    const uint8_t raw[] = { 0xff, 0xfb, 0, 0xff, 0xff, 0x80, '\r', '\n' };
    FakeSocket *socket = &sockets[0];
    socket->send_capacity = 3;
    assert(transport->write(raw, sizeof raw) == 3);
    assert(transport->write(raw + 3, sizeof raw - 3) == 0);
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    socket->send_error = EINTR;
    socket->send_capacity = 8;
    assert(transport->write(raw + 3, sizeof raw - 3) == 0);
    assert(transport->write(raw + 3, sizeof raw - 3) == sizeof raw - 3);
    assert(socket->sent_count == sizeof raw && !memcmp(socket->sent, raw, sizeof raw));
    uint8_t bytes[sizeof raw];
    assert(transport->read(bytes, sizeof bytes) == 0);
    assert(transport->read(bytes, 0) == 0);
    assert(transport->write(raw, 0) == 0);
    memcpy(socket->received, raw, sizeof raw);
    socket->receive_count = sizeof raw;
    socket->eof = 1;
    socket->read_chunk = 3;
    for (size_t used = 0; used < sizeof raw;) {
        assert(transport->status() == MODEM_TRANSPORT_OPEN);
        used += transport->read(bytes + used, sizeof bytes - used);
    }
    assert(!memcmp(bytes, raw, sizeof raw));
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
    assert(close_count == 1);
    reset();
    plan(AF_INET, 2323, 0, 1, 0);
    dial("127.0.0.1:2323");
    sockets[0].read_error = EINTR;
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    sockets[0].read_error = ECONNRESET;
    assert(transport->read(bytes, sizeof bytes) == 0);
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
    reset();
    plan(AF_INET, 2323, 0, 1, 0);
    dial("127.0.0.1:2323");
    sockets[0].send_error = EPIPE;
    assert(transport->write(raw, sizeof raw) == 0);
    assert(transport->status() == MODEM_TRANSPORT_CLOSED);
}

static void test_dns_addresses(void) {
    reset();
    plan(AF_INET6, 7000, EINPROGRESS, 1, ECONNREFUSED);
    plan(AF_INET, 7000, EINPROGRESS, 1, 0);
    dial("multi.fixture:7000");
    wait_state(MODEM_TRANSPORT_OPEN);
    assert(socket_count == 2 && close_count == 1);
    reset();
    dial("missing.fixture:7001");
    wait_state(MODEM_TRANSPORT_NO_ANSWER);
    assert(socket_count == 0);
    reset();
    for (unsigned i = 0; i < 8; ++i) plan(AF_INET, 7002, ECONNREFUSED, 0, 0);
    dial("many.fixture:7002");
    wait_state(MODEM_TRANSPORT_BUSY);
    assert(socket_count == 8 && close_count == 8);
}

static void test_dns_cancel_and_bounded_queue(void) {
    reset();
    atomic_store(&slow_started, 0);
    atomic_store(&slow_release, 0);
    atomic_store(&slow_finished, 0);
    atomic_store(&latest_started, 0);
    atomic_store(&latest_release, 0);
    plan(AF_INET, 8003, 0, 1, 0);
    dial("slow.fixture:8001");
    wait_atomic(&slow_started);
    assert(transport->status() == MODEM_TRANSPORT_CONNECTING);
    dial("skipped.fixture:8002");
    dial("latest.fixture:8003");
    atomic_store(&slow_release, 1);
    wait_atomic(&latest_started); /* the worker has published the OLD answer */
    assert(transport->status() == MODEM_TRANSPORT_CONNECTING);
    assert(socket_count == 0 && !atomic_load(&skipped_lookups));
    atomic_store(&latest_release, 1);
    wait_state(MODEM_TRANSPORT_OPEN);
    assert(socket_count == 1);
    reset();
    atomic_store(&slow_started, 0);
    atomic_store(&slow_release, 0);
    atomic_store(&slow_finished, 0);
    plan(AF_INET, 8005, 0, 1, 0);
    dial("slow.fixture:8004");
    wait_atomic(&slow_started);
    dial("127.0.0.1:8005");
    assert(transport->status() == MODEM_TRANSPORT_OPEN);
    atomic_store(&slow_release, 1);
    wait_atomic(&slow_finished);
    assert(transport->status() == MODEM_TRANSPORT_OPEN && socket_count == 1);
    transport->close();
    assert(close_count == 1 && transport->status() == MODEM_TRANSPORT_CLOSED);
    transport->close();
    assert(close_count == 1);
}

int main(void) {
    alarm(20);
    dispatcher = pthread_self();
    transport = modem_transport_ops();
    test_config_connect_timeout();
    test_raw_partial_eof_errors();
    test_dns_addresses();
    test_dns_cancel_and_bounded_queue();
    reset();
    assert(unsetenv("VC_TEST_MODEM_ENDPOINT") == 0);
    alarm(0);
    puts("native modem syscalls (supplemental): nonblocking flags/DNS, partial I/O, EOF/errors, cancellation and bounds passed");
    return 0;
}
