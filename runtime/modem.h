/* COM1's 16450 UART and Hayes/telnet modem. The device is shared by the
 * native and browser runtimes; its only host dependencies are these bounded,
 * nonblocking transport operations and an explicitly supplied clock. */
#ifndef VC_MODEM_H
#define VC_MODEM_H

#include <stddef.h>
#include <stdint.h>

enum ModemTransportState {
    MODEM_TRANSPORT_CLOSED = 0,
    MODEM_TRANSPORT_CONNECTING,
    MODEM_TRANSPORT_OPEN,
    MODEM_TRANSPORT_NO_ANSWER,
    MODEM_TRANSPORT_BUSY,
};

typedef struct ModemTransport {
    void (*dial)(const char *env_key, const char *web_url);
    enum ModemTransportState (*status)(void);
    size_t (*read)(uint8_t *data, size_t capacity);
    size_t (*write)(const uint8_t *data, size_t count);
    void (*close)(void);
} ModemTransport;

/* init/reset also close the previous call. NULL selects an unplugged modem.
 * tick never sleeps or delivers a CPU interrupt, including when called from
 * a translated port-poll loop. now_ns must be monotonic; equal times are OK. */
void modem_init(const ModemTransport *transport);
/* Empty the phone book for a BBS door, regardless of host endpoint variables.
 * Device reset cannot re-enable it. Defaults off for normal native/web runs. */
void modem_set_door(int enabled);
void modem_reset(void);
void modem_tick(uint64_t now_ns);
uint8_t modem_port_in(uint16_t port); /* Full addresses 03F8h..03FFh. */
void modem_port_out(uint16_t port, uint8_t value);
int modem_irq_pending(void); /* UART IRQ output, including MCR.OUT2 gating. */

/* BIOS INT 14h functions 0..3, returning AX. Port 0 is COM1. Empty receive
 * and busy transmit return the BIOS timeout bit rather than blocking the
 * dispatcher; direct UART users receive exactly the same bytes and status. */
uint16_t modem_bios(unsigned function, uint8_t value, unsigned port);

#endif
