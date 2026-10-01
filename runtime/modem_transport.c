/* Raw, nonblocking byte transport for COM1's shared Hayes/telnet modem.
 * No socket, resolver or JavaScript callback calls translated guest code. */
#define _POSIX_C_SOURCE 200809L
#include "modem_transport.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* The page supplies an ordinary object, also used with an injected fake
 * WebSocket by Node's smoke test. Polling never enters Asyncify: port I/O
 * may call this code from the translated program's indirect call chain. */
EM_JS(void, web_dial, (const char *url), {
    const modem = Module['vcModem'];
    if (modem) modem.dial(UTF8ToString(url));
});

EM_JS(int, web_status, (void), {
    return Module['vcModem'] ? Module['vcModem'].status() : 3;
});

EM_JS(size_t, web_read, (uint8_t *data, size_t capacity), {
    const modem = Module['vcModem'];
    if (!modem) return 0;
    const bytes = modem.read(capacity);
    if (!bytes) return 0;
    const count = Math.min(bytes.length, capacity);
    HEAPU8.set(bytes.subarray(0, count), data);
    return count;
});

EM_JS(size_t, web_write, (const uint8_t *data, size_t count), {
    const modem = Module['vcModem'];
    return modem ? modem.write(HEAPU8.subarray(data, data + count)) : 0;
});

EM_JS(void, web_close, (void), {
    if (Module['vcModem']) Module['vcModem'].close();
});

static void transport_dial(const char *env_key, const char *web_url) {
    (void)env_key;
    web_dial(web_url);
}

static enum ModemTransportState transport_status(void) {
    return (enum ModemTransportState)web_status();
}

static const ModemTransport transport = {
    transport_dial, transport_status, web_read, web_write, web_close,
};

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { MAX_ADDRESSES = 8, MAX_HOST = 253, DIAL_TIMEOUT_SECONDS = 30 };

typedef struct {
    struct sockaddr_storage address;
    socklen_t length;
} Address;

/* One reusable resolver worker bounds even repeated dial/hangup attempts:
 * one lookup in progress, one replaceable pending request, one answer.
 * getaddrinfo never runs on the dispatcher. A generation number discards a
 * late answer after hangup/reconnect; shutdown never joins a blocked DNS
 * lookup. Its static state lives until process exit, like libc's resolver. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int started, pending, answered;
    uint64_t request_generation, answer_generation;
    char host[MAX_HOST + 1], service[6];
    Address answer[MAX_ADDRESSES];
    unsigned answer_count;
} resolver = { .mutex = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };

static struct {
    int fd;
    enum ModemTransportState state;
    uint64_t generation, dial_started;
    Address addresses[MAX_ADDRESSES];
    unsigned count, next;
    int resolving, refused;
} connection = { .fd = -1, .state = MODEM_TRANSPORT_CLOSED };

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void *resolve_worker(void *unused) {
    (void)unused;
    for (;;) {
        char host[MAX_HOST + 1], service[6];
        pthread_mutex_lock(&resolver.mutex);
        while (!resolver.pending) pthread_cond_wait(&resolver.ready, &resolver.mutex);
        uint64_t generation = resolver.request_generation;
        memcpy(host, resolver.host, sizeof host);
        memcpy(service, resolver.service, sizeof service);
        resolver.pending = 0;
        pthread_mutex_unlock(&resolver.mutex);

        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                                  .ai_protocol = IPPROTO_TCP, .ai_flags = AI_NUMERICSERV };
        struct addrinfo *results = NULL;
        Address addresses[MAX_ADDRESSES];
        unsigned count = 0;
        if (getaddrinfo(host, service, &hints, &results) == 0) {
            for (struct addrinfo *it = results; it && count < MAX_ADDRESSES; it = it->ai_next) {
                if ((it->ai_family != AF_INET && it->ai_family != AF_INET6)
                    || it->ai_addrlen > sizeof addresses[count].address) continue;
                memset(&addresses[count], 0, sizeof addresses[count]);
                memcpy(&addresses[count].address, it->ai_addr, it->ai_addrlen);
                addresses[count++].length = it->ai_addrlen;
            }
            freeaddrinfo(results);
        }
        pthread_mutex_lock(&resolver.mutex);
        resolver.answer_generation = generation;
        memcpy(resolver.answer, addresses, count * sizeof *addresses);
        resolver.answer_count = count;
        resolver.answered = 1;
        pthread_mutex_unlock(&resolver.mutex);
    }
    return NULL;
}

static int request_resolution(const char *host, const char *service) {
    if (!resolver.started) {
        pthread_attr_t attributes;
        pthread_t worker;
        if (pthread_attr_init(&attributes)) return 0;
        int failed = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED)
            || pthread_create(&worker, &attributes, resolve_worker, NULL);
        pthread_attr_destroy(&attributes);
        if (failed) return 0;
        resolver.started = 1;
    }
    pthread_mutex_lock(&resolver.mutex);
    resolver.request_generation = connection.generation;
    strcpy(resolver.host, host);
    strcpy(resolver.service, service);
    resolver.pending = 1;
    resolver.answered = 0;
    pthread_cond_signal(&resolver.ready);
    pthread_mutex_unlock(&resolver.mutex);
    return 1;
}

static void close_socket(enum ModemTransportState state) {
    if (connection.fd >= 0) close(connection.fd);
    connection.fd = -1;
    connection.state = state;
}

static void transport_close(void) {
    close_socket(MODEM_TRANSPORT_CLOSED);
    ++connection.generation;
    connection.resolving = 0;
    connection.count = connection.next = 0;
    connection.refused = 0;
}

/* host:port, including the conventional [IPv6]:port form. Never feed this
 * user configuration to a shell, URL parser, or service-name resolver. */
