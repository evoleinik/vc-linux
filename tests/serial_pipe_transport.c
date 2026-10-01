/* Test-only raw ModemTransport for environments that forbid socket().
 * Link this INSTEAD OF runtime/modem_transport.c, never into a release.
 * The pty harness owns both pipe pairs and supplies their inherited fd
 * numbers; the real modem/UART/Kermit translation remain unchanged. This
 * is not a substitute for the separately required real TCP loopback gate. */
#define _POSIX_C_SOURCE 200809L
#include "modem_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static int receive_fd = -1, transmit_fd = -1;
static enum ModemTransportState state = MODEM_TRANSPORT_CLOSED;

static void pipe_close(void) {
    if (receive_fd >= 0) close(receive_fd);
    if (transmit_fd >= 0) close(transmit_fd);
    receive_fd = transmit_fd = -1;
    state = MODEM_TRANSPORT_CLOSED;
}

static int inherited_fd(const char *name) {
    const char *text = getenv(name);
    if (!text || !*text) return -1;
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno || *end || number < 0 || number > INT_MAX) return -1;
    int fd = fcntl((int)number, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pipe_dial(const char *env_key, const char *web_url) {
    (void)env_key;
    (void)web_url;
    pipe_close();
    receive_fd = inherited_fd("VC_MODEM_TEST_RX_FD");
    transmit_fd = inherited_fd("VC_MODEM_TEST_TX_FD");
    if (receive_fd < 0 || transmit_fd < 0) {
        pipe_close();
        state = MODEM_TRANSPORT_NO_ANSWER;
        return;
    }
    /* A dead test harness must become carrier loss, not SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);
    state = MODEM_TRANSPORT_OPEN;
}

static enum ModemTransportState pipe_status(void) {
    if (state == MODEM_TRANSPORT_OPEN) {
        struct pollfd ready = { .fd = receive_fd, .events = POLLIN };
        if (poll(&ready, 1, 0) > 0 && !(ready.revents & POLLIN)
            && (ready.revents & (POLLHUP | POLLERR | POLLNVAL))) pipe_close();
    }
    return state;
}

static size_t pipe_read(uint8_t *data, size_t capacity) {
    if (state != MODEM_TRANSPORT_OPEN || !capacity) return 0;
    ssize_t count = read(receive_fd, data, capacity);
    if (count > 0) return (size_t)count;
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) pipe_close();
    return 0;
}

static size_t pipe_write(const uint8_t *data, size_t count) {
    if (state != MODEM_TRANSPORT_OPEN || !count) return 0;
    ssize_t written = write(transmit_fd, data, count);
    if (written >= 0) return (size_t)written;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) pipe_close();
    return 0;
}

const ModemTransport *modem_transport_ops(void) {
    static const ModemTransport transport = { pipe_dial, pipe_status, pipe_read, pipe_write, pipe_close };
    return &transport;
}