static int split_endpoint(const char *endpoint, char *host, char *service) {
    if (!endpoint || !*endpoint) return 0;
    const char *first = endpoint, *end, *port;
    if (*first == '[') {
        end = strchr(++first, ']');
        if (!end || end[1] != ':') return 0;
        port = end + 2;
    } else {
        end = strrchr(first, ':');
        if (!end || memchr(first, ':', (size_t)(end - first))) return 0;
        port = end + 1;
    }
    size_t length = (size_t)(end - first);
    if (!length || length > MAX_HOST || !*port || strlen(port) > 5) return 0;
    unsigned number = 0;
    for (const char *it = port; *it; ++it) {
        if (*it < '0' || *it > '9') return 0;
        number = number * 10 + (unsigned)(*it - '0');
    }
    if (!number || number > 65535) return 0;
    memcpy(host, first, length);
    host[length] = 0;
    strcpy(service, port);
    return 1;
}

static void start_next_address(void) {
    close_socket(MODEM_TRANSPORT_CONNECTING);
    while (connection.next < connection.count) {
        const Address *candidate = &connection.addresses[connection.next++];
        int fd = socket(candidate->address.ss_family,
                        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) continue;
        connection.fd = fd;
        if (connect(fd, (const struct sockaddr *)&candidate->address, candidate->length) == 0) {
            connection.state = MODEM_TRANSPORT_OPEN;
            return;
        }
        if (errno == EINPROGRESS || errno == EINTR) return;
        if (errno == ECONNREFUSED) connection.refused = 1;
        close_socket(MODEM_TRANSPORT_CONNECTING);
    }
    connection.state = connection.refused ? MODEM_TRANSPORT_BUSY : MODEM_TRANSPORT_NO_ANSWER;
}

static void transport_dial(const char *env_key, const char *web_url) {
    (void)web_url;
    transport_close();
    char host[MAX_HOST + 1], service[6];
    if (!split_endpoint(env_key ? getenv(env_key) : NULL, host, service)) {
        connection.state = MODEM_TRANSPORT_NO_ANSWER;
        return;
    }
    connection.state = MODEM_TRANSPORT_CONNECTING;
    connection.dial_started = now_ns();
    Address *address = &connection.addresses[0];
    memset(address, 0, sizeof *address);
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address->address;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address->address;
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)strtoul(service, NULL, 10));
        address->length = sizeof *v4;
    } else if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)strtoul(service, NULL, 10));
        address->length = sizeof *v6;
    } else {
        connection.resolving = request_resolution(host, service);
        if (!connection.resolving) connection.state = MODEM_TRANSPORT_NO_ANSWER;
        return;
    }
    connection.count = 1;
    start_next_address();
}

static enum ModemTransportState transport_status(void) {
    if (connection.state == MODEM_TRANSPORT_CONNECTING) {
        if (now_ns() - connection.dial_started >= UINT64_C(1000000000) * DIAL_TIMEOUT_SECONDS) {
            connection.resolving = 0;
            close_socket(MODEM_TRANSPORT_NO_ANSWER);
        } else if (connection.resolving && pthread_mutex_trylock(&resolver.mutex) == 0) {
            if (resolver.answered && resolver.answer_generation == connection.generation) {
                connection.count = resolver.answer_count;
                memcpy(connection.addresses, resolver.answer,
                       connection.count * sizeof *connection.addresses);
                connection.resolving = 0;
                resolver.answered = 0;
            }
            pthread_mutex_unlock(&resolver.mutex);
            if (!connection.resolving) start_next_address();
        }
        if (connection.fd >= 0 && connection.state == MODEM_TRANSPORT_CONNECTING) {
            struct pollfd waiting = { .fd = connection.fd, .events = POLLOUT };
            if (poll(&waiting, 1, 0) > 0) {
                int error = 0;
                socklen_t length = sizeof error;
                if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &error, &length)) error = errno;
                if (!error) connection.state = MODEM_TRANSPORT_OPEN;
                else {
                    if (error == ECONNREFUSED) connection.refused = 1;
                    start_next_address();
                }
            }
        }
    }
    if (connection.state == MODEM_TRANSPORT_OPEN) {
        /* EOF becomes NO CARRIER only after the final network bytes have
         * drained. A peer may send its goodbye and FIN in the same packet. */
        uint8_t byte;
        ssize_t count = recv(connection.fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            close_socket(MODEM_TRANSPORT_CLOSED);
    }
    return connection.state;
}

static size_t transport_read(uint8_t *data, size_t capacity) {
    if (connection.state != MODEM_TRANSPORT_OPEN || !capacity) return 0;
    ssize_t count = recv(connection.fd, data, capacity, MSG_DONTWAIT);
    if (count > 0) return (size_t)count;
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        close_socket(MODEM_TRANSPORT_CLOSED);
    return 0;
}

static size_t transport_write(const uint8_t *data, size_t count) {
    if (connection.state != MODEM_TRANSPORT_OPEN || !count) return 0;
    ssize_t written = send(connection.fd, data, count, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written >= 0) return (size_t)written;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        close_socket(MODEM_TRANSPORT_CLOSED);
    return 0;
}

static const ModemTransport transport = {
    transport_dial, transport_status, transport_read, transport_write, transport_close,
};

#endif

const ModemTransport *modem_transport_ops(void) {
    return &transport;
}
